#!/usr/bin/env python3
"""docs/api.md from firmware/gateway/web/api.json (the gateway serves the same file at /api/help, its web
UI shows it under System › API). scripts/ci_checks.sh fails when docs/api.md is not up to date.
Usage: api_md.py [output]"""
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
PERM = {"public": "no sign-in", "admin": "administrators", "*": "any signed-in client", "self": "own account"}


def perm(e):
    return PERM.get(e["perm"]) or f'page `{e["perm"]}`, {"edit" if e["level"] == 2 else "read"}'


def main():
    spec = json.loads((ROOT / "firmware/gateway/web/api.json").read_text())
    out = ["# HTTP API", "",
           "Generated from [firmware/gateway/web/api.json](../firmware/gateway/web/api.json) by `tools/docs/api_md.py`; "
           "do not edit by hand. The gateway serves the same description at `GET /api/help`, and its web UI shows it under "
           "System › API with examples built for that gateway (its address, a token typed in there, its radios and roles) "
           "and a button that tries GET requests.", "",
           "## Authentication", "", spec["auth"]["en"], "",
           "Errors come as `{\"error\": \"...\"}` with 400 (invalid input), 401 (sign-in needed), 403 (permission), "
           "404, 409 (not possible in the current state) or 413 (upload too large). Permissions name the web UI page whose "
           "right a user needs (read or edit); see [users.md](users.md).", "",
           "```sh", "curl -s -H 'Authorization: Bearer p4gw_...' http://192.168.1.10/api/status", "```", ""]
    out += ["## Endpoints", ""]
    for g in spec["groups"]:
        out += [f"### {g['title']['en']}", "", "| Method | Path | What it does | Permission |", "|---|---|---|---|"]
        out += [f"| {e['method']} | `{e['path']}` | {e['summary']['en']} | {perm(e)} |" for e in g["endpoints"]]
        out.append("")
        for e in g["endpoints"]:
            parts = []
            if e.get("query"):
                parts.append("Query: " + ", ".join(f"`{q['name']}={q.get('values', '')}` {q['desc']['en']}" for q in e["query"]))
            if e.get("body_type") == "json" and isinstance(e.get("body"), dict):
                parts.append("Body:\n\n```json\n" + json.dumps(e["body"], indent=2, ensure_ascii=False) + "\n```")
            elif e.get("body_type") == "binary":
                parts.append(f"Body: the raw file (`{e.get('body') or 'file'}`).")
            if e.get("response") not in (None, ""):
                r = e["response"]
                parts.append("Response:\n\n```json\n" + json.dumps(r, indent=2, ensure_ascii=False) + "\n```" if not isinstance(r, str) else f"Response: {r}.")
            if e.get("errors"):
                parts.append("Errors: " + "; ".join(f"{x['code']} {x['when']['en']}" for x in e["errors"]))
            if e.get("effects"):
                parts.append("Effects: " + e["effects"]["en"])
            if parts:
                out += [f"#### {e['method']} `{e['path']}`", "", *[p + "\n" for p in parts]]
    text = "\n".join(out).rstrip() + "\n"
    target = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "docs/api.md"
    target.write_text(text)
    print("written:", target)


if __name__ == "__main__":
    main()
