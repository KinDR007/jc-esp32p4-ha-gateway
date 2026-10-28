#!/usr/bin/env bash
# Build the ESPHome BLE proxy role image (dist/esphome.bin is copied by build_all.sh).
# ESPHOME selects the esphome binary; default is the pinned venv of the ble_proxy project.
set -euo pipefail
cd "$(dirname "$0")/../firmware/esphome"
ESPHOME=${ESPHOME:-"$(cd ../../.. && pwd)/esphome/ble_proxy/.venv/bin/esphome"}
[ -x "$ESPHOME" ] || ESPHOME=$(command -v esphome) || { echo "esphome not found, set ESPHOME" >&2; exit 1; }
[ -f secrets.yaml ] || { echo "missing firmware/esphome/secrets.yaml (see secrets.yaml.example)" >&2; exit 1; }
"$ESPHOME" compile ble-proxy-p4gw.yaml
