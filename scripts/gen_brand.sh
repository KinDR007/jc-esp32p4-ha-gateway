#!/usr/bin/env bash
# Icon images from assets/brand/*.svg: Home Assistant brand images of the integration and the web UI favicon.
# Needs rsvg-convert and ImageMagick (brew install librsvg imagemagick).
set -euo pipefail
cd "$(dirname "$0")/.."
src=assets/brand
brand=custom_components/p4gw/brand
mkdir -p "$brand"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

rsvg-convert -w 256 -h 256 "$src/p4gw.svg" -o "$brand/icon.png"
rsvg-convert -w 512 -h 512 "$src/p4gw.svg" -o "$brand/icon@2x.png"

# favicon.ico: 16 and 32 px from the simplified drawing (no outer arcs, thicker lines), 48 px from the full one
rsvg-convert -w 16 -h 16 "$src/p4gw-small.svg" -o "$tmp/16.png"
rsvg-convert -w 32 -h 32 "$src/p4gw-small.svg" -o "$tmp/32.png"
rsvg-convert -w 48 -h 48 "$src/p4gw.svg" -o "$tmp/48.png"
magick "$tmp/16.png" "$tmp/32.png" "$tmp/48.png" firmware/gateway/web/p4gw_favicon.ico

ls -l "$brand" firmware/gateway/web/p4gw_favicon.ico
