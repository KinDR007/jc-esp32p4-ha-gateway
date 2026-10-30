#!/usr/bin/env python3
"""Release notes of a tag from CHANGELOG.md: the section "## <tag>" or, without one, "## Unreleased".

    scripts/release_notes.py v1.5 > notes.md
"""

import re
import sys
from pathlib import Path

text = (Path(__file__).resolve().parents[1] / "CHANGELOG.md").read_text()
sections = dict(re.findall(r"^## (.+?)\n(.*?)(?=^## |\Z)", text, re.M | re.S))
tag = sys.argv[1] if len(sys.argv) > 1 else ""
body = sections.get(tag) or sections.get(tag.lstrip("v")) or sections.get("Unreleased", "")
print(body.strip() or f"Release {tag}")
