#!/usr/bin/env python3
"""Pack C6 firmware builds into one c6fw partition image.

Layout (little endian, must match firmware/gateway/components/c6_flasher/c6fw_format.h):
  directory at offset 0 (4 KiB): magic "C6FW", format version, entry count,
      entries[MAX_ENTRIES] {name[16], version[32], offset, size, sha256[32]}, crc32 of all previous bytes
  bundle at a 4 KiB aligned offset: magic "C6BN", segment count,
      segments {addr, size, data_off, stored_size, flags, md5[16]}, then segment data (4 byte aligned)

Segment flags: WRITE writes data, ERASE erases the region, PRESERVE marks a region whose
content the gateway snapshots before leaving this firmware and restores when it returns,
DEFLATE means the stored data is a zlib stream (stored_size bytes) that the C6 ROM loader
inflates while writing; size and md5 always describe the uncompressed data.

Slots layout (--slots): the bundle "slots" installs bootloader, partition table (--layout) and
the listed firmwares in ota_0, ota_1, ... at once; each listed bundle then only writes otadata
for its slot (plus PRESERVE of the Zigbee data) and carries a SLOTS marker segment whose md5
field holds the first 16 bytes of the slots bundle sha256. A role switch writes 8 kB instead
of a whole firmware. Bundles not listed keep their own whole-flash layout.
"""
import argparse
import binascii
import hashlib
import os
import struct
import subprocess
import sys
import tempfile
import zlib

DIR_MAGIC = b'C6FW'
BUNDLE_MAGIC = b'C6BN'
FORMAT_VERSION = 3
MAX_ENTRIES = 8
ENTRY_FMT = '<16s32sII32s'
SEG_FMT = '<IIIII16s'
SEG_WRITE, SEG_ERASE, SEG_PRESERVE, SEG_DEFLATE, SEG_SLOTS = 1, 2, 4, 8, 16
SLOTS_BUNDLE = 'slots'
ALIGN = 4096
APP_DESC_MAGIC = 0xABCD5432
# Wiped on every switch: nvs/otadata/phy of all supported layouts lie in 0x9000..0x10000.
SCRATCH_ERASE = (0x9000, 0x7000)
PRESERVE_PARTITIONS = ('zb_storage', 'zb_fct')


def align(value, to):
    return (value + to - 1) // to * to


def parse_flash_args(build_dir):
    with open(os.path.join(build_dir, 'flash_args')) as f:
        lines = f.read().split('\n')[1:]
    files = []
    for line in lines:
        if line.strip():
            addr, path = line.split()
            files.append((int(addr, 0), os.path.join(build_dir, path)))
    return files


def parse_partition_table(path):
    return parse_partition_table_bytes(open(path, 'rb').read())


def parse_partition_table_bytes(data):
    parts = []
    for i in range(0, len(data), 32):
        entry = data[i:i + 32]
        if entry[:2] != b'\xaa\x50':
            break
        offset, size = struct.unpack('<II', entry[4:12])
        parts.append((entry[12:28].rstrip(b'\0').decode(), offset, size))
    return parts


def app_version(app_path):
    data = open(app_path, 'rb').read(256)
    magic, = struct.unpack('<I', data[32:36])
    if magic != APP_DESC_MAGIC:
        sys.exit(f'{app_path}: no esp_app_desc_t')
    version = data[48:80].rstrip(b'\0').decode()
    project = data[80:112].rstrip(b'\0').decode()
    return f'{project} {version}'[:31]


def build_files(build_dir):
    files = parse_flash_args(build_dir)
    pt = next(p for a, p in files if 'partition' in p)
    app = max((p for a, p in files if 'bootloader' not in p and 'partition' not in p and 'ota_data' not in p),
              key=os.path.getsize)
    return files, pt, app


def build_bundle(build_dir):
    files, pt, app = build_files(build_dir)
    segments = [(SCRATCH_ERASE[0], SCRATCH_ERASE[1], b'', SEG_ERASE)]
    for addr, path in sorted(files):
        segments.append((addr, os.path.getsize(path), open(path, 'rb').read(), SEG_WRITE | SEG_DEFLATE))
    for name, offset, size in parse_partition_table(pt):
        if name in PRESERVE_PARTITIONS:
            segments.append((offset, size, b'', SEG_PRESERVE))
    return encode_bundle(segments), app_version(app)


def otadata(slot):
    """otadata selecting ota_<slot> (see components/app_update/otatool.py), second copy empty."""
    seq = struct.pack('<I', slot + 1)
    entry = seq + b'\xff' * 24 + struct.pack('<I', binascii.crc32(seq, 0xFFFFFFFF) & 0xFFFFFFFF)
    return entry + b'\xff' * (0x2000 - len(entry))


