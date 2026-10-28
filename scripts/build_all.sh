#!/usr/bin/env bash
# Build every image and assemble dist/:
#   p4gw.bin          gateway app (web UI upload or OTA)
#   esphome.bin       ESPHome BLE proxy role image (web UI upload)
#   c6fw.bin          C6 firmware bundle (web UI upload)
#   p4gw-factory.bin  complete flash image for the first USB flash (scripts/flash_p4.sh)
set -euo pipefail
cd "$(dirname "$0")/.."
ROOT=$(pwd)

./scripts/fetch_deps.sh
./scripts/build_c6.sh all
./scripts/build_gateway.sh build
./scripts/build_esphome.sh

. scripts/env.sh
mkdir -p dist
part_offset() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $4); print $4}' firmware/partitions.csv; }
part_size() { awk -F, -v n="$1" '$1 ~ "^"n"[ ]*$" {gsub(/ /, "", $5); print $5}' firmware/partitions.csv; }

python3 scripts/pack_c6fw.py --out dist/c6fw.bin --size "$(part_size c6fw)" \
    ot_rcp=build/c6/ot_rcp/build zigbee_ncp=build/c6/zigbee_ncp/build \
    zigbee_router=build/c6/zigbee_router/build esp_hosted=build/c6/esp_hosted/build

cp build/gateway/p4gw.bin dist/p4gw.bin
cp firmware/esphome/.esphome/build/ble-proxy-p4gw/build/firmware.ota.bin dist/esphome.bin

# flash_args lists bootloader, partition table, otadata and app with their offsets.
gw_args=()
while read -r offset file; do
    gw_args+=("$offset" "$ROOT/build/gateway/$file")
done < <(tail -n +2 build/gateway/flash_args)
esptool --chip esp32p4 merge-bin -o dist/p4gw-factory.bin --flash-mode dio --flash-size 16MB \
    "${gw_args[@]}" \
    "$(part_offset esphome)" dist/esphome.bin \
    "$(part_offset c6fw)" dist/c6fw.bin
( cd dist && shasum -a 256 p4gw.bin esphome.bin c6fw.bin p4gw-factory.bin > SHA256SUMS )
ls -l dist
