#!/usr/bin/env python3
"""Hardware test of the P4 gateway: every role and the transitions between them.

Runs against a real board over the LAN. Test cases are described in tests/TESTCASES.md.

    python3 tests/hw_test.py --host 192.168.88.243            # full run, in order
    python3 tests/hw_test.py --host p4gw-xxxxxx.local --only TC04 TC05
    python3 tests/hw_test.py --list

Destructive: switches roles, reflashes the C6 and writes the gateway slot.
"""
import argparse
import glob
import json
import os
import re
import socket
import sys
import time
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = []
CTX = {}


class Fail(Exception):
    pass


class Skip(Exception):
    """The case cannot run here (hardware or test host missing); reported as SKIP."""


def log(msg):
    print(f'  {time.strftime("%H:%M:%S")} {msg}', flush=True)


def http(method, path, body=None, raw=None, timeout=30, port=80, auth=None, user='admin'):
    url = f'http://{ARGS.host}:{port}{path}'
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(url, data=data, method=method)
    if body is not None:
        req.add_header('Content-Type', 'application/json')
    if auth:
        import base64
        req.add_header('Authorization', 'Basic ' + base64.b64encode(f'{user}:{auth}'.encode()).decode())
    elif ARGS.token:
        req.add_header('Authorization', 'Bearer ' + ARGS.token)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as res:
            payload = res.read()
            code = res.status
    except urllib.error.HTTPError as err:
        payload, code = err.read(), err.code
    try:
        return code, json.loads(payload)
    except ValueError:
        return code, payload


def status():
    code, s = http('GET', '/api/status', timeout=5)
    if code != 200:
        raise Fail(f'/api/status returned {code}: {s}')
    return s


def check(cond, msg):
    if not cond:
        raise Fail(msg)
    log('ok: ' + msg)


def port_open(port, timeout=2):
    try:
        with socket.create_connection((ARGS.host, port), timeout=timeout):
            return True
    except OSError:
        return False


def wait_until(what, fn, timeout, interval=2):
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            last = fn()
            if last:
                return last
        except (OSError, Fail, urllib.error.URLError) as err:
            last = err
        time.sleep(interval)
    raise Fail(f'timeout waiting for {what} (last: {last})')


def wait_restart(expected_role, timeout=240):
    """Waits for the gateway to come back and finish role start (C6 flash included)."""
    time.sleep(5)

    def ready():
        s = status()
        if s['c6']['flash']['busy']:
            return None
        if s['role']['active'] == expected_role or s['role']['maintenance']:
            return s
        return None

    s = wait_until(f'gateway with role {expected_role}', ready, timeout)
    log(f'back after restart: active={s["role"]["active"]} c6={s["c6"]["bundle"]} uptime={s["uptime_s"]}s')
    return s


def switch(role, skip_backup=False, timeout=240):
    body = {'role': role}
    if skip_backup:
        body['skip_backup'] = True
    t0 = time.time()
    code, r = http('POST', '/api/role', body)
    if code != 200:
        raise Fail(f'role switch to {role}: {code} {r}')
    s = wait_restart(role, timeout)
    check(not s['role']['maintenance'], f'{role} started without maintenance ({s["role"]["maintenance"]!r})')
    check(s['role']['active'] == role, f'active role is {role}')
    log(f'switch took {time.time() - t0:.0f} s')
    return s


def wait_thread_running(timeout=60):
    """OpenThread starts a moment after the C6 flash finished: wait until the Thread API answers."""
    return wait_until('OpenThread running', lambda: (lambda t: t if t.get('network_name') is not None or
                      t.get('role') else None)(http('GET', '/api/thread/status')[1]), timeout)


def logs_since(seq=0):
    req = urllib.request.Request(f'http://{ARGS.host}/api/logs?since={seq}')
    if ARGS.token:
        req.add_header('Authorization', 'Bearer ' + ARGS.token)
    with urllib.request.urlopen(req, timeout=10) as res:
        return res.read().decode(errors='replace')


# ---------------------------------------------------------------- test cases

def tc01_first_start():
    """First start keeps the factory C6 and reports maintenance."""
    s = status()
    check(s['role']['active'] == 'none', 'no role active on first start')
    check('back up' in s['role']['maintenance'], 'maintenance asks for a C6 backup')
    check(s['eth']['link'] and s['eth'].get('ipv4'), 'Ethernet up with IPv4')
    check(any(a.lower().startswith('fe80') for a in s['eth']['ipv6']), 'IPv6 link-local present')
    names = {i['name'] for i in s['c6']['images']}
    check(names >= {'ot_rcp', 'zigbee_ncp', 'zigbee_router', 'esp_hosted'}, f'all C6 bundles present ({sorted(names)})')


def tc02_backup_guard():
    """A UART role is refused until the C6 is backed up or the backup is skipped."""
    code, r = http('POST', '/api/role', {'role': 'thread_br'})
    check(code == 409 and r.get('error') == 'c6_backup_required', 'role switch refused with c6_backup_required')


def tc03_c6_backup():
    """Full C6 flash dump over JP1 (also proves the jumpers and the ROM loader link)."""
    t0 = time.time()
    code, data = http('GET', '/api/c6/backup', timeout=400)
    check(code == 200, f'backup HTTP 200 (got {code} {data if code != 200 else ""})')
    check(isinstance(data, bytes) and len(data) >= 2 * 1024 * 1024, f'backup size {len(data)} B')
    check(data[0] == 0xE9, 'image header magic 0xE9 at offset 0 (C6 bootloader)')
    check(data[0x8000:0x8002] == b'\xaa\x50', 'partition table magic at 0x8000')
    app = data[0x10000 + 80:0x10000 + 112].rstrip(b'\0').decode(errors='replace')
    log(f'C6 application in the backup: {app or "unknown"}')
    os.makedirs(os.path.join(ROOT, 'backups'), exist_ok=True)
    path = os.path.join(ROOT, 'backups', f'c6_backup_{app or "unknown"}_{time.strftime("%Y%m%d_%H%M%S")}.bin')
    with open(path, 'wb') as f:
        f.write(data)
    log(f'saved {path}')
    log(f'backup took {time.time() - t0:.0f} s')
    check(wait_until('status after backup', status, 30)['c6']['adopted'], 'C6 marked as backed up')


def tc04_thread_br():
    """Thread border router: C6 gets ot_rcp, network forms, OTBR REST and CLI work."""
    switch('thread_br')
    check(status()['c6']['bundle'] == 'ot_rcp', 'C6 runs ot_rcp')

    def leader():
        code, t = http('GET', '/api/thread/status')
        return t if code == 200 and t.get('role') in ('leader', 'router') else None

    t = wait_until('Thread leader', leader, 90)
    check(True, f'Thread role {t["role"]}, network {t["network_name"]}, channel {t["channel"]}, RCP {t["rcp_version"]}')
    check(t['border_router']['routing'] == 'running', 'border routing running')
    code, d = http('GET', '/api/thread/dataset?reveal=1')
    check(code == 200 and len(d.get('tlvs', '')) > 40, 'dataset TLVs readable')
    CTX['dataset'] = d['tlvs']
    CTX['network_name'] = d['network_name']

    def br_services():
        code, t = http('GET', '/api/thread/status')
        br = t.get('border_router', {})
        if not br.get('border_agent'):
            return None
        # Only one border router per network runs SRP/NAT64; as a router next to another
        # primary border router they stay stopped/idle.
        if t.get('role') == 'leader' and (br.get('srp_server') != 'running' or br.get('nat64') != 'active'):
            return None
        return t

    # SRP, NAT64 and the border agent come up some 20 s after the node becomes leader.
    t = wait_until('border agent (and SRP/NAT64 when leader)', br_services, 60)
    br = t['border_router']
    check(True, f'border agent active, SRP {br["srp_server"]}, NAT64 {br["nat64"]} (role {t["role"]})')
    code, node = http('GET', '/node', port=8081)
    check(code == 200 and isinstance(node, dict), f'OTBR REST /node on port 8081 ({code})')
    code, ds = http('GET', '/node/dataset/active', port=8081)
    check(code == 200, f'OTBR REST active dataset ({code})')
    code, cli = http('POST', '/api/thread/cli', {'cmd': 'state', 'wait_ms': 300})
    check(code == 200 and ('leader' in cli['output'] or 'router' in cli['output']), f'web CLI state: {cli.get("output", "").split()[:1]}')


def tc05_zigbee_coordinator():
    """Zigbee coordinator: C6 gets zigbee_ncp, TCP bridge accepts a client, NCP answers after reset."""
    switch('zigbee_coordinator')
    s = status()
    check(s['c6']['bundle'] == 'zigbee_ncp', 'C6 runs zigbee_ncp')
    before = s['role_status']['dropped_bytes']
    http('POST', '/api/zigbee/reset', {})
    time.sleep(3)
    after = status()['role_status']
    check(after['dropped_bytes'] > before, f'NCP sent {after["dropped_bytes"] - before} B after reset (UART link alive)')
    check(after['uart_errors'] == 0, 'no UART errors')
    with socket.create_connection((ARGS.host, 6638), timeout=5) as sock:
        time.sleep(1)
        rs = status()['role_status']
        check(rs['client_connected'], f'TCP client registered ({rs.get("client")})')
    time.sleep(1)
    check(not status()['role_status']['client_connected'], 'client disconnect detected')


def tc06_zigbee_data_kept():
    """Leaving and re-entering the coordinator saves and restores its network data."""
    switch('thread_br')
    snaps = {x['bundle']: x for x in status()['c6'].get('snapshots', [])}
    check('zigbee_ncp' in snaps, f'snapshot of zigbee_ncp saved ({snaps.get("zigbee_ncp")})')
    gen = snaps['zigbee_ncp']['generation']
    t = wait_thread_running()
    check(t.get('network_name') == CTX.get('network_name', t.get('network_name')),
          'Thread network unchanged after a Zigbee detour')
    switch('zigbee_coordinator')
    text = logs_since(0)
    check('restored 0x' in text, 'zb_storage restored on return to the coordinator')
    snaps = {x['bundle']: x for x in status()['c6'].get('snapshots', [])}
    check(snaps['zigbee_ncp']['generation'] == gen, 'snapshot generation unchanged (no extra save)')


def tc07_zigbee_router():
    """Zigbee router: C6 gets the router firmware and reports status over the line protocol."""
    switch('zigbee_router')
    check(status()['c6']['bundle'] == 'zigbee_router', 'C6 runs zigbee_router')

    def router_status():
        rs = status()['role_status']
        return rs if rs.get('router') else None

    rs = wait_until('router status line', router_status, 40)
    r = rs['router']
    check(r['state'] in ('steering', 'idle', 'joined'), f'router state {r["state"]}, channel {r["channel"]}, ieee {r["ieee"]}')
    code, _ = http('POST', '/api/zigbee/command', {'cmd': 'status'})
    check(code == 200, 'command accepted')
    snaps = {x['bundle'] for x in status()['c6'].get('snapshots', [])}
    check('zigbee_ncp' in snaps, 'coordinator snapshot still kept while the router runs')


def tc08_thread_router():
    """Thread router: idle without dataset, joins after the dataset is applied."""
    switch('thread_router')
    check(status()['c6']['bundle'] == 'ot_rcp', 'C6 runs ot_rcp')
    t = wait_thread_running()
    check(t['mode'] == 'router', 'router mode')
    if 'dataset' not in CTX:
        raise Fail('no dataset from TC04 to apply')
    code, r = http('POST', '/api/thread/dataset', {'tlvs': CTX['dataset']})
    check(code == 200, f'dataset applied ({r})')

    def attached():
        code, t = http('GET', '/api/thread/status')
        return t if t.get('role') not in (None, 'disabled', 'detached') else None

    t = wait_until('Thread attach', attached, 120, 5)
    check(True, f'Thread router role {t["role"]} on {t["network_name"]}')


def tc09_esphome_radio():
    """ESPHome Bluetooth proxy as the role of a radio (radio B when present, else A): the ESPHome bundle is
    flashed, the device answers over Improv; with Wi-Fi set (ESPHome page) it joins and
    reports its URL. The radio's role comes back afterwards."""
    b = (http('GET', '/api/radios')[1] or [{}, {}])[1]
    radio = 'b' if b.get('wired') else 'a'
    s0 = status()
    before = s0['role']['b']['desired'] if radio == 'b' else s0['role']['active']
    wifi = http('GET', '/api/esphome/wifi')[1]
    try:
        if radio == 'b':
            if s0['role']['active'] == 'esphome':
                switch('thread_br')
            switch_b('esphome', timeout=400)
        else:
            if s0['role'].get('b', {}).get('desired') == 'esphome':
                switch_b('none')
            switch('esphome', timeout=400)

        def improv():
            rs = status().get('role_status_b' if radio == 'b' else 'role_status') or {}
            return rs if rs.get('mode') == 'esphome' and not rs.get('busy') else None
        rs = wait_until('Improv of the ESPHome radio', improv, 150, 5)
        log(f'radio {radio.upper()}: {rs.get("device")} improv={rs.get("improv")} {rs.get("note")} {rs.get("url")}')
        check('ESPHome' in rs.get('device', '') or 'p4gw' in rs.get('device', ''), 'the radio runs ESPHome and answers over Improv')
        if wifi.get('ssid'):
            # the proxy has no web server, so Improv names no URL: provisioned means it joined
            check(rs.get('improv') == 'provisioned', f'joined Wi-Fi {wifi["ssid"]} ({rs.get("note")})')
        else:
            check(rs.get('improv') == 'not provisioned', 'not provisioned without Wi-Fi settings')
            log('Wi-Fi not set: the join is not tested')
    finally:
        if radio == 'b':
            switch_b(before)
        elif status()['role']['active'] != before:
            switch(before)


def tc10_ota_rollback_slots():
    """Gateway OTA: new image goes to the other slot and confirms itself after 60 s."""
    s = status()
    running = next(x['label'] for x in s['slots'] if x['running'])
    with open(os.path.join(ROOT, 'dist', 'p4gw.bin'), 'rb') as f:
        image = f.read()
    code, r = http('POST', '/api/ota/gateway', raw=image, timeout=120)
    check(code == 200 and r.get('restarting'), f'upload accepted into {r.get("slot")}')
    check(r['slot'] != running, 'written to the inactive slot')
    s = wait_restart(s['role']['active'])
    slot = next(x for x in s['slots'] if x['running'])
    check(slot['label'] == r['slot'] and slot['state'] == 'pending_verify', 'boots the new slot, pending verify')
    time.sleep(70)
    slot = next(x for x in status()['slots'] if x['running'])
    check(slot['state'] == 'valid', 'confirmed valid after 60 s')


def tc11_auth():
    """An admin account protects the API (Basic auth for clients of older versions); deleting it opens the gateway again."""
    if http('GET', '/api/auth/state')[1]['enabled']:
        log('skipped: the gateway has user accounts (TC11 needs an open gateway)')
        return
    pw = 'hwtest-' + str(int(time.time()))
    check(http('POST', '/api/settings', {'password': pw})[0] == 400, 'the old single password is refused (user management instead)')
    check(http('POST', '/api/users', {'name': 'hwtest', 'role': 'admin', 'password': pw})[0] == 200, 'admin account created')
    try:
        check(http('GET', '/api/status')[0] == 401, '401 without credentials')
        code, st = http('GET', '/api/status', auth=pw, user='hwtest')
        check(code == 200, '200 with credentials')
        check(st['auth'] is True, 'status reports sign-in')
        check(http('GET', '/api/status', auth='wrong', user='hwtest')[0] == 401, '401 with a wrong password')
    finally:
        check(http('POST', '/api/users/delete', {'name': 'hwtest'}, auth=pw, user='hwtest')[0] == 200, 'account deleted')
    check(http('GET', '/api/status')[0] == 200, 'open again')
    check(status()['auth'] is False, 'status reports no sign-in')


def tc12_invalid_uploads():
    """Broken uploads are rejected and do not damage the stored images."""
    count = len(status()['c6']['images'])
    code, r = http('POST', '/api/ota/c6fw', raw=b'\x00' * 8192)
    check(code == 400, f'garbage c6fw rejected ({r})')
    check(len(status()['c6']['images']) == count, f'stored bundles intact after the rejected upload ({count})')
    code, r = http('POST', '/api/ota/gateway', raw=b'\xe9' + b'\x00' * 8191)
    check(code == 400, f'garbage gateway image rejected ({r})')
    with open(os.path.join(ROOT, 'dist', 'c6fw.bin'), 'rb') as f:
        code, r = http('POST', '/api/ota/c6fw', raw=f.read(), timeout=120)
    check(code == 200, 'valid c6fw re-uploaded')
    check(len(status()['c6']['images']) == count, f'{count} bundles listed again')


def tc23_slot_switch():
    """C6 slots layout: a role switch only selects the slot, the Zigbee router keeps its network."""
    check('slots' in {i['name'] for i in status()['c6']['images']}, 'c6fw carries the slots layout')
    if status()['role']['active'] != 'thread_br':
        switch('thread_br')
    times = {}
    for role in ('zigbee_router', 'thread_br'):
        t0 = time.time()
        switch(role)
        times[role] = time.time() - t0
        text = logs_since(0)
        check('installing the slots layout' not in text, f'{role}: layout already on the C6, not written again')
        check(f'C6 now runs {"ot_rcp" if role == "thread_br" else role}' in text, f'{role}: slot selected')
        if role == 'zigbee_router':
            r = wait_until('router joined', lambda: (lambda rs: rs if (rs.get('router') or {}).get('joined') else None)(status()['role_status'] or {}), 60)['router']
            check(True, f'router back in its network: pan {r["pan_id"]}, channel {r["channel"]}')
        else:
            rs = wait_until('Thread attached', lambda: (lambda rs: rs if rs.get('role') in ('router', 'leader', 'child') else None)(status()['role_status'] or {}), 60)
            check(True, f'Thread attached as {rs["role"]}')
    check(all(t < 60 for t in times.values()), 'switches done in under a minute: ' + ', '.join(f'{k} {v:.0f} s' for k, v in times.items()))


def tc24_previous_log():
    """Log before the last restart: kept in RAM over a requested restart, ends with the restart."""
    http('POST', '/api/reboot', {})
    wait_restart(status()['role']['desired'])
    code, text = http('GET', '/api/logs/previous')
    text = text.decode(errors='replace') if isinstance(text, bytes) else str(text)
    check(code == 200 and len(text) > 1000, f'previous log {len(text)} B')
    check('restart in' in text[-3000:], 'ends with the restart request of the previous run (other tasks may log in its last 500 ms)')
    check('restart in' not in logs_since(), 'the current log starts fresh')


def tc25_settings_backup():
    """Settings backup: export without board state, refused imports change nothing, import restores."""
    code, full = http('GET', '/api/settings/export?secrets=1')
    check(code == 200 and full.get('format') == 'p4gw-settings' and full['secrets'], f'export with secrets, {len(full["keys"])} keys')
    board = {'bootcnt', 'gw_slot', 'c6_name', 'c6_sha', 'c6_force', 'c6_tries', 'c6_ok', 'ha_seen', 'fail_rst', 'netscan'}
    check(not board & set(full['keys']), 'no per-board state keys')
    check('role' in full['keys'] and 'br_dataset' in full['keys'], 'role and Thread dataset included')
    code, plain = http('GET', '/api/settings/export')
    check(not {'password', 'mqtt_pass'} & set(plain['keys']) and not plain['secrets'], 'passwords only with secrets=1')

    def keys_now():
        return http('GET', '/api/settings/export?secrets=1')[1]['keys']

    test_url = 'ws://192.0.2.1:5580/ws'
    bad = dict(full, keys=dict(full['keys'], matter_url={'str': test_url}, zz_bad={'blob': '***'}))
    code, r = http('POST', '/api/settings/import', bad)
    check(code == 400 and 'zz_bad' in r.get('error', ''), f'invalid base64 refused: {r.get("error")}')
    code, r = http('POST', '/api/settings/import', dict(full, keys={'role': {'u8': 99}}))
    check(code == 400, f'unknown role refused: {r.get("error")}')
    code, r = http('POST', '/api/settings/import', {'format': 'other', 'keys': {}})
    check(code == 400, 'other format refused')
    check(keys_now() == full['keys'], 'refused imports changed nothing')

    flashes = status()['c6']['chip']['flashes']
    changed = dict(full, keys=dict(full['keys'], matter_url={'str': test_url}, c6_force={'u8': 1}))
    code, r = http('POST', '/api/settings/import', changed)
    check(code == 200 and r.get('restarting'), f'import accepted ({r})')
    wait_restart(status()['role']['desired'])
    now = keys_now()
    check(now['matter_url']['str'] == test_url, 'imported value active after the restart')
    check(status()['c6']['chip']['flashes'] == flashes, 'board key in the file ignored (no C6 reflash)')
    code, r = http('POST', '/api/settings/import', full)
    wait_restart(status()['role']['desired'])
    check(keys_now() == full['keys'], 'original settings restored')


