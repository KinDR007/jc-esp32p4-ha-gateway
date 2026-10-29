#!/usr/bin/env bash
# Build the P4 gateway firmware into build/gateway/.
set -euo pipefail
. "$(dirname "$0")/env.sh"
out="$ROOT_DIR/build/gateway"
# sdkconfig.defaults only applies to a new sdkconfig: regenerate it after the defaults changed.
if [ -f "$out/sdkconfig" ] && [ "$ROOT_DIR/firmware/gateway/sdkconfig.defaults" -nt "$out/sdkconfig" ]; then
    rm -f "$out/sdkconfig"
fi
idf.py -C "$ROOT_DIR/firmware/gateway" -B "$out" -DSDKCONFIG="$out/sdkconfig" "${@:-build}"
