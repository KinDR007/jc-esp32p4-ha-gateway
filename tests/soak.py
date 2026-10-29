#!/usr/bin/env python3
"""Long run of the gateway with sound recognition on: samples the state every few minutes and
collects the detection log, then summarises stability and what was detected (sound phase 5).

  python3 tests/soak.py --host 192.168.88.243 --hours 24 --out tests/results/<date>/soak
  python3 tests/soak.py --summary tests/results/<date>/soak     # summary of a finished or running run

samples.jsonl: one status sample per interval; detections.jsonl: detection log entries (deduplicated).
"""
import argparse
import collections
import json
import os
import sys
import time
import urllib.request


def get(host, path, timeout=10):
    with urllib.request.urlopen(f'http://{host}{path}', timeout=timeout) as r:
        return json.loads(r.read())


def sample(host):
    s = get(host, '/api/status')
    snd = get(host, '/api/sound/status')
    inf = snd.get('inference', {})
    return {
        'time': int(time.time()), 'uptime_s': s['uptime_s'], 'reset': s['reset_reason'], 'version': s['version'],
        'role': s['role']['active'], 'maintenance': s['role']['maintenance'],
        'heap_free': s['heap']['internal_free'], 'heap_min': s['heap']['internal_min'], 'psram_free': s['heap']['psram_free'],
        'cpu_load': s['hw'].get('cpu_load'), 'temp_c': s.get('temperature_c'),
        'sound_running': snd.get('running'), 'inference': inf.get('enabled'), 'model_error': snd.get('model', {}).get('error', ''),
        'runs': inf.get('runs'), 'avg_ms': inf.get('avg_ms'), 'max_ms': inf.get('max_ms'), 'cpu_pct': inf.get('cpu_pct'),
        'late': inf.get('late_resyncs'), 'flash_skips': inf.get('flash_skips'),
        'c6_temp_c': (s['c6'].get('telemetry') or {}).get('temp_c'),
    }


def run(args):
    os.makedirs(args.out, exist_ok=True)
    seen = {}  # (class, event, rounded absolute time) of logged detections
    end = time.time() + args.hours * 3600
    samples = open(os.path.join(args.out, 'samples.jsonl'), 'a')
    detections = open(os.path.join(args.out, 'detections.jsonl'), 'a')
    while time.time() < end:
        now = time.time()
        try:
            rec = sample(args.host)
            for e in get(args.host, '/api/sound/log'):
                at = now - e['ago_s']
                key = (e['class'], e['event'], round(at / 10))
                if key in seen or any((e['class'], e['event'], key[2] + d) in seen for d in (-1, 1)):
                    continue
                seen[key] = True
                detections.write(json.dumps(dict(e, time=int(at))) + '\n')
            detections.flush()
        except Exception as err:  # the gateway restarting or unreachable is a result too
            rec = {'time': int(now), 'error': str(err)}
        samples.write(json.dumps(rec) + '\n')
        samples.flush()
        time.sleep(args.interval)
    summary(args.out)


def summary(out):
    path = os.path.join(out, 'samples.jsonl')
    if not os.path.exists(path):
        print(f'no {path}', file=sys.stderr)
        return 1
    rows = [json.loads(l) for l in open(path)]
    ok = [r for r in rows if 'error' not in r]
    if not ok:
        print('no samples')
        return
    hours = (rows[-1]['time'] - rows[0]['time']) / 3600
    restarts = sum(1 for a, b in zip(ok, ok[1:]) if b['uptime_s'] < a['uptime_s'])
    print(f'{hours:.1f} h, {len(rows)} samples, {len(rows) - len(ok)} unreachable, {restarts} restarts '
          f'(last reset reason {ok[-1]["reset"]}), version {ok[-1]["version"]}, role {ok[-1]["role"]}')
    print(f'internal heap free {min(r["heap_free"] for r in ok)}-{max(r["heap_free"] for r in ok)} B, '
          f'lowest ever {min(r["heap_min"] for r in ok)} B; PSRAM free min {min(r["psram_free"] for r in ok)} B')
    cpu = [r['cpu_pct'] for r in ok if r.get('cpu_pct') is not None]
    ms = [r['max_ms'] for r in ok if r.get('max_ms') is not None]
    print(f'inference: still on in the last sample {ok[-1]["inference"]}, CPU of core 1 {min(cpu, default=0)}-{max(cpu, default=0)} %, '
          f'max {max(ms, default=0)} ms, model error {ok[-1]["model_error"]!r}')
    temps = [r['temp_c'] for r in ok if r.get('temp_c') is not None]
    c6 = [r['c6_temp_c'] for r in ok if r.get('c6_temp_c') is not None]
    print(f'temperature P4 {min(temps, default=0)}-{max(temps, default=0)} °C, C6 {min(c6, default=0)}-{max(c6, default=0)} °C')
    path = os.path.join(out, 'detections.jsonl')
    det = [json.loads(l) for l in open(path)] if os.path.exists(path) else []
    events = collections.Counter(d['event'] for d in det if d['event'])
    unassigned = collections.Counter(d['class_name'] for d in det if not d['event'])
    print(f'events ({sum(events.values())}): ' + ', '.join(f'{k} {v}' for k, v in events.most_common()))
    print('event sounds: ' + ', '.join(f'{k} {v}' for k, v in collections.Counter(
        f'{d["event"]}/{d["class_name"]}' for d in det if d['event']).most_common(15)))
    print(f'strong sounds without an event ({sum(unassigned.values())}): ' + ', '.join(f'{k} {v}' for k, v in unassigned.most_common(15)))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--host', default='p4gw.local')
    p.add_argument('--hours', type=float, default=24)
    p.add_argument('--interval', type=int, default=300, help='seconds between samples')
    p.add_argument('--out', default='soak')
    p.add_argument('--summary', metavar='DIR')
    args = p.parse_args()
    if args.summary:
        return summary(args.summary)
    run(args)


if __name__ == '__main__':
    sys.exit(main())