def tc26_zigbee_backup():
    """Zigbee backup on request: refused outside Zigbee, router data saved with its time, router rejoins."""
    if status()['role']['active'] != 'thread_br':
        switch('thread_br')
    code, r = http('POST', '/api/c6/zigbee-snapshot')
    check(code == 409, f'refused in the Thread role ({r.get("error")})')
    check(http('POST', '/api/settings', {'zigbee_backup_days': 3})[0] == 400, 'interval other than 0, 1, 7 refused')
    check(http('POST', '/api/settings', {'zigbee_backup_days': 7})[0] == 200 and status()['c6']['snapshot_days'] == 7, 'weekly set')
    switch('zigbee_router')
    wait_until('router joined', lambda: (lambda rs: rs if (rs.get('router') or {}).get('joined') else None)(status()['role_status'] or {}), 60)
    snap = lambda: next((x for x in status()['c6']['snapshots'] if x['bundle'] == 'zigbee_router'), {})
    before = snap()
    flashes = status()['c6']['chip']['flashes']
    t0 = time.time()
    code, r = http('POST', '/api/c6/zigbee-snapshot')
    check(code == 200 and r.get('restarting'), 'backup requested')
    wait_restart('zigbee_router')
    after = snap()
    check(after.get('generation', 0) == before.get('generation', 0) + 1, f'new generation {after.get("generation")}')
    check(abs(after.get('saved', 0) - time.time()) < 120, 'saved time is now')
    check(status()['c6']['chip']['flashes'] == flashes, 'nothing flashed')
    check('Zigbee network backup of radio A: ESP_OK' in logs_since(0), 'backup logged')
    r = wait_until('router joined', lambda: (lambda rs: rs if (rs.get('router') or {}).get('joined') else None)(status()['role_status'] or {}), 60)['router']
    check(True, f'router back in its network after {time.time() - t0:.0f} s: pan {r["pan_id"]}')
    switch('thread_br')
    wait_until('Thread attached', lambda: (lambda rs: rs if rs.get('role') in ('router', 'leader', 'child') else None)(status()['role_status'] or {}), 60)
    check(snap().get('generation') == after.get('generation') + 1, 'leaving the role still saves a snapshot')


def tc27_online_update_checks():
    """Online update: URL validation, no install without an offer, readable errors of a failed check."""
    def checked(url):
        http('POST', '/api/settings', {'update_url': url})
        return wait_until('check done', lambda: (lambda u: u if not u['checking'] else None)(status()['update']), 40, 0.5)

    before = status()['update']['url']
    check(http('POST', '/api/settings', {'update_url': 'ftp://x/m.json'})[0] == 400, 'non-HTTP URL refused')
    http('POST', '/api/settings', {'update_url': ''})
    check(http('POST', '/api/update/check')[0] == 409, 'check without a URL refused')
    check(http('POST', '/api/update/install')[0] == 409, 'install without an offer refused')
    u = checked(f'http://{ARGS.host}/nope.json')
    check(u['error'] == 'manifest: HTTP 404' and not u['available'], f'missing manifest: {u["error"]}')
    u = checked(f'http://{ARGS.host}/')  # the web UI page: no sign-in, not JSON
    check('expected' in u['error'] and not u['available'], f'not a manifest: {u["error"]}')
    u = checked('https://example.com/p4gw/manifest.json')
    check(u['error'] == 'manifest: HTTP 404', f'HTTPS reaches the server (certificate bundle): {u["error"]}')
    check(http('POST', '/api/update/install')[0] == 409, 'still nothing to install')
    http('POST', '/api/settings', {'update_url': before})
    check(status()['update']['url'] == before, f'URL back to {before or "off"}')


