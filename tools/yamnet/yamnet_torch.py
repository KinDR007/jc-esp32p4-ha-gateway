"""YAMNet (AudioSet, 521 classes) in PyTorch with the official Keras weights.

Mirrors tensorflow/models research/audioset/yamnet (yamnet.py, features.py, params.py):
log-mel patches of 96 x 64 (25 ms window, 10 ms hop, 64 mel bins 125-7500 Hz) go through
a MobileNetV1 trunk; the head gives a 1024-d embedding and 521 sigmoid scores.
TensorFlow 'same' padding is asymmetric for stride 2, reproduced with explicit pads.
"""
import csv
import math

import h5py
import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F

SAMPLE_RATE = 16000
STFT_WINDOW = 400  # 25 ms
STFT_HOP = 160  # 10 ms
FFT_LENGTH = 512
MEL_BANDS = 64
MEL_MIN_HZ = 125.0
MEL_MAX_HZ = 7500.0
LOG_OFFSET = 0.001
PATCH_FRAMES = 96  # 0.96 s
PATCH_HOP_FRAMES = 48  # 0.48 s
BN_EPSILON = 1e-4

# (kind, stride, filters) as in yamnet.py _YAMNET_LAYER_DEFS
LAYERS = [('conv', 2, 32), ('sep', 1, 64), ('sep', 2, 128), ('sep', 1, 128), ('sep', 2, 256),
          ('sep', 1, 256), ('sep', 2, 512), ('sep', 1, 512), ('sep', 1, 512), ('sep', 1, 512),
          ('sep', 1, 512), ('sep', 1, 512), ('sep', 2, 1024), ('sep', 1, 1024)]


# ---------------------------------------------------------------- features

def _hertz_to_mel(f):
    return 1127.0 * np.log1p(np.asarray(f, dtype=np.float64) / 700.0)


def mel_matrix():
    """tf.signal.linear_to_mel_weight_matrix(64, 257, 16000, 125, 7500), DC band zeroed."""
    bins = FFT_LENGTH // 2 + 1
    linear_mel = _hertz_to_mel(np.linspace(0.0, SAMPLE_RATE / 2, bins))[1:]
    edges = np.linspace(_hertz_to_mel(MEL_MIN_HZ), _hertz_to_mel(MEL_MAX_HZ), MEL_BANDS + 2)
    lower, center, upper = edges[:-2], edges[1:-1], edges[2:]
    lm = linear_mel[:, None]
    lower_slopes = (lm - lower) / (center - lower)
    upper_slopes = (upper - lm) / (upper - center)
    weights = np.maximum(0.0, np.minimum(lower_slopes, upper_slopes))
    return np.pad(weights, [[1, 0], [0, 0]]).astype(np.float32)  # [257, 64]


def log_mel(waveform):
    """float waveform in [-1, 1] at 16 kHz -> log-mel frames [n, 64] (float32)."""
    waveform = np.asarray(waveform, dtype=np.float32)
    n = 1 + (len(waveform) - STFT_WINDOW) // STFT_HOP
    if n <= 0:
        return np.zeros((0, MEL_BANDS), np.float32)
    window = (0.5 - 0.5 * np.cos(2 * np.pi * np.arange(STFT_WINDOW) / STFT_WINDOW)).astype(np.float32)
    idx = np.arange(STFT_WINDOW)[None, :] + STFT_HOP * np.arange(n)[:, None]
    frames = waveform[idx] * window
    spec = np.abs(np.fft.rfft(frames, FFT_LENGTH)).astype(np.float32)
    return np.log(spec @ mel_matrix() + LOG_OFFSET).astype(np.float32)


def patches(mel):
    """[n, 64] -> [k, 96, 64] with a hop of 48 frames."""
    if len(mel) < PATCH_FRAMES:
        return np.zeros((0, PATCH_FRAMES, MEL_BANDS), np.float32)
    starts = range(0, len(mel) - PATCH_FRAMES + 1, PATCH_HOP_FRAMES)
    return np.stack([mel[s:s + PATCH_FRAMES] for s in starts])


# ---------------------------------------------------------------- model

class TfSamePad(nn.Module):
    """TensorFlow 'same' padding for a 3x3 kernel (extra row/column at the end for stride 2)."""

    def __init__(self, stride):
        super().__init__()
        self.pad = (1, 1, 1, 1) if stride == 1 else (0, 1, 0, 1)

    def forward(self, x):
        return F.pad(x, self.pad)


