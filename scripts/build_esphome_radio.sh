#!/usr/bin/env bash
# ESPHome Bluetooth proxy for a radio role (firmware/esphome/radio_proxy, docs/dual-radio.md) into
# build/esphome_radio/<chip>/build (an ESP-IDF build directory with flash_args, packed by
# scripts/pack_dist_c6fw.sh as the "esphome" bundle).
# Usage: build_esphome_radio.sh [esp32c6|esp32c5|all]
# ESPHome comes from build/esphome-venv (pinned, created on first use) unless ESPHOME is set.
set -euo pipefail
cd "$(dirname "$0")/.."
ESPHOME_VERSION=2026.10.1
if [ -z "${ESPHOME:-}" ]; then
    venv=build/esphome-venv
    if [ ! -x "$venv/bin/esphome" ] || ! "$venv/bin/esphome" version | grep -q "$ESPHOME_VERSION"; then
        python3 -m venv "$venv"
        "$venv/bin/pip" install -q "esphome==$ESPHOME_VERSION"
    fi
    ESPHOME="$venv/bin/esphome"
fi
target=${1:-all}
case "$target" in
    all) chips="esp32c6 esp32c5" ;;
    esp32c6|esp32c5) chips=$target ;;
    *) echo "unknown chip: $target (esp32c6, esp32c5, all)" >&2; exit 1 ;;
esac
for chip in $chips; do
    out="$PWD/build/esphome_radio/$chip"
    # ESPHome brings its own ESP-IDF; the one of this repository (IDF_PATH, CI container) would make it
    # look for that environment's tools and fail
    env -u IDF_PATH -u IDF_TOOLS_PATH -u IDF_PYTHON_ENV_PATH -u ESP_IDF_VERSION \
        "$ESPHOME" -s chip "$chip" -s build_path "$out" compile firmware/esphome/radio_proxy/radio-proxy.yaml
    test -f "$out/build/flash_args"
done
