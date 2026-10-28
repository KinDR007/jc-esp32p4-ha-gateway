#!/usr/bin/env bash
# Read a full flash image in chunks with retries (CH340 links drop bytes on long reads).
# Usage: backup_flash.sh <port> <chip> <output.bin> [size_mb=16] [baud=115200] [chunk_kb=256]
set -euo pipefail

port=${1:?port}; chip=${2:?chip}; out=${3:?output file}
size_mb=${4:-16}; baud=${5:-115200}; chunk_kb=${6:-256}
chunk=$((chunk_kb * 1024)); count=$((size_mb * 1024 / chunk_kb)); retries=8

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for ((i = 0; i < count; i++)); do
    part="$tmp/$(printf '%05d' "$i").bin"
    for ((try = 1; ; try++)); do
        if esptool --port "$port" --chip "$chip" -b "$baud" \
            read-flash $((i * chunk)) "$chunk" "$part" >"$tmp/log" 2>&1; then
            break
        fi
        if ((try >= retries)); then
            cat "$tmp/log" >&2
            echo "chunk $i failed after $retries tries" >&2
            exit 1
        fi
        echo "chunk $i: retry $try" >&2
    done
    echo "chunk $((i + 1))/$count ok"
done

cat "$tmp"/*.bin >"$out"
shasum -a 256 "$out" | tee "$out.sha256"
