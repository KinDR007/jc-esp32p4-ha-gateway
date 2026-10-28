#!/usr/bin/env bash
# Build one or all C6 co-processor firmwares into build/c6/<name>/.
# Usage: build_c6.sh [ot_rcp|zigbee_ncp|zigbee_router|esp_hosted|all]
set -euo pipefail
. "$(dirname "$0")/env.sh"

# Prints the source tree to copy and, for projects that reach outside their own directory
# (esp-hosted slave uses ../common), the project subdirectory inside it.
src_of() {
    case "$1" in
        ot_rcp) echo "$IDF_PATH/examples/openthread/ot_rcp" ;;
        zigbee_ncp) echo "$ROOT_DIR/third_party/esp-coordinator" ;;
        esp_hosted) echo "$ROOT_DIR/third_party/esp-hosted-mcu|slave" ;;
        zigbee_router) echo "$ROOT_DIR/firmware/c6/zigbee_router" ;;
        *) echo "unknown firmware: $1" >&2; return 1 ;;
    esac
}

build_one() {
    local name=$1 src out sub proj
    IFS="|" read -r src sub <<<"$(src_of "$name")"
    [ -d "$src" ] || { echo "missing $src, run scripts/fetch_deps.sh" >&2; return 1; }
    out="$ROOT_DIR/build/c6/$name"
    mkdir -p "$out"
    # Build from a copy so managed_components/ and lock files never land in pinned sources.
    rsync -a --delete --exclude .git --exclude build --exclude managed_components \
        --exclude sdkconfig --exclude dependencies.lock "$src/" "$out/src/"
    proj="$out/src${sub:+/$sub}"
    # Local changes to third-party sources (C6 telemetry): firmware/c6/<name>/patches/*.patch, -p0 in the project.
    for p in "$ROOT_DIR/firmware/c6/$name"/patches/*.patch; do
        [ -f "$p" ] || continue
        patch -p0 -s -d "$proj" -i "$p" || { echo "failed to apply $p" >&2; return 1; }
    done
    # The copy has no .git, so pin PROJECT_VER to the source revision instead of this repo's.
    if [ ! -f "$proj/version.txt" ]; then
        git -C "$src" describe --tags --always > "$proj/version.txt"
    fi
    local defaults="$proj/sdkconfig.defaults"
    [ -f "$proj/sdkconfig.defaults.esp32c6" ] && defaults="$defaults;$proj/sdkconfig.defaults.esp32c6"
    if [ "$src" != "$ROOT_DIR/firmware/c6/$name" ]; then
        defaults="$defaults;$ROOT_DIR/firmware/c6/$name/sdkconfig.overlay"
    fi
    rm -f "$out/sdkconfig"
    idf.py -C "$proj" -B "$out/build" -DIDF_TARGET=esp32c6 \
        -DSDKCONFIG="$out/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build
}

target=${1:-all}
if [ "$target" = all ]; then
    for n in ot_rcp zigbee_ncp zigbee_router esp_hosted; do build_one "$n"; done
else
    build_one "$target"
fi
