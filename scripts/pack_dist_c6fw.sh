#!/usr/bin/env bash
# Packs the C6 builds under build/c6/ into dist/c6fw.bin (slots layout plus esp_hosted).
set -euo pipefail
cd "$(dirname "$0")/.."
part_size() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $5); print $5}' firmware/partitions.csv; }
mkdir -p dist
# the whole package fits the gateway's PSRAM staging buffer (STAGE_MAX in c6_flasher.c)
python3 scripts/pack_c6fw.py --out dist/c6fw.bin --size 0x240000 \
    --slots ot_rcp,zigbee_router,zigbee_ncp,scanner --layout firmware/c6/slots.csv \
    --slots-bootloader build/c6/zigbee_router/build \
    ot_rcp=build/c6/ot_rcp/build zigbee_ncp=build/c6/zigbee_ncp/build \
    zigbee_router=build/c6/zigbee_router/build esp_hosted=build/c6/esp_hosted/build \
    scanner=build/c6/scanner/build
# what the c6fw partition holds: the whole package, or with layout v3 the directory and the small bundles
python3 scripts/c6fw_local.py dist/c6fw.bin dist/c6fw-local.bin --size "$(part_size c6fw)"
# the XIAO ESP32-C5 of radio B (scripts/build_c6.sh all esp32c5), when built
if [ -d build/c5/zigbee_router/build ]; then
    python3 scripts/pack_c6fw.py --out dist/c5fw.bin --size 0x240000 \
        --slots ot_rcp,zigbee_router,zigbee_ncp,scanner --layout firmware/c6/slots.csv \
        --slots-bootloader build/c5/zigbee_router/build \
        ot_rcp=build/c5/ot_rcp/build zigbee_ncp=build/c5/zigbee_ncp/build \
        zigbee_router=build/c5/zigbee_router/build scanner=build/c5/scanner/build
fi