def build_slots(layout_csv, bootloader_dir, slot_dirs):
    """The slots bundle and, per firmware, the bundle that only selects its slot."""
    with tempfile.TemporaryDirectory() as tmp:
        pt = os.path.join(tmp, 'slots.bin')
        subprocess.run([sys.executable, os.path.join(os.environ['IDF_PATH'], 'components/partition_table/gen_esp32part.py'),
                        layout_csv, pt], check=True, capture_output=True)
        table = open(pt, 'rb').read()
    parts = {name: (offset, size) for name, offset, size in parse_partition_table_bytes(table)}
    boot = os.path.join(bootloader_dir, 'bootloader', 'bootloader.bin')
    segments = [(parts['nvs'][0], parts['nvs'][1], b'', SEG_ERASE), (parts['phy_init'][0], parts['phy_init'][1], b'', SEG_ERASE),
                (0, os.path.getsize(boot), open(boot, 'rb').read(), SEG_WRITE | SEG_DEFLATE),
                (0x8000, len(table), table, SEG_WRITE | SEG_DEFLATE)]
    apps = {}
    for slot, (name, build_dir) in enumerate(slot_dirs):
        files, own_pt, app = build_files(build_dir)
        offset, size = parts[f'ota_{slot}']
        data = open(app, 'rb').read()
        if len(data) > size:
            sys.exit(f'{name}: {len(data)} B does not fit ota_{slot} ({size} B)')
        segments.append((offset, len(data), data, SEG_WRITE | SEG_DEFLATE))
        apps[name] = (slot, app, [p for p, _, _ in parse_partition_table(own_pt)])
    segments.append((parts['otadata'][0], 0x2000, otadata(0), SEG_WRITE | SEG_DEFLATE))
    base = encode_bundle(segments)
    marker = hashlib.sha256(base).digest()[:16]
    bundles = {SLOTS_BUNDLE: (base, f'{len(slot_dirs)} firmware slots')}
    for name, (slot, app, own_parts) in apps.items():
        segs = [(parts['otadata'][0], 0x2000, otadata(slot), SEG_WRITE | SEG_DEFLATE), (0, 0, b'', SEG_SLOTS, marker)]
        segs += [(parts[p][0], parts[p][1], b'', SEG_PRESERVE) for p in PRESERVE_PARTITIONS if p in own_parts]
        bundles[name] = (encode_bundle(segs), app_version(app))
    return bundles


def encode_bundle(segments):
    header_size = 8 + len(segments) * struct.calcsize(SEG_FMT)
    data_off = align(header_size, 4)
    header = bytearray(BUNDLE_MAGIC + struct.pack('<I', len(segments)))
    payload = bytearray()
    for addr, size, data, flags, *md5_override in segments:
        off = data_off + len(payload) if data else 0
        md5 = md5_override[0] if md5_override else hashlib.md5(data).digest() if data else bytes(16)
        stored = zlib.compress(data, 9) if flags & SEG_DEFLATE else data
        header += struct.pack(SEG_FMT, addr, size, off, len(stored), flags, md5)
        if stored:
            payload += stored + b'\xff' * (align(len(stored), 4) - len(stored))
    header += b'\xff' * (data_off - len(header))
    return bytes(header + payload)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--out', required=True)
    parser.add_argument('--size', type=lambda v: int(v, 0), required=True, help='c6fw partition size')
    parser.add_argument('--slots', default='', help='comma separated bundles sharing one layout, in slot order')
    parser.add_argument('--layout', help='partition table CSV of the slots layout')
    parser.add_argument('--slots-bootloader', help='build dir whose bootloader the slots layout uses')
    parser.add_argument('bundles', nargs='+', metavar='NAME=BUILD_DIR')
    args = parser.parse_args()
    specs = [spec.split('=', 1) for spec in args.bundles]
    slot_names = [n for n in args.slots.split(',') if n]
    if len(specs) + (1 if slot_names else 0) > MAX_ENTRIES:
        sys.exit('too many bundles')
    if any(len(name) >= 16 for name, _ in specs):
        sys.exit('bundle name too long')

    built = {}
    if slot_names:
        dirs = dict(specs)
        built = build_slots(args.layout, args.slots_bootloader, [(n, dirs[n]) for n in slot_names])
    image = bytearray(b'\xff' * ALIGN)
    entries = []
    for name, build_dir in ([(SLOTS_BUNDLE, None)] if slot_names else []) + specs:
        bundle, version = built[name] if name in built else build_bundle(build_dir)
        offset = len(image)
        image += bundle + b'\xff' * (align(len(bundle), ALIGN) - len(bundle))
        entries.append((name, version, offset, len(bundle), hashlib.sha256(bundle).digest()))
        print(f'{name:12} {version:32} offset 0x{offset:06x} size {len(bundle)}')

    if len(image) > args.size:
        sys.exit(f'image {len(image)} B exceeds partition {args.size} B')

    directory = bytearray(DIR_MAGIC + struct.pack('<II', FORMAT_VERSION, len(entries)))
    for i in range(MAX_ENTRIES):
        if i < len(entries):
            name, version, offset, size, sha = entries[i]
            directory += struct.pack(ENTRY_FMT, name.encode(), version.encode(), offset, size, sha)
        else:
            directory += bytes(struct.calcsize(ENTRY_FMT))
    directory += struct.pack('<I', zlib.crc32(directory))
    image[:len(directory)] = directory

    with open(args.out, 'wb') as f:
        f.write(image)
    print(f'{args.out}: {len(image)} B of {args.size} B')


if __name__ == '__main__':
    main()
