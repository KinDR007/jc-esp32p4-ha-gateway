#!/usr/bin/env python3
"""Load test of the gateway's Modbus TCP server (docs/modbus.md, phase S5), no third party modules.

Starts the Modbus TCP simulator of this computer with three Eastron SDM630 (units 1-3), adds them to the
gateway (poll 3 s), offers them on the shared port 502 (unit 10) and on own ports 1505-1507 (rate 50/s),
then runs 8 clients at once (the limit: 2 on each port), each alternating a read the cache answers (input
0-5) with one that goes to the device (holding 0xfc00, polled once only). Meanwhile it samples /api/status
(internal heap, uptime, role) and its answer time, and the Modbus polls. The configuration of before comes
back.

  server_load.py --host 192.168.88.243 --token <admin API token> [--seconds 60] [--interval 0.5]
  (--interval: pause of each client between requests; 0 = as fast as the gateway answers)
"""
import argparse
import json
import socket
import statistics
import struct
import subprocess
import sys
import threading
import time
import urllib.request
from pathlib import Path

SIM = Path(__file__).resolve().parent / 'sim_tcp.py'


def http(a, method, path, body=None):
    req = urllib.request.Request(f'http://{a.host}{path}', method=method, data=json.dumps(body).encode() if body is not None else None,
                                 headers={'Authorization': f'Bearer {a.token}', 'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=20) as f:
        return json.loads(f.read() or b'{}')


def request(sock, tid, unit, pdu):
    sock.sendall(struct.pack('>HHHB', tid, 0, len(pdu) + 1, unit) + pdu)
    head = b''
    while len(head) < 7:
        chunk = sock.recv(7 - len(head))
        if not chunk:
            raise ConnectionError('closed')
        head += chunk
    length = struct.unpack('>H', head[4:6])[0]
    body = b''
    while len(body) < length - 1:
        chunk = sock.recv(length - 1 - len(body))
        if not chunk:
            raise ConnectionError('closed')
        body += chunk
    return body


def client(a, port, unit, stop, out):
    lat = {'cache': [], 'device': []}
    errors = {}
    try:
        s = socket.create_connection((a.host, port), timeout=10)
    except OSError as e:
        out.append({'port': port, 'error': str(e)})
        return
    tid = 0
    while not stop.is_set():
        for kind, pdu in (('cache', bytes([4, 0, 0, 0, 6])), ('device', bytes([3, 0xfc, 0, 0, 2]))):
            tid = (tid + 1) & 0xffff
            t0 = time.perf_counter()
            try:
                ans = request(s, tid, unit, pdu)
            except OSError as e:
                errors[type(e).__name__] = errors.get(type(e).__name__, 0) + 1
                stop.wait(1)
                try:
                    s = socket.create_connection((a.host, port), timeout=10)
                except OSError:
                    pass
                continue
            if ans[0] & 0x80:
                errors[f'exception {ans[1]}'] = errors.get(f'exception {ans[1]}', 0) + 1
            else:
                lat[kind].append((time.perf_counter() - t0) * 1000)
            stop.wait(a.interval)
    s.close()
    out.append({'port': port, 'unit': unit, 'lat': lat, 'errors': errors})


def pct(v, p):
    return sorted(v)[min(len(v) - 1, int(len(v) * p))] if v else 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', required=True)
    p.add_argument('--token', required=True)
    p.add_argument('--seconds', type=int, default=60)
    p.add_argument('--sim-port', type=int, default=1504)
    p.add_argument('--interval', type=float, default=0)
    a = p.parse_args()
    here = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    here.connect((a.host, 80))
    my_ip = here.getsockname()[0]
    here.close()
    sim = subprocess.Popen([sys.executable, str(SIM), '--port', str(a.sim_port)] + [f'eastron_sdm630:{u}' for u in (1, 2, 3)],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    before = http(a, 'GET', '/api/modbus')
    keep = {'devices': [{k: d[k] for k in ('id', 'name', 'template', 'transport', 'slave', 'host', 'port', 'poll_s', 'expose', 'enabled') if k in d}
                        for d in before['devices']]}
    srv = http(a, 'GET', '/api/modbus/server')
    srv_keep = {'enabled': srv['enabled'], 'port': srv['port'],
                'devices': [{k: e[k] for k in ('device', 'unit', 'port', 'write', 'allow', 'rate')} for e in srv['devices']]}
    try:
        time.sleep(1)
        devs = [{'name': f'Load {u}', 'template': 'eastron_sdm630', 'transport': 'tcp', 'host': my_ip, 'port': a.sim_port, 'slave': u,
                 'poll_s': 3, 'expose': 'none'} for u in (1, 2, 3)]
        http(a, 'PUT', '/api/modbus', {'devices': keep['devices'] + devs})
        ids = [next(d['id'] for d in http(a, 'GET', '/api/modbus')['devices'] if d['name'] == f'Load {u}') for u in (1, 2, 3)]
        http(a, 'PUT', '/api/modbus/server', {'enabled': True, 'port': 502, 'devices': [
            {'device': ids[0], 'unit': 10, 'port': 1505, 'rate': 50}, {'device': ids[1], 'port': 1506, 'rate': 50},
            {'device': ids[2], 'port': 1507, 'rate': 50}]})
        time.sleep(12)  # the strict simulator: values one by one from the second poll
        base = http(a, 'GET', '/api/status')
        polls0 = {d['id']: d['polls'] for d in http(a, 'GET', '/api/modbus')['devices']}
        stop, out, samples = threading.Event(), [], []
        clients = [threading.Thread(target=client, args=(a, port, unit, stop, out))
                   for port, unit in ((502, 10), (502, 10), (1505, 1), (1505, 1), (1506, 1), (1506, 1), (1507, 1), (1507, 1))]
        for c in clients:
            c.start()
        end = time.time() + a.seconds
        while time.time() < end:
            t0 = time.perf_counter()
            s = http(a, 'GET', '/api/status')
            samples.append({'ms': (time.perf_counter() - t0) * 1000, 'free': s['heap']['internal_free'], 'min': s['heap']['internal_min'],
                            'uptime': s['uptime_s'], 'role': s['role']['active']})
            time.sleep(2)
        stop.set()
        for c in clients:
            c.join(timeout=15)
        srv = http(a, 'GET', '/api/modbus/server')
        polls1 = {d['id']: d['polls'] for d in http(a, 'GET', '/api/modbus')['devices']}
        cache = [x for o in out for x in o.get('lat', {}).get('cache', [])]
        dev = [x for o in out for x in o.get('lat', {}).get('device', [])]
        errors = {}
        for o in out:
            for k, v in o.get('errors', {}).items():
                errors[k] = errors.get(k, 0) + v
        report = {
            'seconds': a.seconds, 'clients': len(out), 'connect_errors': [o for o in out if 'error' in o],
            'answers_per_s': round((len(cache) + len(dev)) / a.seconds, 1),
            'cache_ms': {'n': len(cache), 'p50': round(pct(cache, 0.5), 1), 'p95': round(pct(cache, 0.95), 1)},
            'device_ms': {'n': len(dev), 'p50': round(pct(dev, 0.5), 1), 'p95': round(pct(dev, 0.95), 1)},
            'errors': errors,
            'api_status_ms': {'p50': round(statistics.median(x['ms'] for x in samples), 1), 'max': round(max(x['ms'] for x in samples), 1)},
            'internal_free': {'before': base['heap']['internal_free'], 'min_during': min(x['free'] for x in samples),
                              'internal_min': samples[-1]['min']},
            'restarted': any(b['uptime'] < a_['uptime'] for a_, b in zip(samples, samples[1:])),
            'role': sorted({x['role'] for x in samples}),
            'polls_during': {i: polls1.get(i, 0) - polls0.get(i, 0) for i in ids},
            'server': [{k: e[k] for k in ('device', 'requests', 'cached', 'passed', 'refused', 'failed')} for e in srv['devices']],
        }
        print(json.dumps(report, indent=1))
    finally:
        http(a, 'PUT', '/api/modbus/server', srv_keep)
        http(a, 'PUT', '/api/modbus', keep)
        sim.terminate()
        sim.wait(timeout=5)


if __name__ == '__main__':
    sys.exit(main())
