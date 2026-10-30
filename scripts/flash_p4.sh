#!/usr/bin/env bash
# Flash the P4 over USB (CH340 port next to the Ethernet jack).
#   default      write bootloader, partition table and all images, keep NVS (role, Thread
#                network, password, MQTT); old Zigbee snapshots and recordings are cleared
#   --erase-all  erase the whole chip first and write dist/p4gw-factory.bin (fresh device)
# Usage: flash_p4.sh <port> [--erase-all] [baud=460800]
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?usage: flash_p4.sh <port> [--erase-all] [baud]}
shift
erase_all=0
if [ "${1:-}" = "--erase-all" ]; then erase_all=1; shift; fi
baud=${1:-460800}
[ -f dist/p4gw-factory.bin ] || { echo "run scripts/build_all.sh first" >&2; exit 1; }

part() { awk -F, -v n="$1" -v c="$2" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $c); print $c}' firmware/partitions.csv; }
esptool() { command esptool --chip esp32p4 --port "$port" -b "$baud" "$@"; }

if [ $erase_all = 1 ]; then
    esptool erase-flash
    esptool write-flash --flash-mode dio --flash-size 16MB 0x0 dist/p4gw-factory.bin
    exit 0
fi

args=()
while read -r offset file; do
    args+=("$offset" "build/gateway/$file")
done < <(tail -n +2 build/gateway/flash_args)
args+=("$(part esphome 4)" dist/esphome.bin "$(part c6fw 4)" dist/c6fw-local.bin)
[ -f dist/yamnet_p4.espdl ] && args+=("$(part model 4)" dist/yamnet_p4.espdl)
for p in c6data sounds; do
    esptool erase-region "$(part $p 4)" "$(part $p 5)"
done
esptool write-flash --flash-mode dio --flash-size 16MB "${args[@]}"
