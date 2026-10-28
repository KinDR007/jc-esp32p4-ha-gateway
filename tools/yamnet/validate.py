"""Sanity check of the PyTorch YAMNet on ESC-50 clips (category id is the last field of the name)."""
import glob
import os
import sys

import numpy as np
import soundfile as sf
from scipy.signal import resample_poly

import yamnet_torch as yt

ESC50 = {0: 'dog', 1: 'rooster', 6: 'hen', 7: 'insects', 10: 'rain', 14: 'chirping birds', 19: 'thunderstorm',
         22: 'clapping', 26: 'laughing', 29: 'drinking', 32: 'keyboard typing', 40: 'helicopter',
         43: 'chirping birds', 48: 'fireworks', 49: 'hand saw'}


def load_16k(path):
    audio, sr = sf.read(path, dtype='float32', always_2d=True)
    audio = audio.mean(axis=1)
    if sr != yt.SAMPLE_RATE:
        g = np.gcd(sr, yt.SAMPLE_RATE)
        audio = resample_poly(audio, yt.SAMPLE_RATE // g, sr // g).astype(np.float32)
    return audio


def main():
    model = yt.load()
    names = yt.class_names('data/yamnet_class_map.csv')
    print(f'params {yt.num_params(model) / 1e6:.2f} M, MACs/patch {yt.macs() / 1e6:.1f} M')
    for path in sorted(glob.glob(sys.argv[1] if len(sys.argv) > 1 else 'data/esc50/*.wav')):
        cat = int(os.path.basename(path).rsplit('-', 1)[1].split('.')[0])
        scores = yt.classify(model, load_16k(path))
        top = np.argsort(scores)[::-1][:3]
        print(f'{ESC50.get(cat, cat):16} -> ' + ', '.join(f'{names[i]} {scores[i]:.2f}' for i in top))


if __name__ == '__main__':
    main()
