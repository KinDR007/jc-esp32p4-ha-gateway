"""Compare ESP-DL quantization variants of YAMNet against the float model.

Metrics on all patches of the given clips: cosine similarity of logits and embeddings,
and top-5 class agreement (patches whose float top class is not 'Silence').

    .venv/bin/python quant_experiments.py data/esc50/*.wav
"""
import sys

import numpy as np
import torch
from torch.utils.data import DataLoader, TensorDataset

import yamnet_torch as yt
from export_espdl import Export, calibration_patches


def variants():
    from esp_ppq.api import QuantizationSettingFactory
    from esp_ppq.core import TargetPlatform

    def base():
        return QuantizationSettingFactory.espdl_setting()

    def eq():
        s = base()
        s.equalization = True
        s.equalization_setting.iterations = 4
        s.equalization_setting.value_threshold = 0.4
        s.equalization_setting.opt_level = 2
        s.equalization_setting.including_bias = True
        return s

    def eq_bc():
        s = eq()
        s.bias_correct = True
        return s

    late16 = {f'/trunk/trunk.{b}/trunk.{b}.{i}/Conv': TargetPlatform.ESPDL_INT16 for b in (12, 13) for i in (1, 4)}
    late16['/logits/Gemm'] = TargetPlatform.ESPDL_INT16
    return [
        ('int8', base(), 8, None),
        ('int8+eq', eq(), 8, None),
        ('int8+eq+bc', eq_bc(), 8, None),
        ('int8+eq, late int16', eq(), 8, late16),
        ('int16', base(), 16, None),
    ]


def run_graph(graph, x):
    from esp_ppq.executor import TorchExecutor
    ex = TorchExecutor(graph=graph, device='cpu')
    logits, emb = [], []
    for i in range(len(x)):
        out = ex.forward(inputs=x[i:i + 1])
        logits.append(out[0].detach().numpy())
        emb.append(out[1].detach().numpy())
    return np.concatenate(logits), np.concatenate(emb)


def cosine(a, b):
    return float(np.mean(np.sum(a * b, 1) / (np.linalg.norm(a, axis=1) * np.linalg.norm(b, axis=1) + 1e-9)))


def main():
    from esp_ppq.api import espdl_quantize_torch

    x = calibration_patches(sys.argv[1:])
    model = Export(yt.load()).eval()
    with torch.no_grad():
        ref_logits, ref_emb = (t.numpy() for t in model(x))
    silence = 500
    keep = np.argmax(ref_logits, 1) != silence
    ref_top5 = np.argsort(ref_logits, 1)[:, ::-1][:, :5]
    loader = DataLoader(TensorDataset(x), batch_size=1)
    print(f'{len(x)} patches, {keep.sum()} non-silent')
    for name, setting, bits, override in variants():
        graph = espdl_quantize_torch(
            model=model, espdl_export_file='out/exp.espdl', calib_dataloader=loader,
            calib_steps=min(len(x), 256), input_shape=[1, 1, 96, 64], target='esp32p4',
            num_of_bits=bits, collate_fn=lambda b: b[0], setting=setting, dispatching_override=override,
            device='cpu', error_report=False, skip_export=True, verbose=0)
        q_logits, q_emb = run_graph(graph, x)
        top1 = np.argmax(q_logits, 1)
        agree = np.mean([top1[i] in ref_top5[i] for i in np.where(keep)[0]])
        same_top1 = np.mean(top1[keep] == np.argmax(ref_logits, 1)[keep])
        print(f'{name:22} logits cos {cosine(q_logits, ref_logits):.4f}  emb cos {cosine(q_emb, ref_emb):.4f}  '
              f'top1 same {same_top1:.2f}  top1 in float top5 {agree:.2f}', flush=True)


if __name__ == '__main__':
    main()
