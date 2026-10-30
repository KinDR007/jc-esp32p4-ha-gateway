#!/usr/bin/env python3
"""The part of a C6 package (c6fw.bin) the gateway keeps in a small c6fw partition: the directory
and the small bundles (slot choices); the large bundles are marked remote (offset 0xFFFFFFFF) and
come from the package in PSRAM when the C6 is flashed. Same layout as pkg_store() in
firmware/gateway/components/c6_flasher/c6_flasher.c.

    scripts/c6fw_local.py dist/c6fw.bin dist/c6fw-local.bin --size 0x10000
"""

import argparse
import struct
import zlib

ENTRY = struct.Struct("<16s32sII32s")
REMOTE = 0xFFFFFFFF
LOCAL_MAX_BUNDLE = 32 * 1024
BLOCK = 4096


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("package")
    p.add_argument("out")
    p.add_argument("--size", type=lambda v: int(v, 0), required=True, help="size of the c6fw partition")
    a = p.parse_args()
    pkg = open(a.package, "rb").read()
    magic, fmt, count = struct.unpack_from("<4sII", pkg, 0)
    if magic != b"C6FW":
        raise SystemExit(f"{a.package}: not a c6fw package")
    if len(pkg) <= a.size:
        open(a.out, "wb").write(pkg)  # fits whole
        return
    img = bytearray(a.size)
    dir_len = 12 + 8 * ENTRY.size  # header, MAX_ENTRIES entries, crc follows
    img[:dir_len] = pkg[:dir_len]
    cursor = BLOCK
    for i in range(count):
        off = 12 + i * ENTRY.size
        name, version, offset, size, sha = ENTRY.unpack_from(pkg, off)
        if size <= LOCAL_MAX_BUNDLE and cursor + size <= a.size:
            img[cursor:cursor + size] = pkg[offset:offset + size]
            offset = cursor
            cursor = (cursor + size + 15) & ~15
        else:
            offset = REMOTE
        ENTRY.pack_into(img, off, name, version, offset, size, sha)
    struct.pack_into("<I", img, dir_len, zlib.crc32(bytes(img[:dir_len])) & 0xFFFFFFFF)
    used = (cursor + BLOCK - 1) // BLOCK * BLOCK
    open(a.out, "wb").write(bytes(img[:used]))
    print(f"{a.out}: {used} B, {sum(1 for i in range(count) if ENTRY.unpack_from(img, 12 + i * ENTRY.size)[2] == REMOTE)} remote bundles")


if __name__ == "__main__":
    main()
