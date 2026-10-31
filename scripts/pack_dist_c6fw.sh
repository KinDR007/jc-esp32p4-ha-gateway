#!/usr/bin/env bash
# Packs the radio builds into dist/c6fw.bin (C6 of the board, build/c6/) and dist/c5fw.bin (XIAO
# ESP32-C5 of radio B, build/c5/, when built): the slots layout with the Thread and Zigbee
# firmwares plus the ESPHome Bluetooth proxy (build/esphome_radio/<chip>, scripts/build_esphome_radio.sh).
set -euo pipefail
cd "$(dirname "$0")/.."
part_size() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $5); print $5}' firmware/partitions.csv; }
mkdir -p dist
pack() { # chip build-dir output
    local chip=$1 dir=$2 out=$3 esphome=()
    [ -f "build/esphome_radio/$chip/build/flash_args" ] && esphome=(esphome="build/esphome_radio/$chip/build")
    # the whole package fits the gateway's PSRAM staging buffer (STAGE_MAX in c6_flasher.c)
    python3 scripts/pack_c6fw.py --out "$out" --size 0x400000 \
        --slots ot_rcp,zigbee_router,zigbee_ncp,scanner --layout firmware/c6/slots.csv \
        --slots-bootloader "$dir/zigbee_router/build" \
        ot_rcp="$dir/ot_rcp/build" zigbee_ncp="$dir/zigbee_ncp/build" \
        zigbee_router="$dir/zigbee_router/build" scanner="$dir/scanner/build" "${esphome[@]}"
}
pack esp32c6 build/c6 dist/c6fw.bin
# a gateway installed over USB starts from this (the directory and the small bundles), its NVS has none
python3 scripts/c6fw_local.py dist/c6fw.bin dist/c6fw-local.bin --size "$(part_size c6fw)"
if [ -d build/c5/zigbee_router/build ]; then
    pack esp32c5 build/c5 dist/c5fw.bin
fi
