#!/usr/bin/env bash
# Kept for older notes: the manifest is written by make_update_manifest.py.
# Usage: make_update_manifest.sh [notes] [release_url]
set -euo pipefail
cd "$(dirname "$0")/.."
exec python3 scripts/make_update_manifest.py ${1:+--notes "$1"} ${2:+--release-url "$2"}
