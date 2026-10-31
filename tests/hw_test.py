#!/usr/bin/env python3
"""Hardware test of the P4 gateway: every role and the transitions between them.

Runs against a real board over the LAN. Test cases are described in tests/TESTCASES.md.

    python3 tests/hw_test.py --host 192.168.88.243            # full run, in order
    python3 tests/hw_test.py --host p4gw-xxxxxx.local --only TC04 TC05
    python3 tests/hw_test.py --list

TC09 (ESPHome) needs aioesphomeapi and the API key; run it with the ESPHome venv python:
    ../esphome/ble_proxy/.venv/bin/python tests/hw_test.py --host ... --esphome-key "$(...)"

Destructive: switches roles, reflashes the C6 and writes the gateway slot.
"""
import argparse
import glob
import json
import os
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


def tc09_esphome_roundtrip():
    """ESPHome: gateway hands over, BLE proxy scans, Return to gateway brings back thread_br."""
    try:
        from aioesphomeapi import APIClient
    except ImportError:
        raise Fail('aioesphomeapi missing, run with the ESPHome venv python')
    if not ARGS.esphome_key:
        raise Fail('--esphome-key required')
    import asyncio
    code, r = http('POST', '/api/role', {'role': 'esphome'})
    check(code == 200, 'switch to esphome accepted')
    wait_until('ESPHome API port 6053', lambda: port_open(6053), 240, 3)
    check(not port_open(80), 'gateway web UI is gone (ESPHome runs)')

    async def run():
        cli = APIClient(ARGS.host, 6053, None, noise_psk=ARGS.esphome_key)
        await cli.connect(login=True)
        info = await cli.device_info()
        seen = {}
        unsub = cli.subscribe_bluetooth_le_raw_advertisements(lambda r: [seen.__setitem__(a.address, a.rssi) for a in r.advertisements])
        await asyncio.sleep(10)
        unsub()
        entities, _ = await cli.list_entities_services()
        sel = next(e for e in entities if e.object_id == 'gateway_role')
        btn = next(e for e in entities if e.object_id == 'return_to_gateway')
        cli.select_command(sel.key, 'Thread border router')
        await asyncio.sleep(0.5)
        cli.button_command(btn.key)
        await asyncio.sleep(1)
        await cli.disconnect()
        return info, seen

    info, seen = asyncio.run(run())
    check(True, f'ESPHome {info.name} {info.esphome_version}')
    check(len(seen) > 0, f'BLE advertisements from {len(seen)} devices in 10 s')
    s = wait_restart('thread_br', 240)
    check(s['role']['active'] == 'thread_br' and not s['role']['maintenance'], 'returned to thread_br')
    check(s['c6']['bundle'] == 'ot_rcp', 'C6 reflashed back to ot_rcp')


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
    code, r = http('POST', '/api/ota/esphome', raw=image, timeout=120)
    check(code in (400, 413), f'gateway image refused for the ESPHome slot ({code})')
    esphome = next(x for x in status()['slots'] if x['label'] == 'esphome')
    check(esphome.get('project') == 'ble-proxy-p4gw', 'ESPHome slot left intact')


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
    check('Zigbee network backup: ESP_OK' in logs_since(0), 'backup logged')
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

    check(http('POST', '/api/settings', {'update_url': 'ftp://x/m.json'})[0] == 400, 'non-HTTP URL refused')
    http('POST', '/api/settings', {'update_url': ''})
    check(http('POST', '/api/update/check')[0] == 409, 'check without a URL refused')
    check(http('POST', '/api/update/install')[0] == 409, 'install without an offer refused')
    u = checked(f'http://{ARGS.host}/nope.json')
    check(u['error'] == 'manifest: HTTP 404' and not u['available'], f'missing manifest: {u["error"]}')
    u = checked(f'http://{ARGS.host}/api/mqtt')
    check('expected' in u['error'] and not u['available'], f'not a manifest: {u["error"]}')
    u = checked('https://example.com/p4gw/manifest.json')
    check(u['error'] == 'manifest: HTTP 404', f'HTTPS reaches the server (certificate bundle): {u["error"]}')
    check(http('POST', '/api/update/install')[0] == 409, 'still nothing to install')
    http('POST', '/api/settings', {'update_url': ''})
    check(status()['update']['url'] == '', 'URL cleared')


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
    s = wait_restart('thread_br')
    check(s['c6']['bundle'] == 'ot_rcp' and not s['role']['maintenance'], 'role firmware back on the C6')
    t2 = wait_thread_running()
    check(t2['network_name'] == t['network_name'], 'Thread network unchanged after the scan')


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

    def __init__(self, path, auth=None, token=None):
        import base64
        self.sock = socket.create_connection((ARGS.host, 80), timeout=10)
        key = base64.b64encode(os.urandom(16)).decode()
        hdr = f'GET {path} HTTP/1.1\r\nHost: {ARGS.host}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n' \
              f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n'
        if auth:
            hdr += 'Authorization: Basic ' + base64.b64encode(auth.encode()).decode() + '\r\n'
        if token or (ARGS.token and not auth):
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
    check(http('POST', '/api/users', {'name': 'hwtest', 'role': 'admin', 'password': pw})[0] == 200, 'admin account created')
    try:
        ws = WsClient('/api/ws')
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
        check(http('POST', '/api/users/delete', {'name': 'hwtest'}, auth=pw, user='hwtest')[0] == 200, 'account deleted')


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
    check(http('POST', '/api/reboot', {'full': True})[0] == 200, 'full restart accepted')
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
    """releases.json: only releases that name the board's flash layout are listed; a picked release whose
    manifest does not run on the layout, or an unlisted one, is refused without a restart."""
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
        {'version': 'v0.0-nolayout', 'manifest': f'{base}/nolayout.json', 'layouts': [layout]}]})
    before_url = s0['update']['url']
    try:
        check(http('POST', '/api/settings', {'update_url': f'{base}/manifest.json'})[0] == 200, 'manifest URL set')
        u = checked()
        versions = [r['version'] for r in u['releases']]
        check(versions == [current, 'v0.0-nolayout'], f'listed for layout {layout}: {versions}')
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