def _tone_wav(path, hz, seconds=1.5, lead=0.4):
    import math, random, struct, wave
    rate = 16000
    rnd = random.Random(hz)
    with wave.open(path, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        frames = [int(rnd.gauss(0, 60)) for _ in range(int(lead * rate))] + \
                 [int(8000 * math.sin(2 * math.pi * hz * i / rate) + rnd.gauss(0, 60)) for i in range(int(seconds * rate))]
        w.writeframes(struct.pack(f'<{len(frames)}h', *frames))


def tc28_learned_sounds():
    """Learned sounds from WAV uploads: tone and embedding profiles, background from the room, calibration, scoring."""
    import shutil, subprocess, tempfile
    tmp = tempfile.mkdtemp()
    wav = lambda name: os.path.join(tmp, name)
    _tone_wav(wav('beep.wav'), 2000)
    _tone_wav(wav('beep2.wav'), 2010, seconds=1.0)
    _tone_wav(wav('other.wav'), 900)
    clips = {os.path.basename(c): c for c in glob.glob(os.path.join(ROOT, 'tools/yamnet/data/esc50/*.wav'))}
    dogs = sorted(c for n, c in clips.items() if n.endswith('-0.wav'))
    others = sorted(c for n, c in clips.items() if n.split('-')[-1] in ('10.wav', '22.wav', '48.wav'))
    if len(dogs) < 3 or not others or not shutil.which('afconvert'):
        raise Fail('skipped: needs tools/yamnet/data/esc50 (scripts/build_model.sh) and afconvert (macOS)')
    conv = []
    for k, c in enumerate(dogs + others):
        out = wav(f'clip{k}.wav')
        subprocess.run(['afconvert', '-f', 'WAVE', '-d', 'LEI16@16000', '-c', '1', c, out], check=True)
        conv.append(out)
    dog_samples, dog_test, other_tests = conv[:2], conv[2], conv[3:]
    code, room = http('GET', '/api/sound/clip?seconds=6', timeout=30)
    check(code == 200 and room[:4] == b'RIFF', 'background: 6 s of this room from the microphone')
    with open(wav('room.wav'), 'wb') as f:
        f.write(room)

    learn = lambda: http('GET', '/api/sound/learn')[1]
    check(learn()['ready'], 'recognition runs')
    for pid in ('tc_beep', 'tc_dog'):
        http('DELETE', f'/api/sound/learn?profile={pid}')
    bg_before = {x['slot'] for x in learn()['background']}
    free_before = learn()['slots_free']

    def upload(path, query):
        with open(path, 'rb') as f:
            code, r = http('POST', '/api/sound/learn/upload?' + query, raw=f.read(), timeout=60)
        if code != 200:
            raise Fail(f'upload {os.path.basename(path)} ({query}): {code} {r}')
        return r

    code, r = http('POST', '/api/sound/learn/upload?kind=sample&profile=tc_x', raw=b'RIFF' + b'\0' * 100)
    check(code == 400, f'not a WAV refused ({r.get("error")})')
    code, r = http('POST', '/api/sound/learn/upload?kind=sample&profile=Bad-Id', raw=open(wav('beep.wav'), 'rb').read())
    check(code == 409, f'invalid name refused ({r.get("error")})')
    bg_new = []
    try:
        for f in (wav('beep.wav'), wav('beep2.wav'), wav('beep.wav')):
            upload(f, 'kind=sample&profile=tc_beep&type=tone')
        p = next(x for x in learn()['profiles'] if x['id'] == 'tc_beep')
        check(abs(p['tone_hz'] - 2003) <= 30 and p['samples'] == 3, f'tone profile: {p["tone_hz"]} Hz from {p["samples"]} samples')
        code, r = http('POST', '/api/sound/learn/upload?kind=sample&profile=tc_beep&type=embedding', raw=open(wav('beep.wav'), 'rb').read())
        check(code == 409, 'a sample of the other type refused')
        for f in dog_samples:
            upload(f, 'kind=sample&profile=tc_dog&type=embedding')
        r = upload(wav('room.wav'), 'kind=background')
        l = learn()
        bg_new = [x['slot'] for x in l['background'] if x['slot'] not in bg_before]
        check(len(bg_new) == 5 and l['slots_free'] == free_before - 3 - 2 - 5, f'{r["result"]}, {l["slots_free"]} slots free')
        prof = {p['id']: p for p in l['profiles'] if p['id'].startswith('tc_')}
        for p in prof.values():
            check(p['separable'] and p['background_max'] < p['threshold'] <= p['self_min'],
                  f'{p["id"]}: samples alike {p["self_min"]}, background {p["background_max"]}, threshold {p["threshold"]}')
        code, w = http('GET', f'/api/sound/learn/sample?slot={prof["tc_beep"]["slots"][0]["slot"]}')
        check(code == 200 and w[:4] == b'RIFF' and len(w) == 44 + 2 * 15600, 'a sample plays back as WAV')

        score = lambda f: upload(f, 'test=1')['scores']
        sc = score(wav('beep2.wav'))
        check(sc['tc_beep'] >= prof['tc_beep']['threshold'], f'the beep scores {sc["tc_beep"]} for tc_beep')
        sc = score(wav('other.wav'))
        check(sc['tc_beep'] < prof['tc_beep']['threshold'], f'a 900 Hz tone scores {sc["tc_beep"]} for tc_beep')
        sc = score(dog_test)
        check(sc['tc_dog'] >= prof['tc_dog']['threshold'], f'another dog recording scores {sc["tc_dog"]} for tc_dog')
        worst = max(score(f)['tc_dog'] for f in other_tests)
        check(worst < prof['tc_dog']['threshold'], f'rain, clapping, fireworks score at most {worst} for tc_dog')

        events = http('GET', '/api/sound/events')[1]
        saved = {k: events[k] for k in ('preset', 'exclude', 'events')}
        ev = {'id': 'tc_beep', 'name': 'tc beep', 'kind': 'impulse', 'threshold': prof['tc_beep']['threshold'], 'windows': 1,
              'hold_s': 2, 'cooldown_s': 0, 'classes': [], 'suppress': [], 'profile': 'tc_beep'}
        try:
            code, r = http('PUT', '/api/sound/events', dict(saved, events=saved['events'] + [ev]))
            got = next((e for e in r.get('events', []) if e['id'] == 'tc_beep'), {})
            check(code == 200 and got.get('profile') == 'tc_beep', 'event on the learned tone saved with its profile')
            code, r = http('PUT', '/api/sound/events', dict(saved, events=saved['events'] + [dict(ev, profile='Bad-Id')]))
            check(code == 400, f'invalid profile in an event refused ({r.get("error")})')
        finally:
            http('PUT', '/api/sound/events', saved)
    finally:
        for pid in ('tc_beep', 'tc_dog'):
            http('DELETE', f'/api/sound/learn?profile={pid}')
        for slot in bg_new:
            http('DELETE', f'/api/sound/learn?slot={slot}')
    check(learn()['slots_free'] == free_before, 'test samples removed')


def tc29_mqtt_options():
    """MQTT connection options against a test broker (--mqtt-test host:dir): WebSocket, TLS with own CA and
    name check, client ID, base topic, interval, discovery off and on, Home Assistant birth; then the original settings."""
    if not ARGS.mqtt_test:
        raise Fail('skipped: needs --mqtt-test <host>:<dir with ca.crt, other.crt> (mosquitto on 18883, 8883, 1884, 8884)')
    import subprocess
    host, cert_dir = ARGS.mqtt_test.split(':', 1)
    ca, other = (open(os.path.join(cert_dir, f)).read() for f in ('ca.crt', 'other.crt'))
    code, backup = http('GET', '/api/settings/export?secrets=1')
    check(code == 200, 'original settings exported (restored by import at the end)')
    defaults = {'tls': 'bundle', 'ca': '', 'skip_cn_check': False, 'client_id': '', 'base': '', 'keepalive': 30,
                'interval': 30, 'discovery': True}

    def mqtt(ok=True, **cfg):
        code, r = http('POST', '/api/mqtt', cfg)
        if code != 200:
            raise Fail(f'{cfg}: {code} {r}')
        # a failure has to show its error; a success may follow an error of the previous client
        done = (lambda m: m if m['connected'] else None) if ok else (lambda m: m if m['last_error'] else None)
        return wait_until('connected' if ok else 'connection error', lambda: done(http('GET', '/api/mqtt')[1]), 25, 1)

    def sub(topic, seconds, count=1):
        out = subprocess.run(['mosquitto_sub', '-h', host, '-p', '18883', '-t', topic, '-v', '-W', str(seconds), '-C', str(count)],
                             capture_output=True, text=True)
        return out.stdout.strip().splitlines()

    try:
        for bad, why in ((dict(uri='http://x'), 'http URI'), (dict(prefix='home/#'), 'wildcard prefix'), (dict(base='p4gw/x/'), 'base with outer slash'),
                         (dict(keepalive=5), 'keepalive 5 s'), (dict(interval=5), 'interval 5 s'), (dict(tls='ca', ca='x'), 'CA not PEM'),
                         (dict(tls='insecure'), 'unknown TLS mode')):
            check(http('POST', '/api/mqtt', bad)[0] == 400, f'refused: {why}')
        m = mqtt(uri=f'ws://{host}:1884', user='', password='', **defaults)
        check(m['connected'], 'MQTT over WebSocket connects')
        m = mqtt(False, uri=f'mqtts://{host}:8883')
        check(not m['connected'] and 'public CA' in m['last_error'], f'self-signed broker refused with the public CA bundle: {m["last_error"]}')
        m = mqtt(False, tls='ca', ca=other)
        check(not m['connected'] and 'not accepted' in m['last_error'], f'wrong CA refused: {m["last_error"]}')
        m = mqtt(False, tls='ca', ca=ca)
        check(not m['connected'] and 'not accepted' in m['last_error'], f'right CA, but the IP is not the certificate name: {m["last_error"]}')
        check(http('POST', '/api/mqtt', {'tls': 'ca', 'ca': ''})[0] == 200 and http('GET', '/api/mqtt')[1]['ca'] == '',
              'certificate can be cleared')
        check(http('POST', '/api/mqtt', {'tls': 'ca'})[0] == 400, 'own CA without a certificate refused')
        m = mqtt(tls='ca', ca=ca, skip_cn_check=True)
        check(m['connected'], 'own CA with the name check off connects over TLS')
        m = mqtt(uri=f'wss://{host}:8884')
        check(m['connected'], 'MQTT over secure WebSocket connects')

        m = mqtt(uri=f'mqtt://{host}:18883', client_id='tc29-client', base='p4gw/tc29', keepalive=20, interval=10)
        check(m['connected'] and m['base_topic'] == 'p4gw/tc29', f'base topic {m["base_topic"]}')
        t0 = time.time()
        state = sub('p4gw/tc29/state', 30, 3)  # the retained one at once, then every 10 s
        check(len(state) == 3 and time.time() - t0 < 26, f'state on the new base topic every ~10 s ({time.time() - t0:.0f} s for 3)')
        log = subprocess.run(['docker', 'logs', '--since', '2m', 'p4gw-mqtt-test'], capture_output=True, text=True).stderr + \
              subprocess.run(['docker', 'logs', '--since', '2m', 'p4gw-mqtt-test'], capture_output=True, text=True).stdout
        check('tc29-client' in log and 'k20' in log, 'broker saw client ID tc29-client with keepalive 20 s')
        disc = sub(f'homeassistant/sensor/{status()["hostname"]}/uptime/config', 5)
        check(disc and 'p4gw/tc29/state' in disc[0], 'discovery points to the new base topic')
        mqtt(discovery=False)
        time.sleep(1)
        disc = sub(f'homeassistant/sensor/{status()["hostname"]}/uptime/config', 3)
        check(not disc or disc[0].endswith('config '), f'discovery off: entity removed (empty retained config)')
        mqtt(discovery=True)
        subprocess.run(['mosquitto_pub', '-h', host, '-p', '18883', '-t', f'homeassistant/sensor/{status()["hostname"]}/uptime/config', '-r', '-n'])
        subprocess.run(['mosquitto_pub', '-h', host, '-p', '18883', '-t', 'homeassistant/status', '-m', 'online'])
        disc = sub(f'homeassistant/sensor/{status()["hostname"]}/uptime/config', 10)
        check(disc and 'p4gw/tc29/state' in disc[0], 'Home Assistant birth (online) sends the discovery again')
    finally:
        http('POST', '/api/mqtt', defaults)
        code, r = http('POST', '/api/settings/import', backup)
        check(code == 200, 'original settings imported, the gateway restarts')
        wait_restart(status()['role']['desired'])
    m = wait_until('MQTT back on the original broker', lambda: (lambda m: m if m['connected'] else None)(http('GET', '/api/mqtt')[1]), 40, 2)
    check(m['uri'] == backup['keys']['mqtt_uri']['str'] and m['tls'] == 'bundle' and m['base'] == '', f'original broker {m["uri"]} connected')


def tc46_mqtt_discovery_groups():
    """MQTT discovery groups against a plain test broker (--mqtt-broker host:port, anonymous): every group
    by default, a left out group is removed from Home Assistant (empty retained config), the preset of
    the P4 Gateway integration keeps only the device triggers of the sound events; then the original settings."""
    if not ARGS.mqtt_broker:
        raise Skip('needs --mqtt-broker <host>:<port> (anonymous mosquitto, e.g. mosquitto -p 18883)')
    import subprocess
    host, port = ARGS.mqtt_broker.rsplit(':', 1)
    code, backup = http('GET', '/api/settings/export?secrets=1')
    check(code == 200, 'original settings exported (restored by import at the end)')
    groups_before = http('GET', '/api/mqtt')[1]['discovery_groups']
    name = status()['hostname']

    def retained(component, object_id):
        out = subprocess.run(['mosquitto_sub', '-h', host, '-p', port, '-t', f'homeassistant/{component}/{name}/{object_id}/config',
                              '-v', '-W', '3', '-C', '1'], capture_output=True, text=True).stdout.strip()
        return out.split(' ', 1)[1] if ' ' in out else ''

    def connected():
        return wait_until('connected', lambda: (lambda m: m if m['connected'] else None)(http('GET', '/api/mqtt')[1]), 25, 1)

    try:
        for bad in (['bogus'], 'roles', [1]):
            check(http('POST', '/api/mqtt', {'discovery_groups': bad})[0] == 400, f'refused: discovery_groups {bad!r}')
        all_groups = ['roles', 'system', 'update', 'sound', 'sound_events', 'sound_triggers', 'sensors', 'modbus', 'gpio']
        # an I2C sensor to expose (need not be connected: its entity exists, unavailable)
        sensors = http('GET', '/api/sensors')[1]['sensors']
        if not any(x['addr'] == 0x76 for x in sensors):
            sensors = sensors + [{'addr': 0x76, 'type': 'bme280', 'name': 'TC46'}]
        check(http('PUT', '/api/sensors', {'sensors': [dict(x, expose=True, enabled=True) for x in sensors]})[0] == 200, 'I2C sensor at 0x76 exposed')
        code, _ = http('POST', '/api/mqtt', {'uri': f'mqtt://{host}:{port}', 'user': '', 'password': '', 'tls': 'bundle',
                                             'base': '', 'prefix': 'homeassistant', 'discovery': True,
                                             'discovery_groups': all_groups, 'enabled': True})
        check(code == 200 and connected()['discovery_groups'] == all_groups, 'test broker, every group')
        time.sleep(2)
        entities = {('sensor', 'role'): 'roles', ('sensor', 'uptime'): 'system', ('update', 'firmware'): 'update',
                    ('sensor', 'i2c_76_temperature'): 'sensors'}
        check(all(retained(c, o) for c, o in entities), 'role, uptime, firmware and I2C sensor entities published')
        cfg = json.loads(retained('sensor', 'i2c_76_temperature'))
        check(cfg['device_class'] == 'temperature' and cfg['availability_mode'] == 'all' and len(cfg['availability']) == 2
              and 'availability_topic' not in cfg, 'I2C sensor entity: temperature, unavailable without a value or without the gateway')
        events = subprocess.run(['mosquitto_sub', '-h', host, '-p', port, '-t', f'homeassistant/binary_sensor/{name}/+/config',
                                 '-v', '-W', '3'], capture_output=True, text=True).stdout.split()
        sound_events = [t.split('/')[3] for t in events if t.startswith('homeassistant/') and '/snd_' in t]
        check(http('POST', '/api/mqtt', {'discovery_groups': ['sound_triggers']})[0] == 200, 'preset: only what the integration lacks')
        check(connected()['discovery_groups'] == ['sound_triggers'], 'stored: sound_triggers')
        time.sleep(2)
        check(all(not retained(c, o) for c, o in entities), 'role, uptime, firmware and I2C sensor entities removed (empty retained config)')
        if sound_events:
            check(not retained('binary_sensor', sound_events[0]), f'sound event binary sensor {sound_events[0]} removed')
            trigger = sound_events[0][:-len('_active')]
            check(retained('device_automation', trigger), f'device trigger {trigger} kept')
        else:
            log('no sound event configured: the kept group is not visible')
        check(http('POST', '/api/mqtt', {'discovery_groups': all_groups})[0] == 200 and connected(), 'every group again')
        time.sleep(2)
        check(all(retained(c, o) for c, o in entities), 'entities published again')
    finally:
        # the groups of before explicitly: a backup without the key of the groups would keep the test's
        http('POST', '/api/mqtt', {'discovery_groups': groups_before})
        code, r = http('POST', '/api/settings/import', backup)
        check(code == 200, 'original settings imported, the gateway restarts')
        wait_restart(status()['role']['desired'])
    m = wait_until('MQTT back on the original broker', lambda: (lambda m: m if m['connected'] or not m['enabled'] else None)(http('GET', '/api/mqtt')[1]), 40, 2)
    check(m['uri'] == backup['keys'].get('mqtt_uri', {}).get('str', m['uri']), f'original broker {m["uri"]}')
    check(m['discovery_groups'] == groups_before, f'discovery groups of before: {groups_before}')


def tc47_i2c_sensors():
    """I2C sensors on JP1: the scan finds the board's codec and leaves it alone, the configuration is checked
    and kept, a sensor that does not answer shows its error, the microphone keeps working on the shared bus.
    Sensors connected for real must give plausible values. The configuration of before comes back."""
    code, before = http('GET', '/api/sensors')
    check(code == 200 and before['bus']['ok'] and before['bus']['sda'] == 7 and before['bus']['scl'] == 8, 'bus on GPIO7/8')
    keep = [{k: x[k] for k in ('addr', 'type', 'chip', 'name', 'expose', 'enabled')} for x in before['sensors']]
    snd = http('GET', '/api/sound/status')[1]
    try:
        code, r = http('POST', '/api/sensors/scan', timeout=60)
        devices = {d['addr']: d for d in r['scan']['devices']}
        check(code == 200 and r['scan']['done'], f'scan: {", ".join(f"0x{a:02x}" for a in devices)}, added {r["added"]}')
        check(0x18 in devices and devices[0x18]['onboard'] and not devices[0x18]['candidates'], 'ES8311 codec at 0x18, not taken for a sensor')
        for a, d in devices.items():
            if a != 0x18 and not d['configured']:
                log(f'0x{a:02x}: {[c["chip"] + (" (sure)" if c["sure"] else "") for c in d["candidates"]] or d["hint"] or "unknown"}')
        for bad, why in ((0x18, 'codec address'), (0x40, 'address the type cannot have')):
            check(http('PUT', '/api/sensors', {'sensors': [{'addr': bad, 'type': 'bme280'}]})[0] == 400, f'refused: {why}')
        for body, why in (({'sensors': [{'addr': 0x76, 'type': 'x'}]}, 'unknown type'), ({'interval_s': 1}, 'interval 1 s'),
                          ({'sensors': [{'addr': 0x76, 'type': 'bme280'}, {'addr': 0x76, 'type': 'bmp280'}]}, 'address twice'),
                          ({'sensors': [{'addr': 0x76, 'type': 'bme280', 'name': 'x' * 33}]}, 'name of 33 characters')):
            check(http('PUT', '/api/sensors', body)[0] == 400, f'refused: {why}')
        absent = next(a for a in (0x77, 0x76) if a not in devices)
        name = 'Obývák ' + 'ž' * 25  # 32 characters, 57 bytes
        code, r = http('PUT', '/api/sensors', {'interval_s': 10, 'sensors': [{'addr': absent, 'type': 'bme280', 'name': name}]})
        check(code == 200 and r['interval_s'] == 10 and r['sensors'][0]['name'] == name and r['sensors'][0]['chip'] == 'BME280',
              'stored: interval 10 s, BME280 with a name of 32 characters with diacritics')
        s = wait_until('error of the sensor that does not answer', lambda: (lambda x: x if x['state'] == 'error' else None)(
            http('GET', '/api/sensors')[1]['sensors'][0]), 15, 1)
        check(not s['values'] and s['errors'] >= 1, f'no values, error "{s["error"]}"')
        if devices.keys() - {0x18}:
            r = http('PUT', '/api/sensors', {'sensors': keep})[1]
            r = http('POST', '/api/sensors/scan', timeout=60)[1]
            real = [x for x in r['sensors'] if x['addr'] in devices]
            limits = {'temperature': (-30, 60), 'humidity': (1, 100), 'pressure': (850, 1100), 'illuminance': (0, 150000),
                      'co2': (300, 10000), 'eco2': (400, 65000), 'tvoc': (0, 65000), 'aqi': (1, 5)}
            for x in real:
                v = wait_until(f'values of {x["chip"]} at 0x{x["addr"]:02x}', lambda: (lambda y: y if y and y['state'] == 'ok' else None)(
                    next((y for y in http('GET', '/api/sensors')[1]['sensors'] if y['addr'] == x['addr']), None)), 90, 3)
                odd = {k: val for k, val in v['values'].items() if k in limits and not limits[k][0] <= val <= limits[k][1]}
                check(not odd, f'{x["chip"]}: {v["values"]}' + (f', implausible {odd}' if odd else ''))
        snd2 = http('GET', '/api/sound/status')[1]
        check(snd2['running'] == snd['running'] and snd2.get('read_errors', 0) == snd.get('read_errors', 0),
              f'microphone {"running" if snd2["running"] else "off"} as before, no new read errors')
    finally:
        code, r = http('PUT', '/api/sensors', {'interval_s': before['interval_s'], 'sensors': keep})
        check(code == 200 and len(r['sensors']) == len(keep), 'configuration of before restored')


def tc48_modbus():
    """Modbus over TCP against the simulator of this computer (tools/modbus/sim_tcp.py, started by the test):
    every value of four templates as the simulator encodes them, writes of each kind with read back, refused
    writes, a manual request, the address search, a user template, an unreachable device going offline,
    configuration checks; with --mqtt-broker also the discovery of a device as its own device. The
    configuration of before comes back."""
    import subprocess
    here = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    here.connect((ARGS.host, 80))
    my_ip = here.getsockname()[0]
    here.close()
    port = 1502
    sim_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'modbus', 'sim_tcp.py')
    units = {1: 'eastron_sdm630', 2: 'peacefair_pzem016', 3: 'chint_dtsu666', 4: 'peacefair_pzem017'}
    sim = subprocess.Popen([sys.executable, sim_py, '--port', str(port)] + [f'{t}:{u}' for u, t in units.items()],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    code, before = http('GET', '/api/modbus')
    check(code == 200 and before['bus']['uart'] == 3 and before['bus']['tx'] == 26 and before['bus']['rx'] == 27, 'RS485 on UART3, GPIO26/27')
    keep = {'bus': {k: before['bus'][k] for k in ('baud', 'parity', 'stop', 'timeout_ms')},
            'devices': [{k: d[k] for k in ('id', 'name', 'template', 'transport', 'slave', 'host', 'port', 'poll_s', 'expose', 'enabled') if k in d}
                        for d in before['devices']]}
    tpl_id = 'tc48_meter'
    try:
        templates = {t['id']: t for t in http('GET', '/api/modbus/templates')[1]['templates']}
        check(all(t in templates and templates[t]['builtin'] for t in units.values()) and len(templates) >= 13,
              f'{len(templates)} templates, the four of the test built in')
        for bad, why in (({'devices': [{'template': 'nope'}]}, 'unknown template'),
                         ({'devices': [{'template': 'eastron_sdm630', 'transport': 'tcp'}]}, 'TCP without host'),
                         ({'devices': [{'template': 'eastron_sdm630', 'slave': 248}]}, 'slave 248'),
                         ({'bus': {'baud': 1234}}, 'baud 1234'), ({'devices': [{'template': 'eastron_sdm630', 'expose': 'x'}]}, 'expose x')):
            check(http('PUT', '/api/modbus', bad)[0] == 400, f'refused: {why}')
        devs = [{'name': f'TC48 {t}', 'template': t, 'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': u, 'poll_s': 3,
                 'expose': 'both' if u == 2 else 'integration'} for u, t in units.items()]
        devs.append({'name': 'TC48 nobody', 'template': 'eastron_sdm120', 'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': 9, 'poll_s': 2})
        code, r = http('PUT', '/api/modbus', {'devices': devs})
        check(code == 200 and len(r['devices']) == 5 and r['devices'][0]['name'] == devs[0]['name'], 'five devices configured')
        ids = {d['slave']: d['id'] for d in r['devices']}

        def values():
            return {d['slave']: d for d in http('GET', '/api/modbus')[1]['devices']}

        def close(a, b):
            return a == b or (isinstance(a, (int, float)) and not isinstance(a, bool) and isinstance(b, (int, float))
                              and abs(a - b) <= 1e-6 + abs(b) * 1e-6 + 0.011)

        for u, t in units.items():
            exp = json.loads(subprocess.run([sys.executable, sim_py, '--expect', t], capture_output=True, text=True).stdout)
            d = wait_until(f'values of {t}', lambda: (lambda d: d if len(d['values']) >= len(exp) else None)(values()[u]), 30, 2)
            bad = {k: (v, d['values'].get(k)) for k, v in exp.items() if not close(d['values'].get(k), v)}
            check(not bad and d['state'] == 'ok', f'{t}: all {len(exp)} values as encoded' + (f', different {bad}' if bad else ''))
        dev2, dev4 = ids[2], ids[4]
        for body, want in (({'device': dev2, 'key': 'alarm_threshold', 'value': 2500}, ('alarm_threshold', 2500)),
                           ({'device': dev4, 'key': 'shunt', 'value': '200 A'}, ('shunt', '200 A')),
                           ({'device': dev4, 'key': 'high_voltage_threshold', 'value': 48.5}, ('high_voltage_threshold', 48.5))):
            check(http('POST', '/api/modbus/write', body)[0] == 200, f'write {body["key"]} = {body["value"]}')
            u = 2 if body['device'] == dev2 else 4
            wait_until(f'{want[0]} read back', lambda: values()[u]['values'].get(want[0]) == want[1], 15, 1)
        check(http('POST', '/api/modbus/write', {'device': dev4, 'key': 'reset_energy'})[0] == 200, 'button with the device function 0x42')
        for body, why in (({'device': dev2, 'key': 'voltage', 'value': 1}, 'input register'), ({'device': dev2, 'key': 'alarm_threshold', 'value': 99999}, 'above max'),
                          ({'device': dev4, 'key': 'shunt', 'value': '7 A'}, 'not an option'), ({'device': 'zz', 'key': 'x', 'value': 1}, 'unknown device')):
            check(http('POST', '/api/modbus/write', body)[0] == 400, f'write refused: {why}')
        code, r = http('POST', '/api/modbus/raw', {'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': 2, 'fc': 4, 'addr': 0, 'count': 3})
        check(code == 200 and r['ok'] and len(r['registers']) == 3, f'manual read: {r.get("registers")}')
        code, r = http('POST', '/api/modbus/raw', {'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': 2, 'fc': 4, 'addr': 0x500, 'count': 1})
        check(code == 200 and not r['ok'] and r['result'] == 2, f'manual read of a missing register: {r.get("error")}')
        check(http('POST', '/api/modbus/probe', {'transport': 'tcp', 'host': my_ip, 'port': port, 'from': 1, 'to': 6})[0] == 200, 'search started')
        r = wait_until('search done', lambda: (lambda p: p if not p['running'] else None)(http('GET', '/api/modbus/probe')[1]), 60, 2)
        check(sorted(f['slave'] for f in r['found']) == [1, 2, 3, 4], f'search found units {[f["slave"] for f in r["found"]]}')
        d9 = wait_until('the unreachable device offline', lambda: (lambda d: d if d['state'] == 'offline' else None)(values()[9]), 40, 2)
        check(not d9['values'] and d9['error'], f'offline, no values, error "{d9["error"]}"')

        # a user template: a copy of the PZEM-016 one with its own id
        tpl = http('GET', '/api/modbus/template?id=peacefair_pzem016')[1]
        tpl.update(id=tpl_id, manufacturer='Test', model='TC48 meter')
        check(http('PUT', '/api/modbus/template', dict(tpl, id='eastron_sdm630'))[0] == 409, 'id of a built-in template refused')
        check(http('PUT', '/api/modbus/template', dict(tpl, entities=[]))[0] == 400, 'template without entities refused')
        code, r = http('PUT', '/api/modbus/template', tpl)
        check(code == 200 and not r['builtin'] and r['id'] == tpl_id, 'user template stored')
        devs2 = [{'id': d['id'], **{k: d[k] for k in ('name', 'template', 'transport', 'host', 'port', 'slave', 'poll_s', 'expose')}}
                 for d in http('GET', '/api/modbus')[1]['devices']]
        devs2.append({'name': 'TC48 user template', 'template': tpl_id, 'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': 2, 'poll_s': 3})
        check(http('PUT', '/api/modbus', {'devices': devs2})[0] == 200, 'device with the user template')
        exp = json.loads(subprocess.run([sys.executable, sim_py, '--expect', 'peacefair_pzem016'], capture_output=True, text=True).stdout)
        d = wait_until('values with the user template', lambda: (lambda d: d if d.get('values') else None)(
            next(x for x in http('GET', '/api/modbus')[1]['devices'] if x['template'] == tpl_id)), 20, 2)
        check(close(d['values'].get('voltage'), exp['voltage']), f'user template reads: voltage {d["values"].get("voltage")}')
        check(http('DELETE', f'/api/modbus/template?id={tpl_id}')[0] == 409, 'template in use not deleted')
        check(http('DELETE', '/api/modbus/template?id=eastron_sdm630')[0] == 409, 'built-in template not deleted')

        if ARGS.mqtt_broker:
            host, mport = ARGS.mqtt_broker.rsplit(':', 1)
            code, backup = http('GET', '/api/settings/export?secrets=1')
            groups_before = http('GET', '/api/mqtt')[1]['discovery_groups']
            name = status()['hostname']
            try:
                http('POST', '/api/mqtt', {'uri': f'mqtt://{host}:{mport}', 'user': '', 'password': '', 'tls': 'bundle', 'base': '',
                                           'prefix': 'homeassistant', 'discovery': True, 'enabled': True,
                                           'discovery_groups': ['roles', 'system', 'update', 'sound', 'sound_events', 'sound_triggers', 'sensors', 'modbus']})
                wait_until('MQTT connected', lambda: http('GET', '/api/mqtt')[1]['connected'], 25, 1)
                time.sleep(3)
                out = subprocess.run(['mosquitto_sub', '-h', host, '-p', mport, '-t', f'homeassistant/+/{name}/+/config', '-v', '-W', '3'],
                                     capture_output=True, text=True).stdout.strip().splitlines()
                cfgs = {line.split(' ', 1)[0].split('/')[3]: json.loads(line.split(' ', 1)[1]) for line in out
                        if ' ' in line and line.split(' ', 1)[1] and line.split('/')[3].startswith(f'mb_{dev2}_')}
                voltage, thr, reset = (cfgs.get(f'mb_{dev2}_{k}', {}) for k in ('voltage', 'alarm_threshold', 'reset_energy'))
                check(voltage.get('device', {}).get('via_device') == name and voltage['device']['model'] == 'PZEM-016 / PZEM-014',
                      f'{len(cfgs)} entities of the PZEM as its own device via the gateway')
                check(thr.get('min') == 1 and thr.get('command_topic', '').endswith(f'modbus/{dev2}/alarm_threshold/set') and reset.get('payload_press') == 'PRESS',
                      'number with limits and command topic, button')
                base = http('GET', '/api/mqtt')[1]['base_topic']
                subprocess.run(['mosquitto_pub', '-h', host, '-p', mport, '-t', f'{base}/modbus/{dev2}/alarm_threshold/set', '-m', '1234'])
                wait_until('MQTT write read back', lambda: next(d for d in http('GET', '/api/modbus')[1]['devices']
                                                                if d['id'] == dev2)['values'].get('alarm_threshold') == 1234, 15, 1)
                check(True, 'write over MQTT read back')
            finally:
                http('POST', '/api/mqtt', {'discovery_groups': groups_before})
                http('POST', '/api/settings/import', backup)
                wait_restart(status()['role']['desired'])
                check(http('GET', '/api/mqtt')[1]['discovery_groups'] == groups_before, f'MQTT discovery groups of before: {groups_before}')
    finally:
        http('PUT', '/api/modbus', keep)
        http('DELETE', f'/api/modbus/template?id={tpl_id}')
        sim.terminate()
        writes = [l for l in (sim.communicate(timeout=5)[0] or '').splitlines() if l.startswith(('write', 'function'))]
        log(f'simulator saw {len(writes)} writes')
    code, r = http('GET', '/api/modbus')
    check(code == 200 and len(r['devices']) == len(keep['devices']) and tpl_id not in [t['id'] for t in http('GET', '/api/modbus/templates')[1]['templates']],
          'configuration of before restored, test template deleted')


def tc49_modbus_scan():
    """Modbus TCP search in a network (/api/modbus/scan) against the simulator of this computer playing a Victron GX
    (system on unit 100, VE.Bus on 228) and an Eastron meter (unit 1): request checks, the simulator found and
    recognised by its serial number with both Victron units, an open port without Modbus (the gateway's web server)
    not listed, a /24 search refusing a second one and stopping, the Victron templates reading every value as the
    simulator encodes it. The configuration of before comes back."""
    import subprocess
    here = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    here.connect((ARGS.host, 80))
    my_ip = here.getsockname()[0]
    here.close()
    port = 1503
    sim_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'modbus', 'sim_tcp.py')
    units = {100: 'victron_gx_system', 228: 'victron_vebus', 1: 'eastron_sdm630'}
    sim = subprocess.Popen([sys.executable, sim_py, '--port', str(port)] + [f'{t}:{u}' for u, t in units.items()],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    before = http('GET', '/api/modbus')[1]
    keep = {'devices': [{k: d[k] for k in ('id', 'name', 'template', 'transport', 'slave', 'host', 'port', 'poll_s', 'expose', 'enabled') if k in d}
                        for d in before['devices']]}

    def search(body, limit=120):
        code, r = http('POST', '/api/modbus/scan', body)
        check(code == 200, f'search of {body["net"]} started')
        return wait_until(f'search of {body["net"]} done', lambda: (lambda r: r if not r['running'] else None)(http('GET', '/api/modbus/scan')[1]), limit, 2)

    try:
        time.sleep(1)
        r = http('GET', '/api/modbus/scan')[1]
        check(re.fullmatch(r'\d+\.\d+\.\d+\.0/(2[2-9]|3[0-2])', r['default_net'] or '') is not None, f'default network {r["default_net"]}')
        for bad, why in (({}, 'no net'), ({'net': 'x'}, 'net not an address'), ({'net': '10.0.0.0/21'}, 'more than 1024 addresses'),
                         ({'net': f'{my_ip}/32', 'ports': [1, 2, 3, 4, 5]}, 'five ports'), ({'net': f'{my_ip}/32', 'units': [300]}, 'unit 300')):
            check(http('POST', '/api/modbus/scan', bad)[0] == 400, f'refused: {why}')

        r = search({'net': f'{my_ip}/32', 'ports': [port], 'udp': False})
        f = r['found'][0] if len(r['found']) == 1 else {}
        check(r['addresses'] == 1 and r['open'] == 1 and f.get('host') == my_ip and f.get('port') == port, f'the simulator found: {r["found"]}')
        check(f.get('unit') == 1 and f.get('answer') == 2, 'first unit: the meter, exception 2 at holding register 0')
        check(f.get('manufacturer') == 'Victron Energy' and f.get('template') == 'victron_gx_system' and f.get('device_unit') == 100
              and f.get('serial') == 'SERIAL-01234' and f.get('vebus_unit') == 228, 'recognised as a Victron GX by its serial number, VE.Bus on unit 228')

        r = search({'net': f'{ARGS.host}/32', 'ports': [80], 'udp': False})
        check(r['open'] == 1 and not r['found'], 'the web server of the gateway: open, not Modbus, not listed')

        net = http('GET', '/api/modbus/scan')[1]['default_net']
        check(http('POST', '/api/modbus/scan', {'net': net, 'ports': [port]})[0] == 200, f'search of {net} started')
        check(http('POST', '/api/modbus/scan', {'net': net})[0] == 409, 'a second search refused while one runs')
        time.sleep(4)
        check(http('POST', '/api/modbus/scan', {'stop': True})[0] == 200, 'stop')
        r = wait_until('search stopped', lambda: (lambda r: r if not r['running'] else None)(http('GET', '/api/modbus/scan')[1]), 15, 1)
        check(r['stopped'] and r['progress'] < 100, f'stopped at {r["progress"]} %')

        devs = keep['devices'] + [{'name': f'TC49 {t}', 'template': t, 'transport': 'tcp', 'host': my_ip, 'port': port, 'slave': u,
                                   'poll_s': 3, 'expose': 'none'} for u, t in units.items() if t.startswith('victron')]
        check(http('PUT', '/api/modbus', {'devices': devs})[0] == 200, 'the two Victron devices added')
        for u, t in units.items():
            if not t.startswith('victron'):
                continue
            exp = json.loads(subprocess.run([sys.executable, sim_py, '--expect', t], capture_output=True, text=True).stdout)
            dev = lambda: next(d for d in http('GET', '/api/modbus')[1]['devices'] if d['host'] == my_ip and d['slave'] == u)
            d = wait_until(f'values of {t}', lambda: (lambda d: d if len(d['values']) >= len(exp) else None)(dev()), 30, 2)
            bad = {k: (v, d['values'].get(k)) for k, v in exp.items()
                   if not (d['values'].get(k) == v or isinstance(v, (int, float)) and abs(d['values'].get(k, 1e18) - v) <= 1e-6 + abs(v) * 1e-6)}
            check(not bad and d['state'] == 'ok', f'{t}: all {len(exp)} values as encoded' + (f', different {bad}' if bad else ''))
    finally:
        http('POST', '/api/modbus/scan', {'stop': True})
        http('PUT', '/api/modbus', keep)
        sim.terminate()
        sim.communicate(timeout=5)
    code, r = http('GET', '/api/modbus')
    check(code == 200 and len(r['devices']) == len(keep['devices']), 'configuration of before restored')

def tc50_modbus_goodwe_udp():
    """GoodWe Wi-Fi module transport (Modbus RTU over UDP 8899, answers with AA 55) against the simulator of this
    computer playing a GW3000-XS (template goodwe_xs, unit 127) that splits every answer into two datagrams:
    the network search lists it with unit 127, a manual request reads its serial number, a unit that does not
    exist times out, invalid devices are refused, and a device on transport udp reads every value as the simulator
    encodes it. The configuration of before comes back."""
    import subprocess
    here = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    here.connect((ARGS.host, 80))
    my_ip = here.getsockname()[0]
    here.close()
    sim_py = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'tools', 'modbus', 'sim_tcp.py')
    sim = subprocess.Popen([sys.executable, sim_py, '--udp', '--port', '8899', '--split', 'goodwe_xs:127'],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    before = http('GET', '/api/modbus')[1]
    keep = {'devices': [{k: d[k] for k in ('id', 'name', 'template', 'transport', 'slave', 'host', 'port', 'poll_s', 'expose', 'enabled') if k in d}
                        for d in before['devices']]}
    try:
        time.sleep(1)
        check(sim.poll() is None, 'simulator listens on UDP 8899 and 48899')
        check(http('POST', '/api/modbus/scan', {'net': f'{my_ip}/32', 'ports': [9], 'udp': True})[0] == 200, 'search with UDP started')
        r = wait_until('search done', lambda: (lambda r: r if not r['running'] else None)(http('GET', '/api/modbus/scan')[1]), 60, 2)
        u = r['udp'][0] if len(r['udp']) == 1 else {}
        check(u.get('host') == my_ip and u.get('unit') == 127 and 'Solar-WiFi-SIM' in u.get('info', ''), f'the module found with unit 127: {r["udp"]}')

        code, r = http('POST', '/api/modbus/raw', {'transport': 'udp', 'host': my_ip, 'slave': 127, 'fc': 3, 'addr': 30004, 'count': 8})
        text = ''.join(chr(v >> 8) + chr(v & 0xff) for v in r.get('registers', []))
        check(code == 200 and r.get('ok') and text == 'SERIAL-012345678', f'manual request over UDP: serial number {text!r}')
        code, r = http('POST', '/api/modbus/raw', {'transport': 'udp', 'host': my_ip, 'slave': 200, 'fc': 3, 'addr': 30004, 'count': 1})
        check(code == 200 and not r.get('ok') and r.get('result') == -1, f'a unit that does not exist: no answer ({r.get("error")})')
        check(http('POST', '/api/modbus/raw', {'transport': 'udp', 'slave': 127, 'fc': 3, 'addr': 0})[0] == 400, 'refused: udp without host')

        dev = {'name': 'TC50 GoodWe', 'template': 'goodwe_xs', 'transport': 'udp', 'host': my_ip, 'slave': 127, 'poll_s': 3, 'expose': 'none'}
        check(http('PUT', '/api/modbus', {'devices': keep['devices'] + [dict(dev, transport='xyz')]})[0] == 400, 'refused: unknown transport')
        check(http('PUT', '/api/modbus', {'devices': keep['devices'] + [{k: v for k, v in dev.items() if k != 'host'}]})[0] == 400, 'refused: udp without host')
        check(http('PUT', '/api/modbus', {'devices': keep['devices'] + [dev]})[0] == 200, 'GoodWe device on udp added')
        d = next(d for d in http('GET', '/api/modbus')[1]['devices'] if d['name'] == dev['name'])
        check(d['transport'] == 'udp' and d['port'] == 8899, f'stored as transport {d["transport"]}, default port {d.get("port")}')
        exp = json.loads(subprocess.run([sys.executable, sim_py, '--expect', 'goodwe_xs'], capture_output=True, text=True).stdout)
        get = lambda: next(d for d in http('GET', '/api/modbus')[1]['devices'] if d['name'] == dev['name'])
        d = wait_until('values of goodwe_xs', lambda: (lambda d: d if len(d['values']) >= len(exp) else None)(get()), 40, 2)
        bad = {k: (v, d['values'].get(k)) for k, v in exp.items()
               if not (d['values'].get(k) == v or isinstance(v, (int, float)) and abs(d['values'].get(k, 1e18) - v) <= 1e-6 + abs(v) * 1e-6)}
        check(not bad and d['state'] == 'ok', f'goodwe_xs: all {len(exp)} values as encoded' + (f', different {bad}' if bad else ''))
    finally:
        http('POST', '/api/modbus/scan', {'stop': True})
        http('PUT', '/api/modbus', keep)
        sim.terminate()
        sim.communicate(timeout=5)
    code, r = http('GET', '/api/modbus')
    check(code == 200 and len(r['devices']) == len(keep['devices']), 'configuration of before restored')


def tc13_rcp_reflash():
    """Forced C6 reflash from the UI restores the current role's firmware."""
    role = status()['role']['active']
    code, _ = http('POST', '/api/c6/reflash', {})
    check(code == 200, 'reflash requested')
    s = wait_restart(role)
    check(s['role']['active'] == role and not s['role']['maintenance'], f'{role} running again after reflash')
    text = logs_since(0)
    check('C6 now runs' in text, 'flash log present')
    if 'slots' in {i['name'] for i in s['c6']['images']}:
        check('installing the slots layout' in text, 'forced reflash writes the whole slots layout again')


def tc14_sound_service():
    """Sound service: microphone captures, YAMNet loads and runs within the CPU budget."""
    code, st = http('GET', '/api/sound/status')
    if not st.get('running'):
        http('POST', '/api/sound/config', {'enabled': True, 'inference': True})
        http('POST', '/api/reboot', {})
        wait_restart(status()['role']['desired'])
    st = wait_until('inference runs', lambda: (lambda s: s if s['inference']['runs'] >= 10 else None)(
        http('GET', '/api/sound/status')[1]), 120, 3)
    check(st['running'] and st['read_errors'] == 0, f'capturing, level {st["level_db"]} dBFS, floor {st["floor_db"]}')
    check(st['model']['loaded'], f'model loaded in {st["model"]["load_ms"]} ms')
    inf = st['inference']
    check(inf['avg_ms'] < 150, f'inference {inf["avg_ms"]} ms avg, {inf["max_ms"]} ms max')
    time.sleep(35)
    inf = http('GET', '/api/sound/status')[1]['inference']
    check(inf['enabled'] and inf['cpu_pct'] < 40, f'CPU {inf["cpu_pct"]} % of core 1, still enabled')
    code, wav = http('GET', '/api/sound/clip?seconds=2', timeout=30)
    check(code == 200 and isinstance(wav, bytes) and wav[:4] == b'RIFF' and len(wav) == 44 + 2 * 16000 * 2,
          '2 s WAV clip')


def tc15_sound_recognition():
    """Recognition of sounds played by this computer's speaker (needs --play, board within earshot)."""
    if not ARGS.play:
        raise Fail('skipped: run with --play next to the board')
    import subprocess

    def heard(names, seconds=15):
        end = time.time() + seconds
        best = {}
        while time.time() < end:
            for t in http('GET', '/api/sound/status')[1]['inference'].get('top', []):
                best[t['name']] = max(best.get(t['name'], 0), t['score'])
            time.sleep(1)
        return {n: best.get(n, 0) for n in names}

    subprocess.Popen(['say', 'Hello, this is a recognition test. One, two, three, four, five, six, seven.'])
    sc = heard(['Speech'])
    check(sc['Speech'] >= 0.6, f'speech {sc["Speech"]:.2f}')
    clips = sorted(glob.glob(os.path.join(ROOT, 'tools/yamnet/data/esc50/*-A-0.wav')))
    subprocess.Popen(['sh', '-c', ' ; '.join(f'afplay "{c}"' for c in clips)])
    sc = heard(['Dog', 'Bark'])
    check(max(sc.values()) >= 0.5, f'dog {sc["Dog"]:.2f}, bark {sc["Bark"]:.2f}')


def tc16_thread_dataset_isolation():
    """Border router keeps its own network after the router role joined another one."""
    switch('thread_br')
    br_net = http('GET', '/api/thread/status')[1]['network_name']

    def cli(cmd):
        return http('POST', '/api/thread/cli', {'cmd': cmd, 'wait_ms': 300})[1]['output']

    cli('dataset init new')
    cli('dataset networkname P4TEST')
    tlvs = cli('dataset tlvs').split()[0]
    cli('dataset clear')
    switch('thread_router')
    code, r = http('POST', '/api/thread/dataset', {'tlvs': tlvs})
    check(code == 200 and r.get('network_name') == 'P4TEST', 'router joined the test network')
    switch('thread_br')
    t = wait_until('border router network', lambda: (lambda t: t if t.get('network_name') else None)(
        http('GET', '/api/thread/status')[1]), 60)
    check(t['network_name'] == br_net, f'border router back on {br_net}')


def _events_config():
    cfg = http('GET', '/api/sound/events')[1]
    return {'preset': cfg['preset'], 'exclude': cfg['exclude'],
            'events': [{k: v for k, v in e.items() if k not in ('active', 'count', 'last_ago_s')} for e in cfg['events']]}


def tc17_sound_events_config():
    """Sound events: validation, save, survives a restart; the original configuration is restored."""
    original = _events_config()
    try:
        bad = [({'events': [{'id': 'Bad Id', 'classes': [0]}]}, 'invalid id'),
               ({'events': [{'id': 'a', 'classes': [999]}]}, 'class out of range'),
               ({'events': [{'id': 'a', 'classes': [0]}, {'id': 'a', 'classes': [1]}]}, 'duplicate id')]
        for body, what in bad:
            code, r = http('PUT', '/api/sound/events', body)
            check(code == 400, f'{what} rejected ({r.get("error") if isinstance(r, dict) else r})')
        cfg = json.loads(json.dumps(original))
        cfg['events'].append({'id': 'tc17_test', 'name': 'TC17 test', 'kind': 'impulse', 'threshold': 0.7,
                              'windows': 2, 'hold_s': 4, 'cooldown_s': 15, 'classes': [69, 70], 'suppress': [0]})
        cfg['exclude'] = sorted(set(cfg['exclude']) | {300})
        code, r = http('PUT', '/api/sound/events', cfg)
        check(code == 200 and any(e['id'] == 'tc17_test' for e in r['events']), 'event added')
        http('POST', '/api/reboot', {})
        wait_restart(status()['role']['desired'])
        after = _events_config()
        ev = next((e for e in after['events'] if e['id'] == 'tc17_test'), None)
        check(ev and ev['threshold'] == 0.7 and ev['classes'] == [69, 70] and ev['suppress'] == [0] and
              ev['windows'] == 2 and ev['cooldown_s'] == 15, 'event survived the restart unchanged')
        check(300 in after['exclude'], 'exclusion survived the restart')
        code, r = http('POST', '/api/sound/events/preset', {'preset': 'minimal'})
        check(code == 200 and [e['id'] for e in r['events']] ==
              ['doorbell', 'knock', 'smoke_alarm', 'alarm_siren', 'glass_break'], 'preset applied')
    finally:
        code, _ = http('PUT', '/api/sound/events', original)
        check(code == 200, 'original configuration restored')


def tc18_sound_events_detection():
    """Played cat sound raises the cat event; with Cat ignored it does not (needs --play)."""
    if not ARGS.play:
        raise Fail('skipped: run with --play next to the board')
    import subprocess
    clip = os.path.join(ROOT, 'tools/yamnet/data/events/1-34094-A-5.wav')
    if not os.path.exists(clip):
        raise Fail('missing ESC-50 cat clip, run tools/yamnet fetch (data/events)')
    original = _events_config()

    def cat_count():
        return next(e for e in http('GET', '/api/sound/events')[1]['events'] if e['id'] == 'cat')['count']

    try:
        cfg = json.loads(json.dumps(original))
        if not any(e['id'] == 'cat' for e in cfg['events']):
            cfg['events'].append({'id': 'cat', 'name': 'Cat', 'kind': 'impulse', 'threshold': 0.5, 'windows': 1,
                                  'hold_s': 5, 'cooldown_s': 0, 'classes': [76, 77, 78, 79, 80]})
        for e in cfg['events']:
            if e['id'] == 'cat':
                e['cooldown_s'] = 0
                e['enabled'] = True
        cfg['exclude'] = [c for c in cfg['exclude'] if c != 76]
        http('PUT', '/api/sound/events', cfg)
        time.sleep(8)
        before = cat_count()
        subprocess.run(['afplay', clip])
        time.sleep(8)
        check(cat_count() > before, 'cat event raised')
        cfg['exclude'] = sorted(set(cfg['exclude']) | {76, 77, 78, 79, 80})
        http('PUT', '/api/sound/events', cfg)
        time.sleep(8)
        before = cat_count()
        subprocess.run(['afplay', clip])
        time.sleep(8)
        check(cat_count() == before, 'no cat event while the cat sounds are ignored')
    finally:
        http('PUT', '/api/sound/events', original)


def _zboss_crc8_table():
    """Header crc8 table of esp-coordinator, taken from the gateway filter source."""
    import re
    src = open(os.path.join(ROOT, 'firmware/gateway/components/role_zigbee/zb_filter.c')).read()
    body = src.split('crc8_table[256] = {')[1].split('};')[0]
    return [int(x, 16) for x in re.findall(r'0x[0-9a-f]{2}', body)]


def _zboss_frames(data, table):
    """Splits a ZBOSS NCP byte stream into frames: (type, flags, body); raises on a bad header crc."""
    frames, i = [], 0
    while True:
        i = data.find(b'\xde\xad', i)
        if i < 0 or len(data) - i < 7:
            return frames
        crc = 0
        for b in data[i + 2:i + 6]:
            crc = table[crc ^ b]
        if crc != data[i + 6]:
            raise Fail(f'bad ZBOSS header crc at offset {i}: {data[i:i + 7].hex()}')
        total = data[i + 2] | data[i + 3] << 8
        frames.append((data[i + 4], data[i + 5], data[i + 9:i + 2 + total]))
        i += 2 + total


def _zboss_request(command_id, tsn, table):
    """ZBOSS NCP API request frame as zigbee-herdsman sends it (LL type 0x06, seq 1, single fragment)."""
    hl = bytes([0, 0, command_id & 0xff, command_id >> 8, tsn])
    crc16 = 0
    for b in hl:
        crc16 ^= b
        for _ in range(8):
            crc16 = (crc16 >> 1) ^ 0x8408 if crc16 & 1 else crc16 >> 1
    length = 5 + 2 + len(hl)
    hdr = bytes([length & 0xff, length >> 8, 0x06, 0xc4])
    crc = 0
    for b in hdr:
        crc = table[crc ^ b]
    return b'\xde\xad' + hdr + bytes([crc, crc16 & 0xff, crc16 >> 8]) + hl


def _telemetry(fw, timeout=90):
    return wait_until(f'{fw} telemetry', lambda: (lambda t: t if t and t.get('fw') == fw else None)(
        status()['c6'].get('telemetry')), timeout)


def tc19_c6_telemetry():
    """C6 telemetry in every gateway role; the Zigbee host stream stays clean and answers requests."""
    switch('thread_br')
    failures = http('GET', '/api/thread/status')[1].get('rcp_failures')
    t = _telemetry('ot_rcp')
    check(t.get('temp_c') is not None and 0 < t['temp_c'] < 100 and t['heap_free'] > 0,
          f'Thread RCP: {t["temp_c"]} °C, {t["heap_free"]} B free, up {t["uptime_s"]} s')
    time.sleep(35)
    check(http('GET', '/api/thread/status')[1].get('rcp_failures') == failures, 'no RCP failure while it is polled')

    switch('zigbee_coordinator')
    t = _telemetry('zigbee_ncp')
    check(t.get('temp_c') is not None and t['heap_free'] > 0, f'Zigbee NCP: {t["temp_c"]} °C, {t["heap_free"]} B free')
    table = _zboss_crc8_table()
    with socket.create_connection((ARGS.host, 6638), timeout=5) as sock:
        sock.sendall(_zboss_request(0x0001, 0x37, table))  # GET_MODULE_VERSION
        sock.settimeout(1)
        data, end = b'', time.time() + 35  # longer than the 30 s telemetry period
        while time.time() < end:
            try:
                chunk = sock.recv(4096)
                if not chunk:
                    break
                data += chunk
            except socket.timeout:
                pass
    frames = _zboss_frames(data, table)
    check(all(f[0] != 0x7e for f in frames), f'no telemetry frame reached the host ({len(frames)} frames)')
    resp = [f for f in frames if f[0] == 0x06 and len(f[2]) >= 7 and f[2][1] == 1 and f[2][2:4] == b'\x01\x00' and f[2][4] == 0x37]
    check(resp and resp[0][2][5:7] == b'\x00\x00', f'NCP answered GET_MODULE_VERSION ({resp[0][2].hex() if resp else "none"})')
    later = _telemetry('zigbee_ncp')
    check(later['uptime_s'] >= t['uptime_s'], 'telemetry keeps coming while a host is connected')

    switch('zigbee_router')
    t = _telemetry('zigbee_router')
    check(t.get('temp_c') is not None and t['heap_free'] > 0, f'Zigbee router: {t["temp_c"]} °C, {t["heap_free"]} B free')
    switch('thread_br')


def tc20_netscan():
    """Network surroundings: quick scan in the Thread role, full scan with Wi-Fi, role restored afterwards."""
    switch('thread_br')
    wait_thread_running()
    t = http('GET', '/api/thread/status')[1]

    def finished(mode, since):
        d = http('GET', '/api/net/scan', timeout=5)[1]
        last = d.get('last') or {}
        return last if d.get('state') == 'idle' and last.get('mode') == mode and last.get('duration_ms') != since else None

    before = (http('GET', '/api/net/scan')[1].get('last') or {}).get('duration_ms')
    code, r = http('POST', '/api/net/scan', {'mode': 'quick'})
    check(code == 200, f'quick scan started ({r})')
    q = wait_until('quick scan', lambda: finished('quick', before), 90, 3)
    check(len(q['channels']) == 16 and not q.get('errors'), f'quick: 16 channels in {q["duration_ms"]} ms')
    own = [n for n in q.get('thread', []) if n.get('name') == t['network_name']]
    check(own and own[0]['ch'] == t['channel'], f'quick: own network {t["network_name"]} found on channel {t["channel"]}')
    check(http('GET', '/api/thread/status')[1]['rcp_failures'] == t['rcp_failures'], 'no RCP failure caused by the scan')

    code, r = http('POST', '/api/net/scan', {'mode': 'full'})
    check(code == 200 and r.get('restarting'), 'full scan requested, gateway restarts')
    time.sleep(10)
    f = wait_until('full scan', lambda: finished('full', None), 240, 5)
    check(len(f['channels']) == 16 and f.get('scanner') and not f.get('errors'),
          f'full: scanner {f.get("scanner")}, 16 channels, {f["duration_ms"]} ms')
    bssids = [a['bssid'] for a in f['wifi']]
    check(len(bssids) == len(set(bssids)), f'full: {len(bssids)} Wi-Fi access points, one entry each')
    check(any(p['ch'] == t['channel'] for p in f['pans']), 'full: own Thread PAN seen in traffic')
    auths = {a['auth'] for a in f['wifi']}
    check(f.get('radio') == 'a' and all(a.get('bw') for a in f['wifi']) and 'other' not in auths,
          f'full: radio A, channel width and known security for every access point ({", ".join(sorted(auths))})')
    s = wait_restart('thread_br')
    check(s['c6']['bundle'] == 'ot_rcp' and not s['role']['maintenance'], 'role firmware back on the C6')
    t2 = wait_thread_running()
    check(t2['network_name'] == t['network_name'], 'Thread network unchanged after the scan')

    b = http('GET', '/api/radios')[1][1]
    if b.get('chip') != 'ESP32-C5':
        return
    code, r = http('POST', '/api/net/scan', {'mode': 'full', 'radio': 'b'})
    check(code == 200 and r.get('restarting'), 'full scan on radio B requested')
    time.sleep(10)
    f = wait_until('full scan on radio B', lambda: (lambda x: x if x and x.get('radio') == 'b' else None)(finished('full', None)), 300, 5)
    check(len(f['channels']) == 16 and not f.get('errors'), f'radio B: scanner {f.get("scanner")}, 16 channels, {f["duration_ms"]} ms')
    check(any(a['ch'] > 14 for a in f['wifi']), f'radio B: 5 GHz access points ({sum(a["ch"] > 14 for a in f["wifi"])} of {len(f["wifi"])})')
    wait_restart('thread_br')
    check(http('GET', '/api/radios')[1][1]['role'] == b['role'], f'radio B back in its role {b["role"]}')


def tc21_thread_topology():
    """Thread network map: topology discovery returns this gateway and the routers it links to."""
    switch('thread_br')
    wait_until('Thread router role', lambda: (lambda t: t if t.get('role') in ('router', 'leader') else None)(
        http('GET', '/api/thread/status')[1]), 180, 5)
    # after a start the border router flag appears once the gateway publishes its routes in the network data
    d = wait_until('this gateway in the map as border router', lambda: (lambda d: d if any(
        r['this'] and r['br'] for r in d.get('routers', [])) else None)(http('GET', '/api/thread/topology', timeout=40)[1]), 180, 10)
    check(d['routers'], f'{len(d["routers"])} routers in {d.get("ms")} ms ({d.get("error") or "all answered"})')
    me = [r for r in d['routers'] if r['this']]
    check(len(me) == 1 and me[0]['br'], f'this gateway is in the map as border router R{me[0]["id"] if me else "?"}')
    ids = {r['id'] for r in d['routers']}
    linked = {l['id'] for l in me[0]['links']}
    check(linked and linked <= ids | linked, f'its links: {sorted(linked)}')
    code, d2 = http('GET', '/api/thread/topology', timeout=40)
    check(code == 200, 'a second discovery right after works too')


def tc22_zigbee_net():
    """Zigbee network through Zigbee2MQTT: instance found on the broker, devices, an operation answered."""
    code, d = http('GET', '/api/zigbee/net')
    if code != 200 or not d.get('mqtt'):
        raise Fail('skipped: MQTT not connected')
    d = wait_until('Zigbee2MQTT instance', lambda: (lambda d: d if d.get('base') and d.get('devices') else None)(
        http('GET', '/api/zigbee/net')[1]), 60, 3)
    inst = next(i for i in d['instances'] if i['base'] == d['base'])
    check(True, f'Zigbee2MQTT {inst.get("version")} at {d["base"]}, coordinator {inst.get("coordinator")}, {len(d["devices"])} devices')
    check(any(x['type'] == 'Coordinator' for x in d['devices']), 'device list includes the coordinator')
    code, r = http('POST', '/api/zigbee/net/request', {'action': 'permit_join', 'time': 0}, timeout=40)
    check(code == 200 and r.get('status') == 'ok', f'permit join closed, answered by Zigbee2MQTT ({r.get("status")})')
    code, r = http('POST', '/api/zigbee/net/request', {'action': 'format'})
    check(code == 400, 'unknown action refused')


def mdns_query(service, timeout=3):
    """Answers to a one-shot mDNS PTR query for service (e.g. _p4gw._tcp.local), raw packets."""
    q = b'\x00\x00\x00\x00\x00\x01\x00\x00\x00\x00\x00\x00'
    q += b''.join(bytes([len(p)]) + p.encode() for p in service.split('.')) + b'\x00\x00\x0c\x00\x01'
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.settimeout(0.5)
    sock.sendto(q, ('224.0.0.251', 5353))
    answers, end = [], time.time() + timeout
    while time.time() < end:
        try:
            answers.append(sock.recvfrom(4096))
        except socket.timeout:
            pass
    sock.close()
    return answers


class WsClient:
    """Minimal WebSocket client (text frames from the server, no masking needed on receive)."""

    def __init__(self, path, auth=None, token=None, anonymous=False):
        import base64
        self.sock = socket.create_connection((ARGS.host, 80), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        hdr = f'GET {path} HTTP/1.1\r\nHost: {ARGS.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' \
              f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
        if auth:
            hdr += 'Authorization: Basic ' + base64.b64encode(auth.encode()).decode() + '\r\n'
        if token or (ARGS.token and not auth and not anonymous):
            hdr += f'Authorization: Bearer {token or ARGS.token}\r\n'
        self.sock.sendall((hdr + '\r\n').encode())
        resp = b''
        while b'\r\n\r\n' not in resp:
            chunk = self.sock.recv(1024)
            if not chunk:
                break
            resp += chunk
        self.status = int(resp.split(b' ')[1]) if resp.startswith(b'HTTP/1.1') else 0
        self.buf = resp.split(b'\r\n\r\n', 1)[1] if b'\r\n\r\n' in resp else b''

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise Fail('WebSocket closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def messages(self, seconds):
        """Text messages (parsed JSON) received within seconds."""
        out, end = [], time.time() + seconds
        while time.time() < end:
            self.sock.settimeout(max(0.1, end - time.time()))
            try:
                b0, b1 = self._read(2)
            except socket.timeout:
                break
            n = b1 & 0x7f
            if n == 126:
                n = int.from_bytes(self._read(2), 'big')
            elif n == 127:
                n = int.from_bytes(self._read(8), 'big')
            data = self._read(n)
            if b0 & 0x0f == 1:
                out.append(json.loads(data))
        return out

    def close(self):
        self.sock.close()


def tc30_ha_integration_interface():
    """Home Assistant integration: mDNS _p4gw._tcp with the MAC, WebSocket push of every sound message, password."""
    st = status()
    mac = st['eth']['mac'].encode()
    answers = mdns_query('_p4gw._tcp.local')
    check(any(b'mac=' + mac in a for a, _ in answers), f'mDNS _p4gw._tcp answers with mac={mac.decode()} ({len(answers)} packets)')

    original = _events_config()
    ws = WsClient('/api/ws')
    check(ws.status == 101, 'WebSocket handshake without a password')
    try:
        # "Silence" (class 494) tops every quiet window: the event fires at once
        cfg = json.loads(json.dumps(original))
        cfg['events'].append({'id': 'tc30_test', 'name': 'TC30 test', 'kind': 'impulse', 'threshold': 0.2, 'windows': 1,
                              'hold_s': 2, 'cooldown_s': 0, 'classes': [494], 'suppress': [], 'enabled': True})
        check(http('PUT', '/api/sound/events', cfg)[0] == 200, 'test event added')
        msgs = ws.messages(8)
        states = {m['topic'] for m in msgs if m['topic'].endswith('/state')}
        enabled = [e['id'] for e in cfg['events'] if e.get('enabled', True)]
        check(states >= {f'sound/{i}/state' for i in enabled},
              f'state of all {len(enabled)} enabled events pushed in one burst ({len(states)} received)')
        detected = [json.loads(m['payload']) for m in msgs if m['topic'] == 'sound/tc30_test']
        check(detected and detected[0]['event_type'] == 'detected', f'detection pushed: {detected[:1]}')
    finally:
        ws.close()
        http('PUT', '/api/sound/events', original)
    check('tc30_test' not in {e['id'] for e in http('GET', '/api/sound/events')[1]['events']}, 'events restored')

    pw = 'hwtest-' + str(int(time.time()))
    code, r = http('POST', '/api/users', {'name': 'hwtest', 'role': 'admin', 'password': pw})
    check(code == 200, 'admin account created')
    try:
        if r.get('must_change'):  # created by another admin: its own password first
            new = pw + '-own'
            check(http('POST', '/api/auth/password', {'old': pw, 'new': new}, auth=pw, user='hwtest')[0] == 200, 'own password set')
            pw = new
        ws = WsClient('/api/ws', anonymous=True)
        check(ws.status != 101, f'handshake without credentials refused ({ws.status})')
        ws.close()
        ws = WsClient('/api/ws', auth=f'hwtest:{pw}')
        check(ws.status == 101, 'handshake with credentials')
        ws.close()
        code, r = http('POST', '/api/tokens', {'name': 'hwtest', 'role': 'viewer'}, auth=pw, user='hwtest')
        ws = WsClient('/api/ws', token=r['token'])
        check(ws.status == 101, 'handshake with an API token')
        ws.close()
    finally:
        http('POST', '/api/tokens/delete', {'name': 'hwtest'}, auth=pw, user='hwtest')
        # on a gateway with accounts the new admin has to change its password first: the run's token deletes it
        who = {} if ARGS.token else {'auth': pw, 'user': 'hwtest'}
        check(http('POST', '/api/users/delete', {'name': 'hwtest'}, **who)[0] == 200, 'account deleted')


def tc31_sound_pressure():
    """Sound pressure in dB(A): plausible levels, calibration by a reference reading and by an offset, reset."""
    st = http('GET', '/api/sound/status')[1]
    check(st.get('running'), 'microphone running')
    p = st['spl']
    check(all(k in p for k in ('las', 'laeq', 'lamax', 'overload', 'window_s', 'offset', 'calibrated')), f'spl reported: {p}')
    check(20 <= p['laeq'] <= 110 and p['lamax'] >= p['laeq'] - 0.5, f'plausible levels: LAeq {p["laeq"]}, LAmax {p["lamax"]} dB(A)')
    original = p['offset'] if p['calibrated'] else None
    try:
        code, r = http('POST', '/api/sound/config', {'spl_reference': 5})
        check(code == 400, f'reference out of range rejected ({r})')
        ref = round(p['las'] + 10, 1)
        code, r = http('POST', '/api/sound/config', {'spl_reference': ref})
        check(code == 200 and r['spl']['calibrated'], 'calibrated by a reference reading')
        check(abs(r['spl']['las'] - ref) <= 1.5, f'level now reads the reference: {r["spl"]["las"]} vs {ref} dB(A)')
        check(abs(r['spl']['offset'] - (p['offset'] + 10)) <= 1.5, f'offset moved by the difference: {p["offset"]} -> {r["spl"]["offset"]}')
        code, r = http('POST', '/api/sound/config', {'spl_offset': 140.5})
        check(code == 200 and r['spl']['offset'] == 140.5, 'offset set directly')
        status()  # no restart needed, still answering
        check(http('GET', '/api/sound/status')[1]['spl']['offset'] == 140.5, 'calibration kept')
        code, r = http('POST', '/api/sound/config', {'spl_reset': True})
        check(code == 200 and not r['spl']['calibrated'] and r['spl']['offset'] == 135, 'reset to the default estimate')
    finally:
        if original is not None:
            http('POST', '/api/sound/config', {'spl_offset': original})
        else:
            http('POST', '/api/sound/config', {'spl_reset': True})


def tc32_lan_settings_traffic():
    """Header links, traffic counters, LAN settings: validation, revert when not confirmed, confirm, back to DHCP."""
    s = status()
    ln = s['links']
    check(ln['mqtt'] in ('off', 'connected', 'disconnected') and ln['internet'] in (True, False, None), f'links reported: {ln}')
    e = s['eth']
    check(e['mode'] in ('dhcp', 'static') and e['netmask'] and e['link_mode'], f'eth mode {e["mode"]}, mask {e["netmask"]}')
    t1 = http('GET', '/api/net/traffic')[1]
    for _ in range(5):
        status()
    t2 = http('GET', '/api/net/traffic')[1]
    check(t2['services']['web']['rx'] > t1['services']['web']['rx'] and t2['tx'] > t1['tx'],
          f'web traffic counted: {t1["services"]["web"]["rx"]} -> {t2["services"]["web"]["rx"]} B')
    check(t2['rx'] == sum(v['rx'] for v in t2['services'].values()), 'totals are the sum of the services')
    for body, why in (({'mode': 'static', 'ip': e['ipv4'], 'mask': '255.0.255.0'}, 'netmask'),
                      ({'mode': 'static', 'ip': e['ipv4'], 'mask': '255.255.255.0', 'gw': '10.255.255.1'}, 'gateway outside'),
                      ({'mode': 'static', 'mask': '255.255.255.0'}, 'no address'), ({'link': '1000full'}, 'link mode')):
        check(http('POST', '/api/net/lan', body)[0] == 400, f'rejected: {why}')
    check(http('POST', '/api/net/lan/confirm')[0] == 409, 'confirm without a pending change: 409')
    original = http('GET', '/api/net/lan')[1]
    fixed = {'mode': 'static', 'ip': e['ipv4'], 'mask': e['netmask'], 'gw': e['gateway'], 'dns1': e['dns'][0] if e['dns'] else ''}
    try:
        # the same address, so the test keeps reaching the gateway; not confirmed -> previous settings
        check(http('POST', '/api/net/lan', fixed)[0] == 200, 'static settings applied')
        s = wait_until('static mode', lambda: (lambda x: x if x['eth']['mode'] == 'static' else None)(status()), 15, 1)
        check(0 < s['eth']['confirm_s'] <= 120, f'waits for confirmation ({s["eth"]["confirm_s"]} s)')
        wait_until('revert', lambda: (lambda x: x if x['eth']['confirm_s'] == 0 else None)(status()), 140, 5)
        check(status()['eth']['mode'] == original['mode'], f'not confirmed: back to {original["mode"]}')
        check(http('POST', '/api/net/lan', fixed)[0] == 200, 'static settings applied again')
        time.sleep(2)
        code, r = http('POST', '/api/net/lan/confirm')
        check(code == 200 and r['mode'] == 'static' and r['confirm_s'] == 0, 'confirmed and stored')
        wait_until('internet', lambda: status()['links']['internet'] is True, 90, 5)
        check(True, 'internet reachable with the static address (DNS kept beside the Thread DNS64 server)')
    finally:
        restore = {k: v for k, v in original.items() if k != 'confirm_s'}
        http('POST', '/api/net/lan', restore)
        time.sleep(3)
        http('POST', '/api/net/lan/confirm')
    check(status()['eth']['mode'] == original['mode'], f'settings restored ({original["mode"]})')


def tc33_full_restart():
    """Restart from the menu: the C6 is reset right before the P4, the role comes back."""
    s = status()
    role = s['role']['active']
    boot = boot_id()
    check(http('POST', '/api/reboot', {'full': True})[0] == 200, 'full restart accepted')
    wait_until('restart', lambda: boot_id() not in (None, boot), 120, 3)
    s = wait_restart(role)
    tel = wait_until('C6 telemetry', lambda: (lambda x: x['c6'].get('telemetry') and x['c6']['telemetry']['age_s'] < 40 and x)(status()), 90, 5)
    p4, c6 = tel['uptime_s'], tel['c6']['telemetry']['uptime_s'] + tel['c6']['telemetry']['age_s']
    check(abs(p4 - c6) < 30, f'C6 restarted with the P4 (P4 {p4} s, C6 {c6} s)')


class Client:
    """HTTP client with a session cookie, a Bearer token or Basic auth."""
    def __init__(self, token=None, basic=None):
        self.cookie, self.token, self.basic = None, token, basic

    def req(self, method, path, body=None):
        url = f'http://{ARGS.host}{path}'
        data = json.dumps(body).encode() if body is not None else None
        r = urllib.request.Request(url, data=data, method=method)
        if body is not None:
            r.add_header('Content-Type', 'application/json')
        if self.cookie:
            r.add_header('Cookie', self.cookie)
        if self.token:
            r.add_header('Authorization', 'Bearer ' + self.token)
        if self.basic:
            import base64
            r.add_header('Authorization', 'Basic ' + base64.b64encode(self.basic.encode()).decode())
        try:
            with urllib.request.urlopen(r, timeout=30) as res:
                code, payload, headers = res.status, res.read(), res.headers
        except urllib.error.HTTPError as err:
            code, payload, headers = err.code, err.read(), err.headers
        c = headers.get('Set-Cookie')
        if c and c.startswith('p4s='):
            self.cookie = c.split(';')[0] if 'Max-Age=0' not in c else None
        try:
            return code, json.loads(payload)
        except ValueError:
            return code, payload


def totp_now(secret_b32, offset=0):
    import base64, hmac, hashlib, struct
    key = base64.b32decode(secret_b32 + '=' * (-len(secret_b32) % 8))
    counter = int(time.time()) // 30 + offset
    mac = hmac.new(key, struct.pack('>Q', counter), hashlib.sha1).digest()
    o = mac[19] & 15
    return '%06d' % ((struct.unpack('>I', mac[o:o + 4])[0] & 0x7fffffff) % 1000000)


def tc34_users():
    """Users: first admin, sign-in, lock, permissions per role and user, Basic and tokens, 2FA with recovery codes, reset request, back to open."""
    open_ = Client()
    code, st = open_.req('GET', '/api/auth/state')
    if st['enabled']:
        log('skipped: the gateway has user accounts (TC34 needs an open gateway)')
        return
    pw_a, pw_v = 'tc34-Admin-pass', 'tc34-Viewer-pass'
    created_token = None
    try:
        code, r = open_.req('POST', '/api/users', {'name': 'tc34admin', 'role': 'viewer', 'password': pw_a})
        check(code == 400, f'first account must be an admin ({r})')
        code, r = open_.req('POST', '/api/users', {'name': 'tc34admin', 'role': 'admin', 'password': pw_a})
        check(code == 200 and not r['must_change'], 'first admin created without a forced password change')
        check(open_.req('GET', '/api/status')[0] == 401, 'sign-in required from now on')
        check(status_open_flag() is True, '/api/status auth flag set')

        admin = Client()
        check(admin.req('POST', '/api/auth/login', {'user': 'tc34admin', 'password': 'wrong'})[0] == 401, 'wrong password: 401')
        code, r = admin.req('POST', '/api/auth/login', {'user': 'tc34admin', 'password': pw_a})
        check(code == 200 and admin.cookie and r['role'] == 'admin', 'admin signed in, session cookie set')
        check(admin.req('GET', '/api/status')[0] == 200, 'session works')

        code, r = admin.req('POST', '/api/users', {'name': 'tc34viewer', 'role': 'viewer', 'password': 'short'})
        check(code == 400, f'short password rejected ({r.get("error")})')
        code, r = admin.req('POST', '/api/users', {'name': 'tc34viewer', 'role': 'viewer', 'password': pw_v})
        check(code == 200 and r['must_change'], 'viewer created, must change the password')

        viewer = Client()
        code, r = viewer.req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v})
        check(code == 200 and r['must_change'], 'viewer signed in with a pending password change')
        code, r = viewer.req('GET', '/api/thread/status')
        check(code == 403 and r['error'] == 'password_change_required', 'API blocked until the password is changed')
        pw_v2 = pw_v + '2'
        check(viewer.req('POST', '/api/auth/password', {'old': pw_v, 'new': pw_v2})[0] == 200, 'viewer changed the password')
        check(viewer.req('GET', '/api/status')[0] == 200, 'viewer reads the status')
        check(viewer.req('GET', '/api/thread/dataset')[0] == 403, 'viewer: Thread network key hidden (403)')
        check(viewer.req('POST', '/api/identify', {'seconds': 1})[0] == 403, 'viewer: no edit on the overview (403)')
        check(viewer.req('GET', '/api/users')[0] == 403, 'viewer: no user management (403)')
        # own permission of one user beats the role
        check(admin.req('POST', '/api/users', {'name': 'tc34viewer', 'levels': {'overview': 2}})[0] == 200, 'own level: viewer may edit the overview')
        check(viewer.req('POST', '/api/identify', {'seconds': 1})[0] == 200, 'own level applies')
        admin.req('POST', '/api/users', {'name': 'tc34viewer', 'levels': {'overview': -1}})
        check(viewer.req('POST', '/api/identify', {'seconds': 1})[0] == 403, 'back to the role level')

        # Basic auth (clients of older versions) and tokens
        check(Client(basic=f'tc34viewer:{pw_v2}').req('GET', '/api/status')[0] == 200, 'Basic auth for a user without 2FA')
        code, r = admin.req('POST', '/api/tokens', {'name': 'tc34', 'role': 'viewer'})
        check(code == 200 and r['token'].startswith('p4gw_'), 'API token created')
        created_token = 'tc34'
        tok = Client(token=r['token'])
        check(tok.req('GET', '/api/thread/status')[0] == 200, 'viewer token reads the Thread status')
        check(tok.req('GET', '/api/status')[0] == 200, 'token: status')
        check(tok.req('POST', '/api/reboot', {})[0] == 403, 'viewer token cannot restart (403)')
        check(Client(token='p4gw_' + '0' * 40).req('GET', '/api/status')[0] == 401, 'unknown token: 401')

        # 2FA for the viewer
        code, setup = viewer.req('POST', '/api/auth/totp/setup', {})
        check(code == 200 and setup['secret'] and '<svg' in setup.get('qr_svg', ''), 'TOTP setup with a QR code')
        check(viewer.req('POST', '/api/auth/totp/enable', {'code': '000000'})[0] == 400, 'wrong code rejected')
        code, r = viewer.req('POST', '/api/auth/totp/enable', {'code': totp_now(setup['secret'])})
        check(code == 200 and len(r['recovery_codes']) == 10, '2FA on, 10 recovery codes')
        recovery = r['recovery_codes']
        check(Client(basic=f'tc34viewer:{pw_v2}').req('GET', '/api/status')[0] == 401, 'Basic refused with 2FA on')
        v2 = Client()
        code, r = v2.req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v2})
        check(code == 200 and r.get('code_required') and not v2.cookie, 'password alone asks for the code')
        time.sleep(31)  # the code used for enabling may not be replayed
        code, r = v2.req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v2, 'code': totp_now(setup['secret'])})
        check(code == 200 and v2.cookie, 'signed in with the authenticator code')
        v3 = Client()
        code, r = v3.req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v2, 'code': recovery[0]})
        check(code == 200, 'signed in with a recovery code')
        code, r = Client().req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v2, 'code': recovery[0]})
        check(code == 401, 'a recovery code works only once')
        code, me = v3.req('GET', '/api/auth/me')
        check(me['recovery_left'] == 9, '9 recovery codes left')

        # lock after failed sign-ins
        for _ in range(5):
            Client().req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': 'nope'})
        code, r = Client().req('POST', '/api/auth/login', {'user': 'tc34viewer', 'password': pw_v2})
        check(code == 403 and r['error'] == 'locked', f'locked after 5 failures (retry in {r.get("retry_s")} s)')
        code, users = admin.req('GET', '/api/users')
        check(any(u['name'] == 'tc34viewer' and u['locked'] for u in users['users']), 'admin sees the lock')

        # forgotten password
        check(Client().req('POST', '/api/auth/reset-request', {'user': 'tc34viewer'})[0] == 200, 'reset requested')
        code, me = admin.req('GET', '/api/auth/me')
        check(me['requests'] >= 1, f'admin sees {me["requests"]} request(s)')
        code, r = admin.req('POST', '/api/users', {'name': 'tc34viewer', 'password': 'tc34-Temp-pass', 'totp_reset': True, 'unlock': True})
        check(code == 200 and r['must_change'] and not r['totp'] and not r['locked'], 'admin set a temporary password, 2FA off, unlocked')
        check(admin.req('GET', '/api/auth/me')[1]['requests'] == me['requests'] - 1, 'request answered')
        check(v2.req('GET', '/api/status')[0] == 401, 'old sessions of the user ended')

        # the last admin stays
        check(admin.req('POST', '/api/users', {'name': 'tc34admin', 'role': 'user'})[0] == 400, 'last admin cannot be demoted')
        check(admin.req('POST', '/api/users/delete', {'name': 'tc34admin'})[0] == 400, 'last admin cannot be deleted while users exist')
    finally:
        admin = Client(basic=f'tc34admin:{pw_a}')
        if created_token:
            admin.req('POST', '/api/tokens/delete', {'name': created_token})
        admin.req('POST', '/api/users/delete', {'name': 'tc34viewer'})
        admin.req('POST', '/api/users/delete', {'name': 'tc34admin'})
        admin.req('POST', '/api/auth/requests/dismiss', {'name': 'tc34viewer'})
    check(Client().req('GET', '/api/auth/state')[1]['enabled'] is False, 'test users removed, the gateway is open again')


