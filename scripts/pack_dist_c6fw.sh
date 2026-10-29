#!/usr/bin/env bash
# Packs the C6 builds under build/c6/ into dist/c6fw.bin (slots layout plus esp_hosted).
set -euo pipefail
cd "$(dirname "$0")/.."
part_size() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $5); print $5}' firmware/partitions.csv; }
mkdir -p dist
python3 scripts/pack_c6fw.py --out dist/c6fw.bin --size "$(part_size c6fw)" \
    --slots ot_rcp,zigbee_router,zigbee_ncp,scanner --layout firmware/c6/slots.csv \
    --slots-bootloader build/c6/zigbee_router/build \
    ot_rcp=build/c6/ot_rcp/build zigbee_ncp=build/c6/zigbee_ncp/build \
    zigbee_router=build/c6/zigbee_router/build esp_hosted=build/c6/esp_hosted/build \
    scanner=build/c6/scanner/build
