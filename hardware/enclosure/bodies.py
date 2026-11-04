#!/usr/bin/env python3
"""Counts the separate bodies of an ASCII or binary STL (a printed part must be one): bodies.py part.stl"""
import struct
import sys


def triangles(path):
    data = open(path, 'rb').read()
    if data[:5] == b'solid' and b'facet' in data[:300]:
        tri = []
        for line in data.decode().splitlines():
            line = line.split()
            if line and line[0] == 'vertex':
                tri.append(tuple(round(float(v), 4) for v in line[1:]))
                if len(tri) == 3:
                    yield tri
                    tri = []
        return
    n = struct.unpack('<I', data[80:84])[0]
    for i in range(n):
        v = struct.unpack('<12f', data[84 + 50 * i:84 + 50 * i + 48])
        yield [tuple(round(x, 4) for x in v[3 + 3 * k:6 + 3 * k]) for k in range(3)]


def bodies(path):
    parent = {}

    def find(a):
        while parent.setdefault(a, a) != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    for a, b, c in triangles(path):
        ra = find(a)
        for v in (b, c):
            parent[find(v)] = ra
    return len({find(v) for v in parent})


if __name__ == '__main__':
    n = bodies(sys.argv[1])
    print(f'{sys.argv[1]}: {n} bodies')
    sys.exit(0 if n == 1 else 1)