def status_open_flag():
    return Client(basic='tc34admin:tc34-Admin-pass').req('GET', '/api/status')[1]['auth']


def tc35_telegram():
    """Telegram bot settings: validation, an invalid token reaches Telegram and is refused (TLS works), own settings need a user."""
    check(status()['hw'].get('http_register_errors') == 0, 'every HTTP handler registered (table not full)')
    code, t = http('GET', '/api/telegram')
    check(code == 200 and 'state' in t, f'bot state {t.get("state")}')
    before = {'enabled': t['enabled'], 'tz': t['tz']}
    check(http('POST', '/api/telegram', {'token': 'no-colon'})[0] == 400, 'malformed token rejected')
    check(http('POST', '/api/telegram', {'tz': ''})[0] == 400, 'empty time zone rejected')
    if t['token_set']:
        log('a real token is set: the invalid-token check is skipped to keep it')
    else:
        try:
            check(http('POST', '/api/telegram', {'token': '123456:tc35-invalid', 'enabled': True})[0] == 200, 'invalid token stored, bot on')
            t = wait_until('answer from Telegram', lambda: (lambda x: x if x['state'] == 'error' else None)(http('GET', '/api/telegram')[1]), 40)
            check('401' in t['error'], f'Telegram refused the token over TLS: {t["error"]}')
        finally:
            http('POST', '/api/telegram', {'token': '', 'enabled': before['enabled']})
        check(http('GET', '/api/telegram')[1]['state'] == 'off', 'bot off again')
    code, r = http('GET', '/api/auth/telegram')
    check(code in (409, 200), f'own Telegram settings: {code} (409 for a token, open gateway)')
    check(http('POST', '/api/auth/telegram/pair')[0] == 409, 'pairing refused while the bot does not run or without a user')


