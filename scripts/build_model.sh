#!/usr/bin/env bash
# Build the YAMNet int8 model for ESP-DL (tools/yamnet/out/yamnet_p4.espdl).
# Creates the pinned Python 3.11 venv, fetches the official weights and calibration clips,
# checks the device log-mel code against the reference, quantizes and exports.
set -euo pipefail
cd "$(dirname "$0")/../tools/yamnet"
PY=${PYTHON311:-python3.11}

if [ ! -x .venv/bin/python ]; then
    "$PY" -m venv .venv
    .venv/bin/pip install -q --upgrade pip
    .venv/bin/pip install -q -r requirements.lock
fi

mkdir -p data/esc50 out
fetch() { # url file sha256
    [ -f "$2" ] || curl -sfL -m 300 -o "$2" "$1"
    echo "$3  $2" | shasum -a 256 -c --status || { echo "checksum mismatch: $2" >&2; exit 1; }
}
fetch https://storage.googleapis.com/audioset/yamnet.h5 data/yamnet.h5 \
    13c3308955bbfaef262f175ac9c40e47b134573a93984f009220dd7cc12a1744
fetch https://raw.githubusercontent.com/tensorflow/models/master/research/audioset/yamnet/yamnet_class_map.csv \
    data/yamnet_class_map.csv cdf24d193e196d9e95912a2667051ae203e92a2ba09449218ccb40ef787c6df2

# Calibration clips (ESC-50, CC BY-NC, used locally only). Real recordings from the board can be
# added to data/calib/ and are used as well.
while read -r clip; do
    [ -f "data/esc50/$clip.wav" ] || curl -sfL -m 60 -o "data/esc50/$clip.wav" \
        "https://github.com/karolpiczak/ESC-50/raw/master/audio/$clip.wav"
done < calibration_clips.txt

.venv/bin/python test_mel.py data/esc50/*.wav
.venv/bin/python export_espdl.py --out out/yamnet_p4.espdl data/esc50/*.wav $(ls data/calib/*.wav 2>/dev/null || true) \
    > out/export.log 2>&1 || { tail -20 out/export.log >&2; exit 1; }
.venv/bin/python gen_compile_req.py
ls -l out/yamnet_p4.espdl
