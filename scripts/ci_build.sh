#!/usr/bin/env bash
# Firmware build as CI runs it (inside espressif/idf:v5.5.5): C6 and C5 firmwares, gateway, c6fw.bin, c5fw.bin.
# The ESPHome image is left out, it needs the API key from secrets.yaml.
#
#   docker run --rm -v "$PWD":/src espressif/idf:v5.5.5 bash -c 'cp -a /src /w && /w/scripts/ci_build.sh'
set -euo pipefail
cd "$(dirname "$0")/.."
git config --global --add safe.directory '*'
command -v rsync >/dev/null || { apt-get update -qq && apt-get install -y -qq rsync >/dev/null; }

./scripts/fetch_deps.sh
./scripts/build_c6.sh all
./scripts/build_c6.sh all esp32c5
./scripts/build_gateway.sh build
./scripts/pack_dist_c6fw.sh
cp build/gateway/p4gw.bin dist/p4gw.bin
ls -l dist
