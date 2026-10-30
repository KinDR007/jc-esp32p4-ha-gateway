#!/usr/bin/env python3
"""Writes the manifest of an online update next to p4gw.bin (and c6fw.bin).

    {"version", "p4gw", "notes", "release_url", "layouts",
     "build": {"commit", "date", "idf", "ci"},
     "c6fw": {"url", "sha256", "size", "format", "bundles": {name: version}}}

The gateway compares "version" with its own, and the sha256 of c6fw.bin with the C6 firmware it
holds; both download only on request. URLs are relative to the manifest unless --base-url is
given (a GitHub release uses the tag's download URL, so the files match the manifest even when a
newer release appears meanwhile).

    scripts/make_update_manifest.py [--dist dist] [--notes TEXT | --notes-file FILE]
        [--release-url URL] [--base-url URL] [--ci URL]
"""

import argparse
import datetime
import hashlib
import json
import re
import struct
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
# flash layouts (docs/installation.md) the gateway firmware of this tree runs on; the gateway installs
# a release only when its layout is listed (an older release on a newer layout loses the C6 firmware)
LAYOUTS = [2, 3]


def app_version(path: Path) -> str:
    """esp_app_desc_t.version of an ESP application image."""
    data = path.read_bytes()[:0x200]
    off = 0x18 + 0x8  # image header + first segment header
    if data[0] != 0xE9 or int.from_bytes(data[off:off + 4], "little") != 0xABCD5432:
        raise SystemExit(f"{path}: not an ESP application image")
    return data[off + 0x10:off + 0x30].split(b"\0")[0].decode()


def c6fw_info(path: Path) -> dict:
    """Directory of a c6fw.bin (scripts/pack_c6fw.py): format and the version of each bundle."""
    data = path.read_bytes()
    magic, fmt, count = struct.unpack_from("<4sII", data, 0)
    if magic != b"C6FW":
        raise SystemExit(f"{path}: not a c6fw image")
    bundles = {}
    for i in range(count):
        name, version = struct.unpack_from("<16s32s", data, 12 + i * 88)
        bundles[name.split(b"\0")[0].decode()] = version.split(b"\0")[0].decode()
    return {"sha256": hashlib.sha256(data).hexdigest(), "size": len(data), "format": fmt, "bundles": bundles}


def git(*args: str) -> str:
    try:
        return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return ""


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--dist", default=str(ROOT / "dist"))
    p.add_argument("--notes", default="")
    p.add_argument("--notes-file")
    p.add_argument("--release-url", default="")
    p.add_argument("--base-url", default="", help="absolute URL prefix of the files (else relative to the manifest)")
    p.add_argument("--ci", default="", help="URL of the CI run that built the files")
    a = p.parse_args()

    dist = Path(a.dist)
    gateway = dist / "p4gw.bin"
    if not gateway.exists():
        raise SystemExit(f"{gateway} missing, run scripts/build_all.sh")
    url = lambda name: f"{a.base_url.rstrip('/')}/{name}" if a.base_url else name
    notes = Path(a.notes_file).read_text().strip() if a.notes_file else a.notes
    m = {"version": app_version(gateway), "p4gw": url("p4gw.bin"), "layouts": LAYOUTS}
    if notes:
        m["notes"] = notes if len(notes) <= 1000 else notes[:998].rsplit(" ", 1)[0] + " …"
    if a.release_url:
        m["release_url"] = a.release_url
    idf = re.search(r"v\d+\.\d+\.\d+", (ROOT / "scripts/env.sh").read_text())
    m["build"] = {"commit": git("rev-parse", "--short", "HEAD"), "date": datetime.date.today().isoformat(),
                  "idf": idf.group(0) if idf else ""}
    if a.ci:
        m["build"]["ci"] = a.ci
    c6 = dist / "c6fw.bin"
    if c6.exists():
        m["c6fw"] = {"url": url("c6fw.bin"), **c6fw_info(c6)}
    out = dist / "manifest.json"
    out.write_text(json.dumps(m, indent=2, ensure_ascii=False) + "\n")
    print(out.read_text())


if __name__ == "__main__":
    main()
