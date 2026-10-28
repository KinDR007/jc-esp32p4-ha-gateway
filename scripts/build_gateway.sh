#!/usr/bin/env bash
# Build the P4 gateway firmware into build/gateway/.
set -euo pipefail
. "$(dirname "$0")/env.sh"
out="$ROOT_DIR/build/gateway"
idf.py -C "$ROOT_DIR/firmware/gateway" -B "$out" -DSDKCONFIG="$out/sdkconfig" "${@:-build}"