def switch_b(role, timeout=300):
    """Role of radio B (docs/dual-radio.md); waits for the restart and radio B's start."""
    up = status()['uptime_s']
    code, r = http('POST', '/api/role', {'role': role, 'radio': 'b'})
    if code != 200:
        raise Fail(f'radio B role {role}: {code} {r}')

    def ready():
        s = status()
        rb = s['role'].get('b', {})
        if s['uptime_s'] >= up or not s['role']['stable'] and rb.get('active') != role and not rb.get('maintenance'):
            return None
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
        t = http('GET', '/api/thread/status')[1]
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


TESTS = [
    ('TC01', tc01_first_start), ('TC02', tc02_backup_guard), ('TC03', tc03_c6_backup),
    ('TC04', tc04_thread_br), ('TC05', tc05_zigbee_coordinator), ('TC06', tc06_zigbee_data_kept),
    ('TC07', tc07_zigbee_router), ('TC08', tc08_thread_router), ('TC09', tc09_esphome_roundtrip),
    ('TC10', tc10_ota_rollback_slots), ('TC11', tc11_auth), ('TC12', tc12_invalid_uploads),
    ('TC13', tc13_rcp_reflash), ('TC14', tc14_sound_service), ('TC15', tc15_sound_recognition),
    ('TC16', tc16_thread_dataset_isolation), ('TC17', tc17_sound_events_config),
    ('TC18', tc18_sound_events_detection), ('TC19', tc19_c6_telemetry), ('TC20', tc20_netscan),
    ('TC21', tc21_thread_topology), ('TC22', tc22_zigbee_net), ('TC23', tc23_slot_switch),
    ('TC24', tc24_previous_log), ('TC25', tc25_settings_backup),
    ('TC26', tc26_zigbee_backup), ('TC27', tc27_online_update_checks),
    ('TC28', tc28_learned_sounds), ('TC29', tc29_mqtt_options), ('TC30', tc30_ha_integration_interface),
    ('TC31', tc31_sound_pressure), ('TC32', tc32_lan_settings_traffic), ('TC33', tc33_full_restart), ('TC34', tc34_users), ('TC35', tc35_telegram), ('TC36', tc36_c6_online_update), ('TC37', tc37_release_list), ('TC39', tc39_two_radios),
]


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default='p4gw.local')
    p.add_argument('--only', nargs='*')
    p.add_argument('--token', help='API token (role admin) of a gateway with user accounts')
    p.add_argument('--esphome-key')
    p.add_argument('--list', action='store_true')
    p.add_argument('--report', help='write results as JSON')
    p.add_argument('--play', action='store_true', help='TC15: play test sounds on this computer')
    p.add_argument('--mqtt-test', metavar='HOST:DIR', help='TC29: test mosquitto (anonymous, TLS with DIR/ca.crt) on HOST')
    ARGS = p.parse_args()
    if ARGS.list:
        for tid, fn in TESTS:
            print(f'{tid}  {fn.__doc__}')
        return 0
    for tid, fn in TESTS:
        if ARGS.only and tid not in ARGS.only:
            continue
        print(f'{tid} {fn.__doc__}', flush=True)
        t0 = time.time()
        try:
            fn()
            result, detail = 'PASS', ''
        except Fail as err:
            result, detail = 'FAIL', str(err)
        except Exception as err:  # noqa: BLE001 - report and continue with the next case
            result, detail = 'ERROR', f'{type(err).__name__}: {err}'
        RESULTS.append({'id': tid, 'result': result, 'detail': detail, 'seconds': round(time.time() - t0)})
        print(f'{tid} {result} ({time.time() - t0:.0f} s) {detail}\n', flush=True)
    print('Summary: ' + ', '.join(f'{r["id"]} {r["result"]}' for r in RESULTS))
    if ARGS.report:
        with open(ARGS.report, 'w') as f:
            json.dump(RESULTS, f, indent=2)
    return 0 if all(r['result'] == 'PASS' for r in RESULTS) else 1


if __name__ == '__main__':
    sys.exit(main())
