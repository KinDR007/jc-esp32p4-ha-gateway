#!/usr/bin/env bash
# Checks without the ESP-IDF toolchain: unit tests, Python and web UI script syntax, icon block.
set -euo pipefail
cd "$(dirname "$0")/.."
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

echo "== unit tests"
cc -std=c11 -Wall -Wextra -Werror -o "$tmp/zb_filter_test" tests/unit/zb_filter_test.c
"$tmp/zb_filter_test"
cc -std=c11 -Wall -Wextra -Werror -I firmware/gateway/components/sound -o "$tmp/aweight_test" \
    tests/unit/aweight_test.c firmware/gateway/components/sound/sound_aweight.c -lm
"$tmp/aweight_test"

echo "== python syntax"
python3 -m py_compile scripts/*.py tests/*.py tests/zha/*.py tests/ha_integration/*.py custom_components/p4gw/*.py
python3 -m json.tool custom_components/p4gw/manifest.json >/dev/null
for f in custom_components/p4gw/strings.json custom_components/p4gw/translations/*.json hacs.json; do python3 -m json.tool "$f" >/dev/null; done

echo "== web UI script"
python3 - firmware/gateway/web/index.html "$tmp/ui.js" <<'PY'
import re, sys
html = open(sys.argv[1], encoding='utf-8').read()
scripts = re.findall(r'<script>(.*?)</script>', html, re.S)
assert scripts, 'no inline script'
open(sys.argv[2], 'w', encoding='utf-8').write('\n'.join(scripts))
PY
node --check "$tmp/ui.js"
for f in tests/ui/ui_test.mjs tools/web/gen_icons.mjs tools/web/custom_icons.mjs; do node --check "$f"; done

echo "== icon block up to date"
npm --prefix tools/web ci --silent
cp firmware/gateway/web/index.html "$tmp/index.html"
node tools/web/gen_icons.mjs >/dev/null
if ! cmp -s firmware/gateway/web/index.html "$tmp/index.html"; then
    cp "$tmp/index.html" firmware/gateway/web/index.html
    echo "icon block outdated: run node tools/web/gen_icons.mjs" >&2
    exit 1
fi
echo "all checks passed"
