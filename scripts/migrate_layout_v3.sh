#!/usr/bin/env bash
# Flash layout v2 -> v3 over USB, once: the c6fw partition shrinks from 2 MB to 64 kB (directory and
# the small C6 bundles; the large firmwares come from the package in PSRAM) and the model partition
# grows to 6 MB. NVS (settings, Thread network, users), the gateway slots, ESPHome and the learned
# sounds stay; the Zigbee snapshots (c6data) move; the model is written again.
#
# Before: the gateway runs firmware v1.6 or later (online update or OTA), scripts/build_gateway.sh
# built the v3 partition table, dist/ holds c6fw.bin and yamnet_p4.espdl.
# Usage: migrate_layout_v3.sh <port>
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?usage: migrate_layout_v3.sh <port>}
table=build/gateway/partition_table/partition-table.bin
for f in "$table" dist/c6fw.bin dist/yamnet_p4.espdl; do
    [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
done
# CH340 reads are reliable only at 115200 Bd on macOS; writes are checked by esptool (hash)
rd() { esptool --chip esp32p4 --port "$port" -b 115200 read-flash "$@"; }
wr() { esptool --chip esp32p4 --port "$port" -b 460800 write-flash --flash-mode dio --flash-size 16MB "$@"; }

mkdir -p backups
stamp=$(date +%Y%m%d-%H%M%S)
rd 0x8000 0x1000 "backups/ptable_$stamp.bin"
layout() {
    python3 - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
for i in range(0, len(data), 32):
    magic, ptype, sub, off, size, label = struct.unpack_from('<HBBII16s', data, i)
    if magic != 0x50AA:
        break
    if label.rstrip(b'\0') == b'c6fw':
        print('v3' if size == 0x10000 else 'v2' if size == 0x200000 else 'unknown')
        break
PY
}
case "$(layout "backups/ptable_$stamp.bin")" in
    v3) echo "the board already has layout v3"; exit 0 ;;
    v2) ;;
    *) echo "unexpected partition table, nothing written (backups/ptable_$stamp.bin)" >&2; exit 1 ;;
esac
[ "$(layout "$table")" = v3 ] || { echo "$table is not layout v3, run scripts/build_gateway.sh" >&2; exit 1; }

echo "== backing up NVS and the Zigbee snapshots"
rd 0x9000 0x14000 "backups/nvs_$stamp.bin"
rd 0xa20000 0x10000 "backups/c6data_v2_$stamp.bin"
python3 scripts/c6fw_local.py dist/c6fw.bin dist/c6fw-local.bin --size 0x10000

echo "== writing layout v3"
wr 0x8000 "$table" 0x820000 dist/c6fw-local.bin 0x830000 "backups/c6data_v2_$stamp.bin" 0x840000 dist/yamnet_p4.espdl
echo "done: the gateway restarts with layout v3 (backups in backups/*_$stamp.bin)"
