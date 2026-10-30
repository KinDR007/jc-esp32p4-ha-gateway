#!/usr/bin/env python3
"""Writes dist/releases.json: the releases of the GitHub repository that have a manifest naming
the flash layouts it runs on, newest first. Published with each release next to manifest.json; the
gateway reads it from the same place and offers the releases that run on its layout (rollback).

    {"releases": [{"version", "tag", "date", "manifest", "url", "layouts", "c6", "notes"}]}

The release being published is taken from dist/manifest.json (not public yet).

    scripts/make_releases_index.py --repo owner/name --tag v1.7 [--dist dist] [--max 10]

GITHUB_TOKEN, when set, is sent to the API (higher rate limit).
"""

import argparse
import json
import os
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def get(url: str, api: bool = False):
    headers = {"User-Agent": "p4gw-release"}
    if api:
        headers["Accept"] = "application/vnd.github+json"
        if os.environ.get("GITHUB_TOKEN"):
            headers["Authorization"] = f"Bearer {os.environ['GITHUB_TOKEN']}"
    with urllib.request.urlopen(urllib.request.Request(url, headers=headers), timeout=30) as r:
        return json.load(r)


def entry(tag: str, date: str, manifest_url: str, m: dict):
    if not m.get("layouts"):
        return None  # releases before v1.6 name no layout: never offered
    notes = m.get("notes", "")
    return {
        "version": m["version"],
        "tag": tag,
        "date": date,
        "manifest": manifest_url,
        "url": m.get("release_url", ""),
        "layouts": m["layouts"],
        "c6": m.get("c6fw", {}).get("sha256", "")[:8],
        "notes": notes if len(notes) <= 300 else notes[:297] + "...",
    }


def main() -> None:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--repo", required=True)
    p.add_argument("--tag", required=True, help="tag of the release being published")
    p.add_argument("--dist", default=str(ROOT / "dist"))
    p.add_argument("--max", type=int, default=10)
    a = p.parse_args()

    dist = Path(a.dist)
    if not (dist / "manifest.json").exists():
        raise SystemExit(f"{dist}/manifest.json missing, run scripts/make_update_manifest.py")
    own = json.loads((dist / "manifest.json").read_text())
    base = f"https://github.com/{a.repo}/releases/download"
    releases = [entry(a.tag, own.get("build", {}).get("date", ""), f"{base}/{a.tag}/manifest.json", own)]
    for r in get(f"https://api.github.com/repos/{a.repo}/releases?per_page=50", api=True):
        if r["tag_name"] == a.tag or r["draft"] or r["prerelease"]:
            continue
        if not any(asset["name"] == "manifest.json" for asset in r["assets"]):
            continue
        url = f"{base}/{r['tag_name']}/manifest.json"
        try:
            m = get(url)
        except (urllib.error.URLError, ValueError) as e:
            raise SystemExit(f"{url}: {e}")
        releases.append(entry(r["tag_name"], (r["published_at"] or "")[:10], url, m))
    releases = [r for r in releases if r][: a.max]
    out = dist / "releases.json"
    out.write_text(json.dumps({"releases": releases}, indent=2, ensure_ascii=False) + "\n")
    print(out.read_text())


if __name__ == "__main__":
    main()
