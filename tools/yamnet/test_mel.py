"""Builds the device log-mel code (firmware/gateway/components/sound/sound_mel.c) for the host and
compares it with the Python reference on real clips. Fails when the difference could change the
model input noticeably.

    .venv/bin/python test_mel.py data/esc50/*.wav
"""
import ctypes
import os
import subprocess
import sys
import tempfile

import numpy as np

import yamnet_torch as yt
from validate import load_16k

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SRC = os.path.join(ROOT, 'firmware', 'gateway', 'components', 'sound', 'sound_mel.c')


def build():
    lib = os.path.join(tempfile.mkdtemp(), 'libmel.so')
    subprocess.check_call(['cc', '-O2', '-shared', '-fPIC', '-o', lib, SRC, '-lm'])
    so = ctypes.CDLL(lib)
    so.mel_init()
    so.mel_frame.argtypes = [ctypes.POINTER(ctypes.c_float), ctypes.POINTER(ctypes.c_float)]
    return so


def device_log_mel(so, wave):
    n = 1 + (len(wave) - yt.STFT_WINDOW) // yt.STFT_HOP
    out = np.zeros((n, yt.MEL_BANDS), np.float32)
    for i in range(n):
        frame = np.ascontiguousarray(wave[i * yt.STFT_HOP:i * yt.STFT_HOP + yt.STFT_WINDOW], np.float32)
        so.mel_frame(frame.ctypes.data_as(ctypes.POINTER(ctypes.c_float)),
                     out[i].ctypes.data_as(ctypes.POINTER(ctypes.c_float)))
    return out


def main():
    so = build()
    worst = 0.0
    for path in sys.argv[1:]:
        wave = load_16k(path)
        ref = yt.log_mel(wave)
        dev = device_log_mel(so, wave)
        err = float(np.max(np.abs(ref - dev)))
        worst = max(worst, err)
        print(f'{os.path.basename(path):24} frames {len(ref):4}  max |diff| {err:.2e}')
    print(f'worst {worst:.2e}')
    return 0 if worst < 1e-3 else 1


if __name__ == '__main__':
    sys.exit(main())
