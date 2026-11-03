#!/usr/bin/env bash
# Flash layout v3 -> v4 over USB, once: the esphome slot (unused since v1.8) goes to the gateway slots,
# 3 -> 4 MB each. gw_a and everything from c6fw on keep their offsets, so NVS (settings, Thread network,
# users), the Zigbee snapshots, the model and the learned sounds stay. The gateway built by
# scripts/build_gateway.sh is written to gw_a and otadata is cleared, so the board starts from gw_a;
# gw_b moves and is filled by the next update.
#
# Before: scripts/build_gateway.sh built the v4 partition table.
# Usage: migrate_layout_v4.sh <port>
# Back to v3: migrate_layout_v4.sh <port> --rollback <stamp>   (stamp of the backups/*_<stamp>.bin)
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?usage: migrate_layout_v4.sh <port> [--rollback <stamp>]}
table=build/gateway/partition_table/partition-table.bin
app=build/gateway/p4gw.bin
# CH340 reads are reliable only at 115200 Bd on macOS; writes are checked by esptool (hash)
rd() { esptool --chip esp32p4 --port "$port" -b 115200 read-flash "$@"; }
wr() { esptool --chip esp32p4 --port "$port" -b 460800 write-flash --flash-mode dio --flash-size 16MB "$@"; }

if [ "${2:-}" = "--rollback" ]; then
    stamp=${3:?usage: migrate_layout_v4.sh <port> --rollback <stamp>}
    for f in "backups/ptable_$stamp.bin" "backups/otadata_$stamp.bin"; do
        [ -f "$f" ] || { echo "missing $f" >&2; exit 1; }
    done
    echo "== writing the v3 partition table and otadata of $stamp back"
    wr 0x8000 "backups/ptable_$stamp.bin" 0x1d000 "backups/otadata_$stamp.bin"
    echo "done: the gateway restarts with layout v3"
    exit 0
fi

for f in "$table" "$app"; do
    [ -f "$f" ] || { echo "missing $f, run scripts/build_gateway.sh" >&2; exit 1; }
done
layout() {
    python3 - "$1" <<'PY'
import struct, sys
data = open(sys.argv[1], 'rb').read()
parts = {}
for i in range(0, len(data), 32):
    magic, ptype, sub, off, size, label = struct.unpack_from('<HBBII16s', data, i)
    if magic != 0x50AA:
        break
    parts[label.rstrip(b'\0').decode()] = (off, size)
gw_a, c6fw = parts.get('gw_a'), parts.get('c6fw')
if gw_a == (0x20000, 0x400000) and 'esphome' not in parts:
    print('v4')
elif gw_a == (0x20000, 0x300000) and c6fw == (0x820000, 0x10000):
    print('v3')
else:
    print('unknown')
PY
}
[ "$(layout "$table")" = v4 ] || { echo "$table is not layout v4, run scripts/build_gateway.sh" >&2; exit 1; }
[ "$(stat -f %z "$app" 2>/dev/null || stat -c %s "$app")" -le $((0x400000)) ] || { echo "$app larger than 4 MB" >&2; exit 1; }

mkdir -p backups
stamp=$(date +%Y%m%d-%H%M%S)
rd 0x8000 0x1000 "backups/ptable_$stamp.bin"
case "$(layout "backups/ptable_$stamp.bin")" in
    v4) echo "the board already has layout v4"; exit 0 ;;
    v3) ;;
    *) echo "unexpected partition table (not v3), nothing written (backups/ptable_$stamp.bin)" >&2; exit 1 ;;
esac

echo "== backing up NVS and otadata"
rd 0x9000 0x14000 "backups/nvs_$stamp.bin"
rd 0x1d000 0x2000 "backups/otadata_$stamp.bin"

echo "== writing layout v4"
python3 -c "import sys; open(sys.argv[1], 'wb').write(b'\xff' * 0x2000)" "backups/otadata_empty.bin"
wr 0x8000 "$table" 0x1d000 backups/otadata_empty.bin 0x20000 "$app"
echo "done: the gateway restarts with layout v4 from gw_a (backups in backups/*_$stamp.bin)"
echo "back to v3: $0 $port --rollback $stamp"
