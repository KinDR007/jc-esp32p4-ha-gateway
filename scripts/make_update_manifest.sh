#!/usr/bin/env bash
# Writes dist/manifest.json for the online update next to dist/p4gw.bin.
# Copy both to the server the gateway checks, e.g. /config/www/p4gw/ of Home Assistant
# (URL http://<ha>:8123/local/p4gw/manifest.json).
# Usage: make_update_manifest.sh [notes] [release_url]
set -euo pipefail
cd "$(dirname "$0")/.."
[ -f dist/p4gw.bin ] || { echo "error: dist/p4gw.bin missing, run scripts/build_all.sh" >&2; exit 1; }
# The version the image reports (esp_app_desc_t.version, 32 bytes at offset 0x30 of the descriptor).
version=$(python3 - <<'PY'
data = open('dist/p4gw.bin', 'rb').read(0x200)
off = 0x18 + 0x8  # image header + first segment header
assert data[0] == 0xE9 and int.from_bytes(data[off:off + 4], 'little') == 0xABCD5432, 'not an ESP app image'
print(data[off + 0x10:off + 0x30].split(b'\0')[0].decode())
PY
)
python3 - "$version" "${1:-}" "${2:-}" > dist/manifest.json <<'PY'
import json, sys
m = {'version': sys.argv[1], 'p4gw': 'p4gw.bin'}
if sys.argv[2]: m['notes'] = sys.argv[2]
if sys.argv[3]: m['release_url'] = sys.argv[3]
print(json.dumps(m, indent=2))
PY
cat dist/manifest.json
