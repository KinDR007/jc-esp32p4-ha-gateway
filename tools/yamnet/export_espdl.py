"""Quantize YAMNet to an ESP-DL model for the ESP32-P4.

Input:  log-mel patch [1, 1, 96, 64] (float, quantized on device by ESP-DL)
Output: logits [1, 521] (sigmoid on device) and embedding [1, 1024] (learning mode)

Calibration uses log-mel patches of the WAV files given (ESC-50 clips, later real recordings
from the board). Also writes the int8 error report and a float reference for the device test.

    .venv/bin/python export_espdl.py --out out/yamnet_p4.espdl data/esc50/*.wav
"""
import argparse
import os

import numpy as np
import torch
from torch.utils.data import DataLoader, TensorDataset

import yamnet_torch as yt
from validate import load_16k


LOGIT_CLIP = 16.0  # sigmoid(+-16) is 0/1 within 1e-7


class Export(torch.nn.Module):
    """Logits instead of sigmoid scores: the sigmoid runs in float on the device.

    Float logits span about -611..+51 (rare classes are very negative), which gives a per-tensor
    power-of-2 int8 step of 4 and scores of only 0.02 or 0.5. Clipping to +-16 gives a step of 0.125.
    """

    def __init__(self, model):
        super().__init__()
        self.model = model

    def forward(self, x):
        emb = self.model.embedding(x)
        return torch.clamp(self.model.logits(emb), -LOGIT_CLIP, LOGIT_CLIP), emb


def calibration_patches(paths):
    data = []
    for path in paths:
        p = yt.patches(yt.log_mel(load_16k(path)))
        data.extend(p)
    return torch.from_numpy(np.stack(data))[:, None]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--out', default='out/yamnet_p4.espdl')
    ap.add_argument('--bits', type=int, default=8)
    ap.add_argument('wavs', nargs='+')
    args = ap.parse_args()
    os.makedirs(os.path.dirname(args.out), exist_ok=True)

    from esp_ppq.api import espdl_quantize_torch

    model = Export(yt.load()).eval()
    calib = calibration_patches(args.wavs)
    print(f'calibration patches: {len(calib)}')
    loader = DataLoader(TensorDataset(calib), batch_size=1, shuffle=False)
    espdl_quantize_torch(
        model=model,
        espdl_export_file=args.out,
        calib_dataloader=loader,
        calib_steps=min(len(calib), 256),
        input_shape=[1, 1, yt.PATCH_FRAMES, yt.MEL_BANDS],
        target='esp32p4',
        num_of_bits=args.bits,
        collate_fn=lambda batch: batch[0].to('cpu'),
        device='cpu',
        error_report=True,
        export_test_values=True,
        verbose=1,
    )
    print(f'{args.out}: {os.path.getsize(args.out)} B')


if __name__ == '__main__':
    main()
