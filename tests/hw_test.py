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


def http(method, path, body=None, raw=None, timeout=30, port=80, auth=None):
    url = f'http://{ARGS.host}:{port}{path}'
    data = raw if raw is not None else (json.dumps(body).encode() if body is not None else None)
    req = urllib.request.Request(url, data=data, method=method)
    if body is not None:
        req.add_header('Content-Type', 'application/json')
    if auth:
        import base64
        req.add_header('Authorization', 'Basic ' + base64.b64encode(f'admin:{auth}'.encode()).decode())
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


def logs_since(seq=0):
    req = urllib.request.Request(f'http://{ARGS.host}/api/logs?since={seq}')
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
    code, t = http('GET', '/api/thread/status')
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
    code, t = http('GET', '/api/thread/status')
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
    """Password protects the API; it can be removed again."""
    pw = 'hwtest-' + str(int(time.time()))
    check(http('POST', '/api/settings', {'password': pw})[0] == 200, 'password set')
    check(http('GET', '/api/status')[0] == 401, '401 without credentials')
    check(http('GET', '/api/status', auth=pw)[0] == 200, '200 with credentials')
    check(http('GET', '/api/status', auth='wrong')[0] == 401, '401 with a wrong password')
    check(http('POST', '/api/settings', {'password': ''}, auth=pw)[0] == 200, 'password removed')
    check(http('GET', '/api/status')[0] == 200, 'open again')


def tc12_invalid_uploads():
    """Broken uploads are rejected and do not damage the stored images."""
    code, r = http('POST', '/api/ota/c6fw', raw=b'\x00' * 8192)
    check(code == 400, f'garbage c6fw rejected ({r})')
    check(len(status()['c6']['images']) == 4, 'stored bundles intact after the rejected upload')
    code, r = http('POST', '/api/ota/gateway', raw=b'\xe9' + b'\x00' * 8191)
    check(code == 400, f'garbage gateway image rejected ({r})')
    with open(os.path.join(ROOT, 'dist', 'c6fw.bin'), 'rb') as f:
        code, r = http('POST', '/api/ota/c6fw', raw=f.read(), timeout=120)
    check(code == 200, 'valid c6fw re-uploaded')
    check(len(status()['c6']['images']) == 4, 'four bundles listed again')


def tc13_rcp_reflash():
    """Forced C6 reflash from the UI restores the current role's firmware."""
    role = status()['role']['active']
    code, _ = http('POST', '/api/c6/reflash', {})
    check(code == 200, 'reflash requested')
    s = wait_restart(role)
    check(s['role']['active'] == role and not s['role']['maintenance'], f'{role} running again after reflash')
    check('C6 now runs' in logs_since(0), 'flash log present')


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


TESTS = [
    ('TC01', tc01_first_start), ('TC02', tc02_backup_guard), ('TC03', tc03_c6_backup),
    ('TC04', tc04_thread_br), ('TC05', tc05_zigbee_coordinator), ('TC06', tc06_zigbee_data_kept),
    ('TC07', tc07_zigbee_router), ('TC08', tc08_thread_router), ('TC09', tc09_esphome_roundtrip),
    ('TC10', tc10_ota_rollback_slots), ('TC11', tc11_auth), ('TC12', tc12_invalid_uploads),
    ('TC13', tc13_rcp_reflash), ('TC14', tc14_sound_service), ('TC15', tc15_sound_recognition),
    ('TC16', tc16_thread_dataset_isolation),
]


def main():
    global ARGS
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default='p4gw.local')
    p.add_argument('--only', nargs='*')
    p.add_argument('--esphome-key')
    p.add_argument('--list', action='store_true')
    p.add_argument('--report', help='write results as JSON')
    p.add_argument('--play', action='store_true', help='TC15: play test sounds on this computer')
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
