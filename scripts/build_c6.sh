#!/usr/bin/env bash
# Build one or all radio co-processor firmwares into build/c6/<name>/ (C6 of the board) or
# build/c5/<name>/ (external XIAO ESP32-C5, docs/dual-radio.md).
# Usage: build_c6.sh [ot_rcp|zigbee_ncp|zigbee_router|esp_hosted|scanner|all] [esp32c6|esp32c5]
# Settings per chip (UART0 pins, flash size): firmware/c6/<name>/sdkconfig.defaults.<chip> for our
# projects, sdkconfig.overlay.<chip> for the third-party ones.
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
        scanner) echo "$ROOT_DIR/firmware/c6/scanner" ;;
        *) echo "unknown firmware: $1" >&2; return 1 ;;
    esac
}

build_one() {
    local name=$1 src out sub proj
    IFS="|" read -r src sub <<<"$(src_of "$name")"
    [ -d "$src" ] || { echo "missing $src, run scripts/fetch_deps.sh" >&2; return 1; }
    out="$ROOT_DIR/build/${CHIP#esp32}/$name"
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
    [ -f "$proj/sdkconfig.defaults.$CHIP" ] && defaults="$defaults;$proj/sdkconfig.defaults.$CHIP"
    if [ "$src" != "$ROOT_DIR/firmware/c6/$name" ]; then
        defaults="$defaults;$ROOT_DIR/firmware/c6/$name/sdkconfig.overlay"
        local overlay="$ROOT_DIR/firmware/c6/$name/sdkconfig.overlay.$CHIP"
        [ -f "$overlay" ] && defaults="$defaults;$overlay"
    fi
    rm -f "$out/sdkconfig"
    idf.py -C "$proj" -B "$out/build" -DIDF_TARGET="$CHIP" \
        -DSDKCONFIG="$out/sdkconfig" -DSDKCONFIG_DEFAULTS="$defaults" build
}

target=${1:-all}
CHIP=${2:-esp32c6}
case "$CHIP" in
    esp32c6) all="ot_rcp zigbee_ncp zigbee_router esp_hosted scanner" ;;
    esp32c5) all="ot_rcp zigbee_ncp zigbee_router scanner" ;; # no esp-hosted: the C5 is never the P4's Wi-Fi
    *) echo "unknown chip: $CHIP (esp32c6, esp32c5)" >&2; exit 1 ;;
esac
if [ "$target" = all ]; then
    for n in $all; do build_one "$n"; done
else
    build_one "$target"
fi
