#!/usr/bin/env bash
# Build one or all C6 co-processor firmwares into build/c6/<name>/.
# Usage: build_c6.sh [ot_rcp|zigbee_ncp|esp_hosted|all]
set -euo pipefail
. "$(dirname "$0")/env.sh"

src_of() {
    case "$1" in
        ot_rcp) echo "$IDF_PATH/examples/openthread/ot_rcp" ;;
        zigbee_ncp) echo "$ROOT_DIR/third_party/esp-coordinator" ;;
        esp_hosted) echo "$ROOT_DIR/third_party/esp-hosted-mcu/slave" ;;
        *) echo "unknown firmware: $1" >&2; return 1 ;;
    esac
}

build_one() {
    local name=$1 src out
    src=$(src_of "$name")
    [ -d "$src" ] || { echo "missing $src, run scripts/fetch_deps.sh" >&2; return 1; }
    out="$ROOT_DIR/build/c6/$name"
    mkdir -p "$out"
    # Build from a copy so managed_components/ and lock files never land in pinned sources.
    rsync -a --delete --exclude .git --exclude build --exclude managed_components \
        --exclude sdkconfig --exclude dependencies.lock "$src/" "$out/src/"
    # The copy has no .git, so pin PROJECT_VER to the source revision instead of this repo's.
    if [ ! -f "$out/src/version.txt" ]; then
        git -C "$src" describe --tags --always > "$out/src/version.txt"
    fi
    local defaults="$out/src/sdkconfig.defaults"
    [ -f "$out/src/sdkconfig.defaults.esp32c6" ] && defaults="$defaults;$out/src/sdkconfig.defaults.esp32c6"
    defaults="$defaults;$ROOT_DIR/firmware/c6/$name/sdkconfig.overlay"
    rm -f "$out/sdkconfig"
    idf.py -C "$out/src" -B "$out/build" -DIDF_TARGET=esp32c6 \
        -DSDKCONFIG="$out/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build
}

target=${1:-all}
if [ "$target" = all ]; then
    for n in ot_rcp zigbee_ncp esp_hosted; do build_one "$n"; done
else
    build_one "$target"
fi