def _conv_bn_relu(cin, cout, kernel, stride, groups=1):
    layers = []
    if kernel == 3:
        layers.append(TfSamePad(stride))
    layers += [nn.Conv2d(cin, cout, kernel, stride, 0, groups=groups, bias=False),
               nn.BatchNorm2d(cout, eps=BN_EPSILON, affine=True),
               nn.ReLU()]
    return layers


class YAMNet(nn.Module):
    def __init__(self):
        super().__init__()
        blocks = []
        cin = 1
        for kind, stride, cout in LAYERS:
            if kind == 'conv':
                blocks.append(nn.Sequential(*_conv_bn_relu(cin, cout, 3, stride)))
            else:
                blocks.append(nn.Sequential(*_conv_bn_relu(cin, cin, 3, stride, groups=cin),
                                            *_conv_bn_relu(cin, cout, 1, 1)))
            cin = cout
        self.trunk = nn.Sequential(*blocks)
        self.logits = nn.Linear(1024, 521)

    def embedding(self, x):
        """x: [b, 1, 96, 64] log-mel patches -> [b, 1024]."""
        return self.trunk(x).mean(dim=(2, 3))

    def forward(self, x):
        """x: [b, 1, 96, 64] -> (scores [b, 521], embedding [b, 1024])."""
        emb = self.embedding(x)
        return torch.sigmoid(self.logits(emb)), emb


def load_keras_weights(model, path):
    f = h5py.File(path, 'r')

    def get(name):
        return torch.from_numpy(np.array(f[name]))

    def load_bn(bn, prefix):
        # Keras BN with center=True, scale=False: gamma is 1.
        bn.weight.data.fill_(1.0)
        bn.bias.data.copy_(get(f'{prefix}/beta:0'))
        bn.running_mean.data.copy_(get(f'{prefix}/moving_mean:0'))
        bn.running_var.data.copy_(get(f'{prefix}/moving_variance:0'))

    for i, (kind, _, _) in enumerate(LAYERS, start=1):
        block = model.trunk[i - 1]
        convs = [m for m in block if isinstance(m, nn.Conv2d)]
        bns = [m for m in block if isinstance(m, nn.BatchNorm2d)]
        if kind == 'conv':
            p = f'layer{i}/conv'
            convs[0].weight.data.copy_(get(f'{p}/layer{i}/conv/kernel:0').permute(3, 2, 0, 1))
            load_bn(bns[0], f'{p}/bn/layer{i}/conv/bn')
        else:
            p = f'layer{i}/depthwise_conv'
            # depthwise kernel [3, 3, cin, 1] -> [cin, 1, 3, 3]
            convs[0].weight.data.copy_(get(f'{p}/layer{i}/depthwise_conv/depthwise_kernel:0').permute(2, 3, 0, 1))
            load_bn(bns[0], f'{p}/bn/layer{i}/depthwise_conv/bn')
            p = f'layer{i}/pointwise_conv'
            convs[1].weight.data.copy_(get(f'{p}/layer{i}/pointwise_conv/kernel:0').permute(3, 2, 0, 1))
            load_bn(bns[1], f'{p}/bn/layer{i}/pointwise_conv/bn')
    model.logits.weight.data.copy_(get('logits/logits/kernel:0').T)
    model.logits.bias.data.copy_(get('logits/logits/bias:0'))
    return model.eval()


def class_names(path):
    with open(path) as f:
        return [row['display_name'] for row in csv.DictReader(f)]


def load(weights='data/yamnet.h5'):
    return load_keras_weights(YAMNet(), weights)


def classify(model, waveform):
    """Mean scores over all patches of a clip -> np.ndarray [521]."""
    p = patches(log_mel(waveform))
    if len(p) == 0:
        raise ValueError('clip shorter than 0.96 s')
    with torch.no_grad():
        scores, _ = model(torch.from_numpy(p)[:, None])
    return scores.numpy().mean(axis=0)


def num_params(model):
    return sum(p.numel() for p in model.parameters())


def macs():
    """Multiply-accumulates per patch (convolutions + logits)."""
    h, w, cin, total = 96, 64, 1, 0
    for kind, stride, cout in LAYERS:
        h, w = math.ceil(h / stride), math.ceil(w / stride)
        if kind == 'conv':
            total += h * w * 9 * cin * cout
        else:
            total += h * w * 9 * cin + h * w * cin * cout
        cin = cout
    return total + 1024 * 521
