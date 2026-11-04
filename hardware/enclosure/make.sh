#!/usr/bin/env bash
# STL of every part into stl/, after checking that the board model fits and every part is one body.
#   hardware/enclosure/make.sh [-D name=value ...]     e.g. -D xiao_bay=false -D inner_h=16
set -euo pipefail
cd "$(dirname "$0")"
scad=${OPENSCAD:-$(command -v openscad || echo /Applications/OpenSCAD.app/Contents/MacOS/OpenSCAD)}
[ -x "$scad" ] || { echo "OpenSCAD not found (set OPENSCAD)" >&2; exit 1; }
mkdir -p stl
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
if "$scad" -o "$tmp/clash.stl" -D 'part="clash"' "$@" p4_case.scad 2>"$tmp/log"; then
    echo "the board model collides with the case (stl/clash.stl)" >&2
    cp "$tmp/clash.stl" stl/
    exit 1
fi
grep -q "top level object is empty" "$tmp/log" || { cat "$tmp/log" >&2; exit 1; }
echo "board model fits"
for p in bottom top fit_test; do
    "$scad" -o "stl/p4_case_$p.stl" -D "part=\"$p\"" "$@" p4_case.scad 2>"$tmp/log" || { cat "$tmp/log" >&2; exit 1; }
    python3 bodies.py "stl/p4_case_$p.stl"
done
