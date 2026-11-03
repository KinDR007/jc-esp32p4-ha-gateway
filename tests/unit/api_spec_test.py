#!/usr/bin/env python3
"""firmware/gateway/web/api.json against the firmware: every registered endpoint is described once, nothing
else is, and the permission of each one is the one gw_auth.c enforces (AUTH_MAP, unlisted = admin only,
gw_http_register_public = public). Texts in English and Czech, the fields the docs and the UI read.
Usage: api_spec_test.py [api.json]"""
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SPEC = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / 'firmware/gateway/web/api.json'
SOURCES = [p for d in ('firmware/gateway/components', 'firmware/gateway/main') for p in (ROOT / d).rglob('*.c')]
REG = re.compile(r'gw_http_register(_public)?\("(/api/[^"]*)",\s*HTTP_([A-Z]+)')
RULE = re.compile(r'\{"(/api/[^"]*)",\s*(-1|HTTP_[A-Z]+),\s*"([^"]+)",\s*(\d)\}')


def registered():
    found = {('GET', '/api/ws'): 'ws'}  # gw_http.c registers the WebSocket with httpd directly
    for src in SOURCES:
        for public, path, method in REG.findall(src.read_text(errors='replace')):
            found[(method, path)] = 'public' if public else 'private'
    return found


def auth_rules():
    text = (ROOT / 'firmware/gateway/components/gw_core/gw_auth.c').read_text()
    block = text[text.index('AUTH_MAP[] = {'):]
    block = block[:block.index('};')]
    return [(path, None if m == '-1' else m[5:], page, int(level)) for path, m, page, level in RULE.findall(block)]


def expected_perm(method, path, kind, rules):
    if kind == 'public':
        return 'public', 0
    for rpath, rmethod, page, level in rules:
        if rpath == path and rmethod in (None, method):
            return page, level
    return 'admin', 2


def main():
    errors = []
    spec = json.loads(SPEC.read_text())
    code, rules = registered(), auth_rules()
    seen = {}
    for g in spec['groups']:
        if not g.get('title', {}).get('en') or not g['title'].get('cs'):
            errors.append(f'group {g.get("id")}: title en/cs missing')
        for e in g['endpoints']:
            key = (e['method'], e['path'])
            if key in seen:
                errors.append(f'{key} described twice ({seen[key]}, {g["id"]})')
            seen[key] = g['id']
            for field in ('summary',):
                if not e.get(field, {}).get('en') or not e[field].get('cs'):
                    errors.append(f'{key}: {field} en/cs missing')
            if e.get('body_type') not in ('json', 'binary', 'none'):
                errors.append(f'{key}: body_type {e.get("body_type")!r}')
            if key in code:
                want = expected_perm(*key, code[key], rules)
                if (e.get('perm'), e.get('level')) != want:
                    errors.append(f'{key}: perm {e.get("perm")}/{e.get("level")}, the firmware enforces {want[0]}/{want[1]}')
    for key in sorted(set(code) - set(seen)):
        errors.append(f'{key} registered in the firmware but not described')
    for key in sorted(set(seen) - set(code)):
        errors.append(f'{key} described but not registered in the firmware')
    if not spec.get('auth', {}).get('en') or not spec['auth'].get('cs'):
        errors.append('auth: en/cs missing')
    for e in errors:
        print('FAIL', e)
    print(f'{len(seen)} endpoints described, {len(code)} registered, {len(errors)} problems')
    sys.exit(1 if errors else 0)


if __name__ == '__main__':
    main()
