#!/usr/bin/env bash
# Packs the radio builds into dist/c6fw.bin (C6 of the board, build/c6/) and dist/c5fw.bin (XIAO
# ESP32-C5 of radio B, build/c5/, when built): the slots layout with the Thread and Zigbee firmwares,
# the scanner and the Wi-Fi uplink co-processor (docs/wifi-uplink.md); the ESPHome Bluetooth proxy
# (build/esphome_radio/<chip>, scripts/build_esphome_radio.sh) is a slot on the C5 (8 MB) and has its
# own whole-flash layout on the C6 (4 MB).
set -euo pipefail
cd "$(dirname "$0")/.."
part_size() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $5); print $5}' firmware/partitions.csv; }
mkdir -p dist
pack() { # chip build-dir output layout slots esphome-dir
    local chip=$1 dir=$2 out=$3 layout=$4 slots=$5 esphome=$6
    # the whole package fits the gateway's PSRAM staging buffer (STAGE_MAX in c6_flasher.c)
    python3 scripts/pack_c6fw.py --out "$out" --size 0x400000 \
        --slots "$slots" --layout "$layout" --slots-bootloader "$dir/zigbee_router/build" \
        ot_rcp="$dir/ot_rcp/build" zigbee_ncp="$dir/zigbee_ncp/build" \
        zigbee_router="$dir/zigbee_router/build" scanner="$dir/scanner/build" \
        wifi_uplink="$dir/wifi_uplink/build" esphome="$esphome"
}
slots=ot_rcp,zigbee_router,zigbee_ncp,scanner,wifi_uplink
pack esp32c6 build/c6 dist/c6fw.bin firmware/c6/slots.csv "$slots" build/esphome_radio/esp32c6/build
# a gateway installed over USB starts from this (the directory and the small bundles), its NVS has none
python3 scripts/c6fw_local.py dist/c6fw.bin dist/c6fw-local.bin --size "$(part_size c6fw)"
if [ -d build/c5/zigbee_router/build ]; then
    pack esp32c5 build/c5 dist/c5fw.bin firmware/c6/slots_c5.csv "$slots,esphome" build/esphome_radio/esp32c5/build
fi
