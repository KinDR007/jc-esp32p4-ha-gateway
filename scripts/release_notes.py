#!/usr/bin/env python3
"""Release notes of a tag: its CHANGELOG.md section ("## v1.5 ..." or, without one, "## Unreleased")
and the commits since the previous tag.

    scripts/release_notes.py v1.5 [--max N] > notes.md
"""

import argparse
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True).stdout.strip()


def main() -> None:
    p = argparse.ArgumentParser()
    p.add_argument("tag")
    p.add_argument("--max", type=int, default=0, help="cut the text to this many characters (manifest)")
    p.add_argument("--no-commits", action="store_true")
    a = p.parse_args()
    text = (ROOT / "CHANGELOG.md").read_text()
    sections = {h.split()[0]: b for h, b in re.findall(r"^## (.+?)\n(.*?)(?=^## |\Z)", text, re.M | re.S)}
    body = (sections.get(a.tag) or sections.get("Unreleased") or "").strip() or f"Release {a.tag}"
    out = body
    if not a.no_commits:
        ref = a.tag if git("rev-parse", "-q", "--verify", f"refs/tags/{a.tag}") else "HEAD"
        prev = git("describe", "--tags", "--abbrev=0", f"{ref}^")
        log = git("log", "--no-merges", "--format=- %h %s", f"{prev}..{ref}" if prev else ref)
        if log:
            out += f"\n\n### Commits{' since ' + prev if prev else ''}\n\n{log}"
    print(out[: a.max] if a.max else out)


if __name__ == "__main__":
    main()
