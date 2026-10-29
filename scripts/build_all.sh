#!/usr/bin/env bash
# Build every image and assemble dist/:
#   p4gw.bin          gateway app (web UI upload or OTA)
#   esphome.bin       ESPHome BLE proxy role image (web UI upload)
#   c6fw.bin          C6 firmware bundle (web UI upload)
#   yamnet_p4.espdl   sound recognition model (web UI upload), if scripts/build_model.sh ran
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

./scripts/pack_dist_c6fw.sh

cp build/gateway/p4gw.bin dist/p4gw.bin
cp firmware/esphome/.esphome/build/ble-proxy-p4gw/build/firmware.ota.bin dist/esphome.bin
model=tools/yamnet/out/yamnet_p4.espdl
model_args=()
if [ -f "$model" ]; then
    cp "$model" dist/yamnet_p4.espdl
    model_args=("$(part_offset model)" dist/yamnet_p4.espdl)
else
    echo "warning: no $model, run scripts/build_model.sh for sound recognition" >&2
fi

# flash_args lists bootloader, partition table, otadata and app with their offsets.
gw_args=()
while read -r offset file; do
    gw_args+=("$offset" "$ROOT/build/gateway/$file")
done < <(tail -n +2 build/gateway/flash_args)
esptool --chip esp32p4 merge-bin -o dist/p4gw-factory.bin --flash-mode dio --flash-size 16MB \
    "${gw_args[@]}" \
    "$(part_offset esphome)" dist/esphome.bin \
    "$(part_offset c6fw)" dist/c6fw.bin "${model_args[@]}"
( cd dist && shasum -a 256 *.bin *.espdl 2>/dev/null > SHA256SUMS )
ls -l dist
