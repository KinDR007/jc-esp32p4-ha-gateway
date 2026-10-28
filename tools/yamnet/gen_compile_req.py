"""Regenerates firmware/gateway/components/sound/yamnet.compile_req.yml from the esp-dl kernel list
(keeps only int8 kernels) and the op types of the exported ONNX graph.

    .venv/bin/python gen_compile_req.py
"""
import os
import re

import onnx

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SPEC = os.path.join(ROOT, 'firmware/gateway/managed_components/espressif__esp-dl/spec/kernels.yml')
OUT = os.path.join(ROOT, 'firmware/gateway/components/sound/yamnet.compile_req.yml')


def main():
    text = open(SPEC).read()
    abi = int(re.search(r'^kernel_abi:\s*(\d+)', text, re.M).group(1))
    symbols = [l.strip()[2:] for l in text.splitlines() if l.strip().startswith('- ')]
    kernels = [s for s in symbols if '_s8_' in s and 'w8a16' not in s]
    model = onnx.load(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'out/yamnet_p4.onnx'))
    # RequantizeLinear is used by the tensor runtime itself, not only by graph nodes.
    # Constant nodes are folded into their consumers by the ESP-DL export.
    ops = sorted(({n.op_type for n in model.graph.node} - {'Constant'}) | {'RequantizeLinear'})
    lines = ['# esp-dl compile-time op/kernel selection for the YAMNet int8 model (firmware/gateway/components/sound).',
             '# Ops from the exported graph; kernels: every int8 (s8) kernel, so any int8 conv shape still has one.',
             '# Regenerate the kernel list after an esp-dl update: tools/yamnet/gen_compile_req.py.',
             f'kernel_abi: {abi}', 'ops:'] + [f'  - {o}' for o in ops] + ['kernels:'] + [f'  - {k}' for k in kernels]
    with open(OUT, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    print(f'{OUT}: {len(ops)} ops, {len(kernels)} kernels')


if __name__ == '__main__':
    main()