def tc36_c6_online_update():
    """Online update of the C6 package: offered by a manifest, downloaded into PSRAM, checked (sha256), stored, the old one back."""
    import hashlib, shutil, socketserver, struct, tempfile, threading, zlib
    from http.server import SimpleHTTPRequestHandler
    src = os.path.join(ROOT, 'dist', 'c6fw.bin')
    original = open(src, 'rb').read()
    s0 = status()
    if not s0['update']['c6']['installed_id'] or hashlib.sha256(original).hexdigest()[:8] != s0['update']['c6']['installed_id']:
        log(f'skipped: the gateway does not hold dist/c6fw.bin ({s0["update"]["c6"]["installed_id"]})')
        return
    # the same firmwares with another label: the gateway stores it, the C6 is not rewritten (bundle hashes unchanged)
    test = bytearray(original)
    test[28:60] = b'tc36 label'.ljust(32, b'\0')
    crc_off = 12 + 8 * 88
    struct.pack_into('<I', test, crc_off, zlib.crc32(bytes(test[:crc_off])) & 0xffffffff)
    root = tempfile.mkdtemp()
    for name, data in (('test.bin', bytes(test)), ('orig.bin', original)):
        open(os.path.join(root, name), 'wb').write(data)

    def manifest(name, data, sha=None):
        m = {'version': s0['version'], 'p4gw': 'none.bin', 'notes': 'tc36',
             'c6fw': {'url': name, 'sha256': sha or hashlib.sha256(data).hexdigest(), 'size': len(data), 'format': 3, 'bundles': {}}}
        open(os.path.join(root, 'manifest.json'), 'w').write(json.dumps(m))

    handler = lambda *a, **k: SimpleHTTPRequestHandler(*a, directory=root, **k)
    server = socketserver.TCPServer(('', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    me = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    me.connect((ARGS.host if ARGS.host[0].isdigit() else socket.gethostbyname(ARGS.host), 80))
    url = f'http://{me.getsockname()[0]}:{server.server_address[1]}/manifest.json'
    before_url = s0['update']['url']

    def offered():
        def checked():
            u = status()['update']
            if u['checking'] or not u['checked']:
                http('POST', '/api/update/check')  # again: a request sent while the gateway restarted is lost
                return None
            return u
        http('POST', '/api/update/check')
        return wait_until('manifest checked', checked, 60, 2)

    def install_and_wait():
        up = status()['uptime_s']
        check(http('POST', '/api/update/install', {'target': 'c6'})[0] == 200, 'C6 install started')
        # restarted: the uptime starts again
        return wait_until('gateway restarted', lambda: (lambda x: x if x['uptime_s'] < up and x['update']['c6']['installed_id'] else None)(status()), 180, 3)

    try:
        manifest('test.bin', test)
        check(http('POST', '/api/settings', {'update_url': url})[0] == 200, f'manifest URL {url}')
        u = offered()
        check(u['c6']['offered'] and u['c6']['available'], f'C6 package offered: {u["c6"]["installed_id"]} -> {u["c6"]["offered_id"]}')
        manifest('test.bin', test, sha='00' * 32)
        offered()
        up = status()['uptime_s']
        check(http('POST', '/api/update/install', {'target': 'c6'})[0] == 200, 'install with a wrong sha256 started')
        u = wait_until('refused', lambda: (lambda u: u if not u['c6']['in_progress'] and u['error'] else None)(status()['update']), 30, 1)
        check('sha256' in u['error'] and status()['uptime_s'] >= up, f'refused, no restart: {u["error"]}')
        manifest('test.bin', test)
        offered()
        s = install_and_wait()
        check(s['update']['c6']['installed_versions'].get('slots') == 'tc36 label', 'new package stored after the restart')
        c6 = s['update']['c6']
        check(c6['installed_id'] == c6['offered_id'] and not c6['available'], f'installed {c6["installed_id"]} = offered {c6["offered_id"]}, nothing more to install')
        check(not s['c6']['flash']['busy'] and s['role']['active'] == s0['role']['active'], 'role back, the C6 was not rewritten')
        manifest('orig.bin', original)
        offered()
        s = install_and_wait()
        check(s['update']['c6']['installed_id'] == s0['update']['c6']['installed_id'], 'original package back')
    finally:
        http('POST', '/api/settings', {'update_url': before_url})
        server.shutdown()
        shutil.rmtree(root, ignore_errors=True)


def tc37_release_list():
    """releases.json: only releases that name the board's flash layout are listed (on layout 4 also those
    of layout 3: v4 only grew the gateway slots); a picked release whose manifest does not run on the
    layout, or an unlisted one, is refused without a restart."""
    import shutil, socketserver, tempfile, threading
    from http.server import SimpleHTTPRequestHandler
    s0 = status()
    layout, current = s0['update']['layout'], s0['version']
    root = tempfile.mkdtemp()
    handler = lambda *a, **k: SimpleHTTPRequestHandler(*a, directory=root, **k)
    server = socketserver.TCPServer(('', 0), handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    me = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    me.connect((ARGS.host if ARGS.host[0].isdigit() else socket.gethostbyname(ARGS.host), 80))
    base = f'http://{me.getsockname()[0]}:{server.server_address[1]}'

    def write(name, obj):
        open(os.path.join(root, name), 'w').write(json.dumps(obj))

    def checked():
        http('POST', '/api/update/check')
        def done():
            u = status()['update']
            return u if not u['checking'] and u['checked'] else None
        time.sleep(2)
        return wait_until('manifest checked', done, 60, 2)

    write('manifest.json', {'version': current, 'p4gw': 'none.bin', 'layouts': [layout]})
    write('nolayout.json', {'version': 'v0.0-nolayout', 'p4gw': 'none.bin'})
    write('releases.json', {'releases': [
        {'version': current, 'manifest': f'{base}/manifest.json', 'layouts': [layout]},
        {'version': 'v0.0-other', 'manifest': f'{base}/manifest.json', 'layouts': [99]},
        {'version': 'v0.0-v3', 'manifest': f'{base}/manifest.json', 'layouts': [3]},
        {'version': 'v0.0-nolayout', 'manifest': f'{base}/nolayout.json', 'layouts': [layout]}]})
    before_url = s0['update']['url']
    try:
        check(http('POST', '/api/settings', {'update_url': f'{base}/manifest.json'})[0] == 200, 'manifest URL set')
        u = checked()
        versions = [r['version'] for r in u['releases']]
        expected = [current] + (['v0.0-v3'] if layout in (3, 4) else []) + ['v0.0-nolayout']
        check(versions == expected, f'listed for layout {layout}: {versions}')
        check(http('POST', '/api/update/install', {'version': 'v0.0-other'})[0] == 409, 'release of another layout refused (409)')
        check(http('POST', '/api/update/install', {'version': 'v9.9'})[0] == 409, 'unlisted release refused (409)')
        up = status()['uptime_s']
        check(http('POST', '/api/update/install', {'version': 'v0.0-nolayout'})[0] == 200, 'listed release accepted')
        u = wait_until('refused', lambda: (lambda u: u if not u['in_progress'] and u['error'] else None)(status()['update']), 60, 1)
        check('flash layout' in u['error'], f'manifest without the layout refused: {u["error"]}')
        check(u['latest_version'] == current and status()['uptime_s'] >= up, 'offer restored, no restart')
        write('manifest.json', {'version': 'v0.0-new', 'p4gw': 'none.bin', 'layouts': [99]})
        u = checked()
        check('flash layout' in u['error'] and u['latest_version'] == current, f'latest of another layout not offered: {u["error"]}')
    finally:
        http('POST', '/api/settings', {'update_url': before_url})
        server.shutdown()
        shutil.rmtree(root, ignore_errors=True)


def boot_id():
    """Changes with every start of the gateway (notification ring)."""
    code, r = http('GET', '/api/notify?since=0', timeout=5)
    return r.get('boot') if code == 200 and isinstance(r, dict) else None


def switch_b(role, timeout=300):
    """Role of radio B (docs/dual-radio.md); waits for the restart and radio B's start."""
    boot = boot_id()
    code, r = http('POST', '/api/role', {'role': role, 'radio': 'b'})
    if code != 200:
        raise Fail(f'radio B role {role}: {code} {r}')

    def ready():
        try:
            if boot_id() == boot:
                return None
            s = status()
        except (OSError, Fail):
            return None  # restarting
        rb = s['role'].get('b', {})
        if role == 'none':
            return s if rb.get('desired') == 'none' and s['role']['active'] != 'none' or s['role']['maintenance'] else None
        return s if rb.get('active') == role or rb.get('maintenance') else None
    s = wait_until(f'radio B with role {role}', ready, timeout, 5)
    check(not s['role']['b']['maintenance'], f'radio B {role} started ({s["role"]["b"]["maintenance"]!r})')
    return s


def tc39_two_radios():
    """Radio A Thread border router and radio B (external, JP1) Zigbee coordinator at the same time: both
    run, radio B's NCP sends telemetry and frames, its TCP bridge takes a client, role conflicts are refused."""
    radios = http('GET', '/api/radios')[1]
    b = radios[1] if isinstance(radios, list) and len(radios) > 1 else {}
    if not b.get('wired'):
        log(f'skipped: no radio B on JP1 ({b.get("wiring")})')
        return
    s0 = status()
    before_b = s0['role'].get('b', {}).get('desired', 'none')
    if s0['role']['active'] != 'thread_br':
        switch('thread_br')
    try:
        s = switch_b('zigbee_coordinator') if status()['role']['b']['active'] != 'zigbee_coordinator' else status()
        check(s['role']['active'] == 'thread_br', 'radio A still Thread border router')
        wait_thread_running()
        # after the restart the border router attaches again within a few seconds (detached meanwhile)
        t = wait_until('Thread attached', lambda: (lambda t: t if t.get('role') in ('leader', 'router', 'child') else None)(http('GET', '/api/thread/status')[1]), 90, 3)
        check(t.get('role') in ('leader', 'router', 'child'), f'Thread network up on radio A ({t.get("role")})')

        def telemetry():
            r = http('GET', '/api/radios')[1][1]
            return r if (r.get('telemetry') or {}).get('fw') == 'zigbee_ncp' else None
        r = wait_until('telemetry of radio B', telemetry, 90, 5)
        log(f'radio B: {r["chip"]}, {r["telemetry"].get("temp_c")} °C, up {r["telemetry"].get("uptime_s")} s')
        rs = status()['role_status_b']
        check(rs['uart_errors'] == 0 and rs['telemetry_frames'] > 0, f'radio B link clean ({rs["telemetry_frames"]} telemetry frames)')
        with socket.create_connection((ARGS.host, 6638), timeout=5):
            time.sleep(1)
            check(status()['role_status_b']['client_connected'], 'TCP client on radio B registered')
        time.sleep(1)
        check(not status()['role_status_b']['client_connected'], 'client disconnect detected')
        check(http('POST', '/api/role', {'role': 'thread_router', 'radio': 'b'})[0] == 409, 'second Thread network refused (409)')
        check(http('POST', '/api/role', {'role': 'zigbee_router'})[0] == 409, 'second Zigbee network on radio A refused (409)')
        t0 = r['telemetry'].get('uptime_s', 0)
        time.sleep(35)
        r2 = http('GET', '/api/radios')[1][1]
        check(r2['telemetry'].get('uptime_s', 0) > t0, f'radio B did not restart ({t0} -> {r2["telemetry"].get("uptime_s")} s)')
    finally:
        if before_b != status()['role']['b']['desired']:
            switch_b(before_b)


def tc40_thread_per_radio():
    """Each radio keeps its own Thread network: Thread on radio B starts with B's settings (not radio A's
    network), and back on radio A the first network returns unchanged."""
    radios = http('GET', '/api/radios')[1]
    if not (isinstance(radios, list) and len(radios) > 1 and radios[1].get('wired')):
        log('skipped: no radio B on JP1')
        return
    s0 = status()
    a0, b0 = s0['role']['active'], s0['role']['b']['desired']
    if a0 not in ('thread_br', 'thread_router'):
        switch('thread_br')
    wait_thread_running()
    net_a = http('GET', '/api/thread/status')[1]
    log(f'radio A network {net_a.get("network_name")} {net_a.get("ext_pan_id")}')
    try:
        if status()['role']['b']['desired'] != 'none':
            switch_b('none')
        switch('zigbee_router')
        switch_b('thread_br')

        def running():
            t = http('GET', '/api/thread/status')[1]
            return t if t.get('ext_pan_id') and t.get('role') in ('leader', 'router') else None
        net_b = wait_until('Thread network on radio B', running, 120, 3)
        log(f'radio B network {net_b.get("network_name")} {net_b.get("ext_pan_id")}, RCP {net_b.get("rcp_version")}')
        check('esp32c5' in net_b.get('rcp_version', '') or 'esp32c6' in net_b.get('rcp_version', ''), 'RCP of radio B answers')
        check(net_b['ext_pan_id'] != net_a.get('ext_pan_id'), 'radio B has its own Thread network')
        switch_b('none')
        switch('thread_br')
        net = wait_until('Thread network on radio A', running, 120, 3)
        check(net['ext_pan_id'] == net_a.get('ext_pan_id') and net['network_name'] == net_a.get('network_name'),
              f'radio A back on its network ({net["network_name"]})')
    finally:
        s = status()
        if s['role']['b']['desired'] != 'none' and b0 != s['role']['b']['desired']:
            switch_b('none')
        if s['role']['active'] != a0:
            switch(a0)
        if b0 != status()['role']['b']['desired']:
            switch_b(b0)


# ---------------------------------------------------------------- Wi-Fi uplink (docs/wifi-uplink.md)

def http_at(ip, method, path, body=None, timeout=10):
    """Like http(), to another address of the gateway (its Wi-Fi uplink or setup access point)."""
    host = ARGS.host
    try:
        ARGS.host = ip
        return http(method, path, body, timeout=timeout)
    finally:
        ARGS.host = host


def uplink(s=None):
    """Status of the Wi-Fi uplink role (radio A or B), {} without it."""
    s = s or status()
    for key in ('role_status_b', 'role_status'):
        if (s.get(key) or {}).get('mode') == 'wifi_uplink':
            return s[key]
    return {}


def thread_status(s):
    for key in ('role_status', 'role_status_b'):
        if (s.get(key) or {}).get('mode') == 'border_router':
            return s[key]
    return {}


def ensure_uplink_b():
    s = status()
    if not s['role'].get('b', {}).get('wired'):
        raise Skip('no radio B on JP1')
    if s['role']['b'].get('active') != 'wifi_uplink':
        stage_package('c5')
        switch_b('wifi_uplink')
    w = wait_until('Wi-Fi uplink connected', lambda: (lambda w: w if w.get('ipv4') else None)(uplink()), 120, 3)
    log(f'uplink on radio {w["radio"].upper()}: {w["ipv4"]}, {w.get("band")} channel {w.get("channel")}, {w.get("rssi")} dBm')
    return w


def outage(seconds):
    code, r = http('POST', '/api/net/lan/outage', {'seconds': seconds})
    if code != 200:
        raise Fail(f'LAN outage test: {code} {r}')
    return time.time() + seconds


def wait_backbone(which, timeout):
    def ready():
        s = status()
        t = thread_status(s)
        return s if t.get('backbone') == which and t.get('phase') == 'running' else None
    return wait_until(f'Thread backbone {which}', ready, timeout, 5)


def tc41_wifi_uplink():
    """Radio B as the Wi-Fi uplink next to Thread on radio A, LAN up: Wi-Fi joins with its own address,
    Ethernet keeps the default route, the web UI answers over Wi-Fi, its traffic is counted, no password
    in the API, a second uplink is refused, throughput over Wi-Fi recorded."""
    w = ensure_uplink_b()
    s = status()
    check(s['eth']['link'] and not w['default_route'], 'Ethernet keeps the default route, Wi-Fi is the standby')
    check(thread_status(s).get('backbone') == 'ethernet', 'Thread backbone on Ethernet')
    code, ws = http_at(w['ipv4'], 'GET', '/api/status')
    check(code == 200 and ws.get('hostname') == s['hostname'], f'web UI over Wi-Fi ({w["ipv4"]})')
    t0 = http('GET', '/api/net/wifi/traffic')[1]
    t1 = time.time()
    req = urllib.request.Request(f'http://{w["ipv4"]}/')
    with urllib.request.urlopen(req, timeout=30) as res:
        size = len(res.read())
    rate = size / (time.time() - t1)
    log(f'web UI over Wi-Fi: {size} B in {time.time() - t1:.2f} s ({rate / 1000:.0f} kB/s)')
    check(rate > 30000, 'throughput over Wi-Fi above 30 kB/s')
    t2 = http('GET', '/api/net/wifi/traffic')[1]
    check(t2['services']['web']['tx'] - t0['services']['web']['tx'] >= size, 'the page counted under web on Wi-Fi')
    cfg = http('GET', '/api/net/wifi')[1]
    check('password' not in json.dumps(cfg['settings']).replace('password_set', ''), 'no password in the API')
    code, nets = http('GET', '/api/net/wifi/scan', timeout=30)
    check(code == 200 and isinstance(nets, list) and nets, f'scan: {len(nets) if isinstance(nets, list) else nets} networks')
    code, r = http('POST', '/api/role', {'role': 'wifi_uplink', 'radio': 'a'})
    check(code == 409 and 'Wi-Fi uplink' in str(r), f'second uplink refused ({code})')


def tc42_wifi_failover():
    """LAN pulled while running (outage test): the gateway stays reachable over Wi-Fi and reaches the
    internet, the Thread border router moves to the Wi-Fi backbone (restart); LAN back: traffic on
    Ethernet at once, the backbone back on Ethernet after the 5 min hysteresis."""
    w = ensure_uplink_b()
    ip = w['ipv4']
    end = outage(150)

    def over_wifi():
        code, ws = http_at(ip, 'GET', '/api/status', timeout=4)
        return ws if code == 200 and not ws['eth']['link'] and uplink(ws).get('default_route') else None
    ws = wait_until('the gateway over Wi-Fi with the default route', over_wifi, 30, 2)
    check(True, f'reachable over Wi-Fi {ip} {int(150 - (end - time.time()))} s into the outage')
    ws = wait_until('internet over Wi-Fi', lambda: (lambda c, ws: ws if c == 200 and ws.get('links', {}).get('internet') else None)(
        *http_at(ip, 'GET', '/api/status', timeout=4)), 120, 5)
    check(True, 'internet reachable over Wi-Fi (TCP to github.com:443)')

    def moved():
        code, ws = http_at(ip, 'GET', '/api/status', timeout=4)
        t = thread_status(ws) if code == 200 else {}
        return ws if t.get('backbone') == 'wifi' and t.get('phase') == 'running' else None
    ws = wait_until('Thread backbone on Wi-Fi', moved, 150, 5)
    check(ws['uptime_s'] < 150, f'border router restarted onto the Wi-Fi backbone (up {ws["uptime_s"]} s)')
    code, t = http_at(ip, 'GET', '/api/thread/status')
    check(code == 200 and t.get('role') in ('leader', 'router'), f'Thread network {t.get("network_name")} {t.get("role")} over Wi-Fi')
    time.sleep(max(0, end - time.time()))
    s = wait_until('LAN back', lambda: (lambda s: s if s['eth']['link'] else None)(status()), 60, 3)
    check(not uplink(s).get('default_route'), 'Ethernet has the default route again')
    s = wait_backbone('ethernet', 480)
    check(True, f'backbone back on Ethernet (up {s["uptime_s"]} s)')


def tc43_wifi_wrong_network():
    """A network that is not there: clear error in the status, retries, LAN untouched; dropping the own
    setting returns to the ESPHome Wi-Fi."""
    ensure_uplink_b()
    code, r = http('POST', '/api/net/wifi', {'ssid': 'p4gw-test-absent'})
    check(code == 200 and r['settings']['ssid'] == 'p4gw-test-absent', 'network set')
    w = wait_until('error shown', lambda: (lambda w: w if w.get('state') in ('disconnected', 'connecting') and
                   'not found' in w.get('error', '') else None)(uplink()), 90, 3)
    check(True, f'state {w["state"]}: {w["error"]}')
    check(status()['eth']['link'], 'LAN untouched')
    code, r = http('POST', '/api/net/wifi', {'ssid': '', })
    check(code == 200 and r['settings']['from_esphome'], 'own setting dropped')
    w = wait_until('Wi-Fi connected again', lambda: (lambda w: w if w.get('ipv4') else None)(uplink()), 150, 3)
    check(w['connects'] >= 2, f'connected again ({w["connects"]} connections)')
    code, r = http('POST', '/api/net/wifi', {'ssid': 'x', 'password': 'short'})
    check(code == 400, 'password shorter than 8 refused')
    code, r = http('POST', '/api/net/wifi', {'ssid': 'x', 'mode': 'static', 'ip': '10.0.0.1'})
    check(code == 400, 'static without netmask refused')


def _dns_a(server, name, src_ip):
    q = os.urandom(2) + b'\x01\x00\x00\x01\x00\x00\x00\x00\x00\x00'
    q += b''.join(bytes([len(p)]) + p.encode() for p in name.split('.')) + b'\x00\x00\x01\x00\x01'
    # retried like a phone's resolver: a scan of the station takes the radio off the access point's
    # channel for a few seconds
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind((src_ip, 0))
        sock.settimeout(3)
        for attempt in range(5):
            try:
                sock.sendto(q, (server, 53))
            except OSError:
                if attempt == 4:
                    raise
                time.sleep(2)
                continue
            try:
                r = sock.recv(512)
                break
            except OSError:  # timeout, or no route while the station rejoins
                if attempt == 4:
                    raise
                time.sleep(2)
    return socket.inet_ntoa(r[-4:]) if r[7] else None


def _iface_ip(iface):
    import subprocess
    out = subprocess.run(['ipconfig', 'getifaddr', iface], capture_output=True, text=True).stdout.strip()
    return out or None


def tc44_wifi_setup_portal():
    """No LAN and no Wi-Fi: the open setup access point appears, gives an address and the gateway as DNS,
    every name resolves to it, phone connectivity checks are sent to the web UI, the API works over it
    and a network set there is joined (macOS test host with --wifi-iface)."""
    import subprocess
    if not ARGS.wifi_iface or sys.platform != 'darwin':
        raise Skip('needs --wifi-iface on a macOS test host')
    w = ensure_uplink_b()
    hostname = status()['hostname']
    if status()['uptime_s'] > 300:  # the access point opens only in the first minutes after a start
        http('POST', '/api/reboot', {})
        time.sleep(10)
        wait_until('restart', lambda: (lambda w: w if w.get('ipv4') else None)(uplink()), 180, 3)
    code, r = http('POST', '/api/net/wifi', {'ssid': 'p4gw-test-absent'})
    check(code == 200, 'Wi-Fi set to an absent network')
    end = outage(300)
    iface = ARGS.wifi_iface
    try:
        def joined():
            out = subprocess.run(['networksetup', '-setairportnetwork', iface, hostname], capture_output=True, text=True)
            return (out.stdout + out.stderr).strip() == '' and _iface_ip(iface)
        src = wait_until(f'joined the setup access point {hostname}', joined, 150, 8)
        time.sleep(3)
        src = _iface_ip(iface) or src
        check(src.startswith('192.168.4.'), f'address from the access point ({src})')
        ap = '192.168.4.1'
        check(_dns_a(ap, 'connectivitycheck.gstatic.com', src) == ap, 'every name resolves to the gateway')
        req = urllib.request.Request(f'http://{ap}/generate_204')

        class NoRedirect(urllib.request.HTTPRedirectHandler):
            def redirect_request(self, *a, **k):
                return None
        try:
            urllib.request.build_opener(NoRedirect).open(req, timeout=5)
            code, loc = 200, ''
        except urllib.error.HTTPError as err:
            code, loc = err.code, err.headers.get('Location', '')
        check(code == 302 and loc.startswith(f'http://{ap}/'), f'connectivity check sent to the web UI ({code} {loc})')
        code, ps = http_at(ap, 'GET', '/api/status')
        p = uplink(ps).get('portal', {})
        check(code == 200 and p.get('on') and p.get('clients', 0) >= 1, f'API over the access point, portal {p}')
        code, r = http_at(ap, 'POST', '/api/net/wifi', {'ssid': ''}, timeout=30)
        check(code == 200, 'network set over the access point')
    finally:
        subprocess.run(['networksetup', '-removepreferredwirelessnetwork', iface, hostname], capture_output=True)
        subprocess.run(['networksetup', '-setairportpower', iface, 'off'], capture_output=True)
        subprocess.run(['networksetup', '-setairportpower', iface, 'on'], capture_output=True)
        time.sleep(max(0, end - time.time()))
        wait_until('LAN back', lambda: (lambda s: s if s['eth']['link'] else None)(status()), 240, 5)
        if not http('GET', '/api/net/wifi')[1]['settings']['from_esphome']:
            http('POST', '/api/net/wifi', {'ssid': ''})  # the case failed before the portal set it
    w = wait_until('uplink joined the network set over the portal', lambda: (lambda w: w if w.get('ipv4') else None)(uplink()), 120, 3)
    check(not w['portal']['on'], 'access point closed once online')
    wait_backbone('ethernet', 480)


def stage_package(chip):
    """--packages DIR: the radio package of the chip ('c6'/'c5') staged on the gateway, for bundles a
    release does not have yet; the radio is flashed from it at the next start."""
    if not ARGS.packages:
        return
    with open(os.path.join(ARGS.packages, f'{chip}fw.bin'), 'rb') as f:
        code, r = http('POST', '/api/ota/c6fw', raw=f.read(), timeout=120)
    if code != 200:
        raise Fail(f'{chip} package upload: {code} {r}')
    log(f'{chip} package from {ARGS.packages} staged')


def tc45_wifi_swap():
    """The Wi-Fi uplink moves to radio A (C6) by swapping the roles: radio A joins the Wi-Fi, radio B runs
    radio A's former role; swapped back, radio A returns to its Thread network unchanged."""
    w = ensure_uplink_b()
    s0 = status()
    a0 = s0['role']['active']
    net0 = http('GET', '/api/thread/status')[1].get('network_name') if a0.startswith('thread') else None
    stage_package('c6')
    boot = boot_id()
    code, r = http('POST', '/api/role', {'swap': True})
    check(code == 200, 'swap accepted')

    def swapped():
        if boot_id() == boot:
            return None
        s = status()
        u = uplink(s)
        return s if s['role']['active'] == 'wifi_uplink' and s['role']['b']['active'] == a0 and u.get('ipv4') else None
    s = wait_until('roles swapped', swapped, 420, 5)
    u = uplink(s)
    check(u['radio'] == 'a', f'radio A (C6) is the Wi-Fi uplink: {u["ipv4"]}, {u.get("band")} channel {u.get("channel")}')
    code, ws = http_at(u['ipv4'], 'GET', '/api/status')
    check(code == 200, 'web UI over the Wi-Fi of radio A')
    if a0 == 'thread_br':
        wait_until('Thread on radio B', lambda: (lambda t: t if t.get('phase') == 'running' else None)(thread_status(status())), 120, 5)
        check(True, 'Thread border router runs on radio B')
    stage_package('c5')
    boot = boot_id()
    code, r = http('POST', '/api/role', {'swap': True})
    check(code == 200, 'swap back accepted')

    def back():
        if boot_id() == boot:
            return None
        s = status()
        return s if s['role']['active'] == a0 and s['role']['b']['active'] == 'wifi_uplink' and uplink(s).get('ipv4') else None
    s = wait_until('roles swapped back', back, 420, 5)
    if net0:
        t = wait_thread_running(120)
        check(t.get('network_name') == net0, f'radio A back on its Thread network ({net0})')


def tc51_time_settings():
    """Clock of the gateway (System › LAN › Time): SNTP synced, invalid settings refused, new servers sync
    again without a restart, the zone reaches /api/status and the Telegram settings, the old Telegram field
    still sets it, an empty list stops the sync, the settings survive a restart. The settings of before come back."""
    code, before = http('GET', '/api/time')
    check(code == 200 and before['synced'] > 1700000000 and before['time'] >= before['synced'],
          f'synced at {before["synced"]} from {[s["name"] for s in before["servers"]]}, zone {before["tz_name"] or before["tz"]}')
    check(status()['tz_name'] == before['tz_name'], 'zone name in /api/status')
    keep = {'servers': [s['name'] for s in before['servers']], 'tz': before['tz'], 'tz_name': before['tz_name']}
    try:
        for body, why in (({'servers': ['a', 'b', 'c', 'd']}, 'four servers'), ({'servers': ['bad host']}, 'space in a name'),
                          ({'servers': 'pool.ntp.org'}, 'servers not a list'), ({'tz_name': 'UTC'}, 'name without a rule'),
                          ({'tz': ''}, 'empty rule'), ({'tz': 'X' * 64}, 'rule of 64 characters')):
            check(http('POST', '/api/time', body)[0] == 400, f'refused: {why}')
        code, r = http('POST', '/api/time', {'servers': ['tik.cesnet.cz', '192.0.2.1'], 'tz': 'UTC0', 'tz_name': 'UTC'})
        check(code == 200 and [s['name'] for s in r['servers']] == ['tik.cesnet.cz', '192.0.2.1'] and r['tz'] == 'UTC0', 'stored: two servers, UTC')
        s = wait_until('sync from the new server', lambda: (lambda x: x if x['synced'] > r['synced'] and x['servers'][0]['reach'] & 1
                                                                 and x['servers'][0]['address'] else None)(http('GET', '/api/time')[1]), 60, 3)
        check(abs(s['time'] - time.time()) < 5, f'synced again from {s["servers"][0]["address"]} without a restart, {s["time"] - time.time():+.1f} s from this computer')
        check(status()['tz_name'] == 'UTC' and http('GET', '/api/telegram')[1]['tz'] == 'UTC0', 'zone in /api/status and the Telegram settings')
        code, _ = http('POST', '/api/telegram', {'tz': 'EET-2EEST,M3.5.0/3,M10.5.0/4'})
        r = http('GET', '/api/time')[1]
        check(code == 200 and r['tz'] == 'EET-2EEST,M3.5.0/3,M10.5.0/4' and r['tz_name'] == '', 'the Telegram field sets the zone, without a name')
        code, r = http('POST', '/api/time', {'servers': []})
        check(code == 200 and r['servers'] == [], 'empty list: no time sync')
        http('POST', '/api/time', {'servers': ['tik.cesnet.cz'], 'tz': 'UTC0', 'tz_name': 'UTC'})
        http('POST', '/api/reboot', {})
        wait_restart(status()['role']['desired'])
        r = wait_until('sync after the restart', lambda: (lambda x: x if x['synced'] else None)(http('GET', '/api/time')[1]), 60, 3)
        check([s['name'] for s in r['servers']] == ['tik.cesnet.cz'] and r['tz'] == 'UTC0' and r['tz_name'] == 'UTC',
              'server and zone kept over a restart, synced again')
    finally:
        http('POST', '/api/time', keep)
    r = http('GET', '/api/time')[1]
    check([s['name'] for s in r['servers']] == keep['servers'] and r['tz'] == keep['tz'] and r['tz_name'] == keep['tz_name'], 'settings of before restored')


def tc52_gpio():
    """GPIO of JP1 without a jumper: header with radio A locked (radio B while it has a role), invalid settings
    refused, inputs follow their pull, outputs read back on the pad (inverted, open drain against push-pull), a pulse,
    PWM duty and resolution, counter, values restored after a restart per pin. The configuration of before comes back."""
    code, before = http('GET', '/api/gpio')
    check(code == 200 and len(before['header']) == 26, '26 pins of JP1')
    hdr = {h['gpio']: h for h in before['header'] if h['kind'] == 'gpio'}
    check(all(not hdr[g]['free'] and 'radio A' in hdr[g]['reserved'] for g in (4, 5, 20)), 'radio A pins GPIO4, 5, 20 locked')
    free = [g for g in (1, 2, 3, 45) if hdr[g]['free']]
    check(free == [1, 2, 3, 45], f'GPIO1, 2, 3, 45 free')
    b_role = status()['role'].get('b', {}).get('desired', 'none')
    if b_role != 'none':
        check(all(not hdr[g]['free'] for g in (32, 33, 46, 47)), f'radio B pins locked while radio B runs {b_role}')
        check(http('PUT', '/api/gpio', {'pins': [{'gpio': 32, 'mode': 'output'}]})[0] == 400, 'radio B pin refused')
    keep = {'pins': [{k: p[k] for k in ('gpio', 'mode', 'name', 'expose', 'invert', 'pull', 'debounce_ms', 'open_drain', 'restore', 'freq', 'edge', 'filter_ns') if k in p}
                     for p in before['pins']]}
    pin = lambda g: next((p for p in http('GET', '/api/gpio')[1]['pins'] if p['gpio'] == g), None)
    try:
        for body, why in (({'pins': [{'gpio': 4, 'mode': 'output'}]}, 'radio A pin'), ({'pins': [{'gpio': 9, 'mode': 'output'}]}, 'GPIO not on JP1'),
                          ({'pins': [{'gpio': 2, 'mode': 'output'}, {'gpio': 2, 'mode': 'input'}]}, 'GPIO twice'),
                          ({'pins': [{'gpio': 2, 'mode': 'blink'}]}, 'unknown mode'), ({'pins': [{'gpio': 1, 'mode': 'pwm', 'freq': 2000000}]}, 'PWM 2 MHz'),
                          ({'pins': [{'gpio': 2, 'mode': 'input', 'debounce_ms': 2000}]}, 'debounce 2 s'),
                          ({'pins': [{'gpio': 2, 'mode': 'output', 'name': 'x' * 33}]}, 'name of 33 characters'),
                          ({'pins': [{'gpio': 2, 'mode': 'output', 'expose': 'all'}]}, 'unknown expose'),
                          ({'pins': [{'gpio': 45, 'mode': 'counter', 'filter_ns': 20000}]}, 'filter 20 us')):
            check(http('PUT', '/api/gpio', body)[0] == 400, f'refused: {why}')
        for pull, want in (('up', 1), ('down', 0)):
            r = http('PUT', '/api/gpio', {'pins': [{'gpio': g, 'mode': 'input', 'pull': pull, 'debounce_ms': 0} for g in free]})[1]
            time.sleep(0.3)
            levels = {p['gpio']: p['level'] for p in http('GET', '/api/gpio')[1]['pins']}
            check(all(levels[g] == want for g in free), f'inputs with pull-{pull} read {want}: {levels}')
        code, r = http('PUT', '/api/gpio', {'pins': [
            {'gpio': 2, 'mode': 'output', 'name': 'Relé', 'restore': 'last'},
            {'gpio': 3, 'mode': 'output', 'invert': True, 'restore': 'on'},
            {'gpio': 1, 'mode': 'pwm', 'freq': 1000, 'restore': 'last'},
            {'gpio': 45, 'mode': 'counter', 'pull': 'down', 'edge': 'both', 'filter_ns': 1000}]})
        check(code == 200 and all(p['active'] for p in r['pins']), 'output, inverted output, PWM and counter active')
        p3 = pin(3)
        check(p3['level'] == 1 and p3['pin_level'] == 0, 'inverted output restored on: level 1, pin 0')
        check(http('POST', '/api/gpio/set', {'gpio': 2, 'level': 1})[0] == 200 and pin(2)['pin_level'] == 1, 'output on: pin reads 1')
        http('POST', '/api/gpio/set', {'gpio': 2, 'level': 0})
        http('POST', '/api/gpio/set', {'gpio': 2, 'pulse_ms': 800})
        during = pin(2)['pin_level']
        time.sleep(1.2)
        check(during == 1 and pin(2)['pin_level'] == 0, 'pulse of 0.8 s: on, then off')
        code, r = http('POST', '/api/gpio/set', {'gpio': 1, 'duty': 33})
        check(code == 200 and r['duty'] == 33 and r['resolution_bits'] >= 10, f'PWM 1 kHz at 33 % with {r.get("resolution_bits")} bits')
        for body, want, why in (({'gpio': 1, 'duty': 101}, 400, 'duty 101'), ({'gpio': 2, 'duty': 50}, 400, 'duty on an output'),
                                ({'gpio': 9, 'level': 1}, 404, 'pin not configured'), ({'gpio': 2, 'pulse_ms': 0}, 400, 'pulse 0 ms')):
            check(http('POST', '/api/gpio/set', body)[0] == want, f'set refused ({want}): {why}')
        c = pin(45)
        check(c['count'] == 0 and c['frequency'] == 0 and http('POST', '/api/gpio/set', {'gpio': 45, 'reset': True})[0] == 200, 'counter at 0 with nothing connected, reset')
        http('POST', '/api/gpio/set', {'gpio': 2, 'level': 1})
        time.sleep(6.5)  # the last values are saved 5 s after the last change
        http('PUT', '/api/gpio', {'pins': [{'gpio': 45, 'mode': 'output', 'open_drain': True, 'pull': 'down', 'restore': 'on'},
                                           {'gpio': 3, 'mode': 'output', 'pull': 'down', 'restore': 'on'}]})
        check(pin(45)['level'] == 1 and pin(45)['pin_level'] == 0 and pin(3)['pin_level'] == 1,
              'open drain with pull-down: level 1 releases the pin (reads 0), push-pull drives it (reads 1)')
        http('PUT', '/api/gpio', {'pins': [
            {'gpio': 2, 'mode': 'output', 'name': 'Relé', 'restore': 'last'},
            {'gpio': 3, 'mode': 'output', 'invert': True, 'restore': 'on'},
            {'gpio': 1, 'mode': 'pwm', 'freq': 1000, 'restore': 'last'}]})
        http('POST', '/api/reboot', {})
        wait_restart(status()['role']['desired'])
        p2, p1, p3 = pin(2), pin(1), pin(3)
        check(p2['level'] == 1 and p2['pin_level'] == 1 and p1['duty'] == 33 and p3['level'] == 1 and p3['pin_level'] == 0,
              f'after the restart: output last 1, PWM last {p1["duty"]} %, inverted output on (pin 0)')
    finally:
        http('PUT', '/api/gpio', keep)
    check(len(http('GET', '/api/gpio')[1]['pins']) == len(keep['pins']), 'configuration of before restored')


def tc53_gpio_mqtt():
    """GPIO of JP1 over MQTT against a plain test broker (--mqtt-broker host:port): the device "<host> GPIO" via the
    gateway with a switch, binary sensor, light (brightness = duty) and two counter sensors, retained state, commands
    ON/OFF/PULSE and duty, a mode change replaces the entity, the gpio group off removes them; then the original settings."""
    if not ARGS.mqtt_broker:
        raise Skip('needs --mqtt-broker <host>:<port> (anonymous mosquitto, e.g. mosquitto -p 18883)')
    import subprocess
    host, port = ARGS.mqtt_broker.rsplit(':', 1)
    code, backup = http('GET', '/api/settings/export?secrets=1')
    check(code == 200, 'original settings exported (restored by import at the end)')
    groups_before = http('GET', '/api/mqtt')[1]['discovery_groups']
    gpio_before = http('GET', '/api/gpio')[1]['pins']
    name = status()['hostname']
    base = f'p4gw/{name}'

    def sub(topic):
        out = subprocess.run(['mosquitto_sub', '-h', host, '-p', port, '-t', topic, '-v', '-W', '3', '-C', '1'],
                             capture_output=True, text=True).stdout.strip()
        return out.split(' ', 1)[1] if ' ' in out else ''

    def pub(topic, payload):
        subprocess.run(['mosquitto_pub', '-h', host, '-p', port, '-t', topic, '-m', payload], check=True)
        time.sleep(0.6)

    retained = lambda component, oid: sub(f'homeassistant/{component}/{name}/{oid}/config')
    pin = lambda g: next((p for p in http('GET', '/api/gpio')[1]['pins'] if p['gpio'] == g), None)
    try:
        check(http('PUT', '/api/gpio', {'pins': [
            {'gpio': 2, 'mode': 'output', 'name': 'Relé', 'expose': 'mqtt'},
            {'gpio': 3, 'mode': 'input', 'pull': 'up', 'name': 'Tlačítko', 'expose': 'both'},
            {'gpio': 1, 'mode': 'pwm', 'freq': 1000, 'expose': 'mqtt'},
            {'gpio': 45, 'mode': 'counter', 'pull': 'down', 'name': 'Vodoměr', 'expose': 'mqtt'}]})[0] == 200, 'pins set to MQTT')
        code, _ = http('POST', '/api/mqtt', {'uri': f'mqtt://{host}:{port}', 'user': '', 'password': '', 'tls': 'bundle', 'base': '',
                                             'prefix': 'homeassistant', 'discovery': True, 'discovery_groups': ['gpio'], 'enabled': True})
        check(code == 200 and wait_until('connected', lambda: http('GET', '/api/mqtt')[1]['connected'], 25, 1), 'test broker, group gpio only')
        time.sleep(2)
        sw = json.loads(retained('switch', 'gpio_2_output_level') or '{}')
        check(sw.get('name') == 'Relé' and sw.get('command_topic') == f'{base}/gpio/2/set' and sw['device']['identifiers'] == [f'{name}_gpio']
              and sw['device']['via_device'] == name, 'output: switch of the device "<host> GPIO" via the gateway')
        check(json.loads(retained('binary_sensor', 'gpio_3_input_level') or '{}').get('value_template', '').startswith("{{ 'ON' if value_json['3']"), 'input: binary sensor')
        light = json.loads(retained('light', 'gpio_1_pwm_duty') or '{}')
        check(light.get('name') == 'GPIO1' and light.get('brightness_scale') == 100 and light.get('brightness_command_topic') == f'{base}/gpio/1/duty/set',
              'PWM: light with brightness 0-100 (the duty)')
        freq = json.loads(retained('sensor', 'gpio_45_counter_frequency') or '{}')
        check(retained('sensor', 'gpio_45_counter_count') and freq.get('unit_of_measurement') == 'Hz', 'counter: count and frequency sensors')
        state = json.loads(sub(f'{base}/gpio') or '{}')
        check(state.get('3') == 1 and state.get('2') == 0 and state.get('45', {}).get('count') == 0, f'retained state {state}')
        pub(f'{base}/gpio/2/set', 'ON')
        p2 = wait_until('output on after ON', lambda: (lambda x: x if x['level'] == 1 else None)(pin(2)), 3, 0.3)
        state = json.loads(sub(f'{base}/gpio') or '{}')
        check(p2['pin_level'] == 1 and state.get('2') == 1, f'command ON: output on, state published {state}')
        pub(f'{base}/gpio/2/set', 'OFF')
        pub(f'{base}/gpio/2/set', 'PULSE 1500')
        check(pin(2)['pin_level'] == 1, 'command PULSE 1500: on')
        time.sleep(1.5)
        check(pin(2)['pin_level'] == 0, 'and off again')
        pub(f'{base}/gpio/1/duty/set', '30')
        check(pin(1)['duty'] == 30, 'duty 30 from the light')
        pub(f'{base}/gpio/1/set', 'OFF')
        check(pin(1)['duty'] == 0, 'light OFF: duty 0')
        http('PUT', '/api/gpio', {'pins': [{'gpio': 2, 'mode': 'input', 'expose': 'mqtt'}]})
        time.sleep(2)
        check(not retained('switch', 'gpio_2_output_level') and retained('binary_sensor', 'gpio_2_input_level')
              and not retained('light', 'gpio_1_pwm_duty'), 'mode changed and pins freed: old entities removed, new one published')
        check(http('POST', '/api/mqtt', {'discovery_groups': ['sound_triggers']})[0] == 200, 'group gpio off')
        time.sleep(3)
        check(not retained('binary_sensor', 'gpio_2_input_level'), 'GPIO entities removed (empty retained config)')
    finally:
        http('PUT', '/api/gpio', {'pins': [{k: p[k] for k in ('gpio', 'mode', 'name', 'expose', 'invert', 'pull', 'debounce_ms', 'open_drain', 'restore', 'freq', 'edge', 'filter_ns') if k in p}
                                           for p in gpio_before]})
        http('POST', '/api/mqtt', {'discovery_groups': groups_before})
        code, r = http('POST', '/api/settings/import', backup)
        check(code == 200, 'original settings imported, the gateway restarts')
        wait_restart(status()['role']['desired'])
    m = wait_until('MQTT back on the original broker', lambda: (lambda m: m if m['connected'] or not m['enabled'] else None)(http('GET', '/api/mqtt')[1]), 40, 2)
    check(m['discovery_groups'] == groups_before, f'original broker {m["uri"]}, groups {groups_before}')


TESTS = [
    ('TC01', tc01_first_start), ('TC02', tc02_backup_guard), ('TC03', tc03_c6_backup),
    ('TC04', tc04_thread_br), ('TC05', tc05_zigbee_coordinator), ('TC06', tc06_zigbee_data_kept),
    ('TC07', tc07_zigbee_router), ('TC08', tc08_thread_router), ('TC09', tc09_esphome_radio),
    ('TC10', tc10_ota_rollback_slots), ('TC11', tc11_auth), ('TC12', tc12_invalid_uploads),
    ('TC13', tc13_rcp_reflash), ('TC14', tc14_sound_service), ('TC15', tc15_sound_recognition),
    ('TC16', tc16_thread_dataset_isolation), ('TC17', tc17_sound_events_config),
    ('TC18', tc18_sound_events_detection), ('TC19', tc19_c6_telemetry), ('TC20', tc20_netscan),
    ('TC21', tc21_thread_topology), ('TC22', tc22_zigbee_net), ('TC23', tc23_slot_switch),
    ('TC24', tc24_previous_log), ('TC25', tc25_settings_backup),
    ('TC26', tc26_zigbee_backup), ('TC27', tc27_online_update_checks),
    ('TC28', tc28_learned_sounds), ('TC29', tc29_mqtt_options), ('TC30', tc30_ha_integration_interface),
    ('TC31', tc31_sound_pressure), ('TC32', tc32_lan_settings_traffic), ('TC33', tc33_full_restart), ('TC34', tc34_users), ('TC35', tc35_telegram), ('TC36', tc36_c6_online_update), ('TC37', tc37_release_list), ('TC39', tc39_two_radios), ('TC40', tc40_thread_per_radio),
    ('TC46', tc46_mqtt_discovery_groups), ('TC47', tc47_i2c_sensors), ('TC48', tc48_modbus), ('TC49', tc49_modbus_scan), ('TC50', tc50_modbus_goodwe_udp), ('TC51', tc51_time_settings), ('TC52', tc52_gpio), ('TC53', tc53_gpio_mqtt), ('TC41', tc41_wifi_uplink), ('TC42', tc42_wifi_failover), ('TC43', tc43_wifi_wrong_network),
    ('TC44', tc44_wifi_setup_portal), ('TC45', tc45_wifi_swap),
]


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default='p4gw.local')
    p.add_argument('--only', nargs='*')
    p.add_argument('--token', help='API token (role admin) of a gateway with user accounts')
    p.add_argument('--list', action='store_true')
    p.add_argument('--report', help='write results as JSON')
    p.add_argument('--play', action='store_true', help='TC15: play test sounds on this computer')
    p.add_argument('--mqtt-broker', metavar='HOST:PORT', help='TC46: plain anonymous test broker (mosquitto)')
    p.add_argument('--mqtt-test', metavar='HOST:DIR', help='TC29: test mosquitto (anonymous, TLS with DIR/ca.crt) on HOST')
    p.add_argument('--packages', help='TC45: directory with c6fw.bin and c5fw.bin holding the wifi_uplink bundle (before a release has it)')
    p.add_argument('--wifi-iface', help='TC44: Wi-Fi interface of this macOS host that joins the setup access point (e.g. en0)')
    ARGS = p.parse_args()
    if ARGS.list:
        for tid, fn in TESTS:
            print(f'{tid}  {fn.__doc__}')
        return 0
    # the single-radio cases switch radio A freely: radio B off meanwhile, its role back at the end
    selected = [t for t in TESTS if not ARGS.only or t[0] in ARGS.only]
    b_role = None
    # cases that leave radio B alone or set it up themselves
    keep_b = ('TC24', 'TC32', 'TC33', 'TC39', 'TC40', 'TC41', 'TC42', 'TC43', 'TC44', 'TC45', 'TC46', 'TC47', 'TC48', 'TC49', 'TC50', 'TC51', 'TC52', 'TC53')
    if any(tid not in keep_b for tid, _ in selected):
        try:
            b_role = status()['role'].get('b', {}).get('desired', 'none')
        except Exception:  # noqa: BLE001 - gateway without radio B support
            b_role = None
        if b_role and b_role != 'none':
            log(f'radio B {b_role} switched off for the run')
            switch_b('none')
    for tid, fn in selected:
        print(f'{tid} {fn.__doc__}', flush=True)
        t0 = time.time()
        try:
            fn()
            result, detail = 'PASS', ''
        except Fail as err:
            result, detail = 'FAIL', str(err)
        except Skip as err:
            result, detail = 'SKIP', str(err)
        except Exception as err:  # noqa: BLE001 - report and continue with the next case
            result, detail = 'ERROR', f'{type(err).__name__}: {err}'
        RESULTS.append({'id': tid, 'result': result, 'detail': detail, 'seconds': round(time.time() - t0)})
        print(f'{tid} {result} ({time.time() - t0:.0f} s) {detail}\n', flush=True)
    if b_role and b_role != 'none':
        try:
            switch_b(b_role)
            log(f'radio B {b_role} again')
        except Exception as err:  # noqa: BLE001
            print(f'radio B {b_role} not restored: {err}')
    print('Summary: ' + ', '.join(f'{r["id"]} {r["result"]}' for r in RESULTS))
    if ARGS.report:
        with open(ARGS.report, 'w') as f:
            json.dump(RESULTS, f, indent=2)
    return 0 if all(r['result'] in ('PASS', 'SKIP') for r in RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
