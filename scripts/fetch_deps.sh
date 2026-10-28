#!/usr/bin/env bash
# Clone or update third_party/ to the commits pinned in deps.lock (idempotent).
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party

grep -v '^#' deps.lock | while read -r name url commit _; do
    [ -n "$name" ] || continue
    dir="third_party/$name"
    if [ ! -d "$dir/.git" ]; then
        git clone -q --filter=blob:none "$url" "$dir"
    fi
    if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ]; then
        git -C "$dir" fetch -q origin "$commit" 2>/dev/null || git -C "$dir" fetch -q origin
        git -C "$dir" checkout -q "$commit"
    fi
    if [ -n "$(git -C "$dir" status --porcelain)" ]; then
        echo "warning: $dir has local changes" >&2
    fi
    echo "$name $(git -C "$dir" rev-parse --short HEAD)"
done
