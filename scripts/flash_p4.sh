#!/usr/bin/env bash
# First flash of the P4 over USB (CH340 port next to the Ethernet jack).
# Writes dist/p4gw-factory.bin. This resets NVS (role, Thread network, settings): the merged image
# fills gaps with 0xFF. Later updates go through the web UI and keep NVS. --erase wipes the whole chip first.
# Usage: flash_p4.sh <port> [--erase] [baud=460800]
set -euo pipefail
cd "$(dirname "$0")/.."
port=${1:?usage: flash_p4.sh <port> [--erase] [baud]}
shift
erase=0
if [ "${1:-}" = "--erase" ]; then erase=1; shift; fi
baud=${1:-460800}
[ -f dist/p4gw-factory.bin ] || { echo "run scripts/build_all.sh first" >&2; exit 1; }
if [ $erase = 1 ]; then
    esptool --chip esp32p4 --port "$port" -b "$baud" erase-flash
fi
esptool --chip esp32p4 --port "$port" -b "$baud" write-flash --flash-mode dio --flash-size 16MB 0x0 dist/p4gw-factory.bin
