#!/usr/bin/env python3
"""Modbus TCP simulator of a Huawei SUN2000 hybrid PV inverter with a LUNA2000 battery and a power meter
(no third party modules).

The data follow a simulated day: PV power along the sun with drifting clouds, a household load with
spikes, the battery charging from surplus and covering deficits, energy counters counting up. The
register map is the inverter's (huawei_sun2000.json, the template for the gateway, reads it):
holding registers (function 3), big endian, scaled integers. Undefined addresses answer exception 2,
more than 125 registers exception 3, like the inverter.

  huawei_sun2000_sim.py [--port 1502] [--unit 1] [--speed 60] [--start 11:30] [--seed 1] [-v]

Writes: 40125 active power limit (% x 10), 40200 startup, 40201 shutdown (functions 6 and 16).

To simulate another device, copy this file and change REGISTERS, WRITES and Model.step()."""
import argparse
import math
import random
import socketserver
import struct
import sys
import threading
import time
from datetime import datetime, timedelta

RATED_W = 10000       # SUN2000-10KTL-M1
PV_PEAK_W = 11500     # DC peak of the two strings
BATTERY_WH = 10000    # LUNA2000-10
BATTERY_MAX_W = 5000

# address, type, gain (raw = value * gain), quantity of Model.v
REGISTERS = [
    (30000, 'str15', 1, 'model'),
    (30015, 'str10', 1, 'serial'),
    (30073, 'u32', 1, 'rated_power'),
    (32016, 's16', 10, 'pv1_voltage'),
    (32017, 's16', 100, 'pv1_current'),
    (32018, 's16', 10, 'pv2_voltage'),
    (32019, 's16', 100, 'pv2_current'),
    (32064, 's32', 1, 'input_power'),
    (32066, 'u16', 10, 'line_voltage_ab'),
    (32067, 'u16', 10, 'line_voltage_bc'),
    (32068, 'u16', 10, 'line_voltage_ca'),
    (32069, 'u16', 10, 'voltage_a'),
    (32070, 'u16', 10, 'voltage_b'),
    (32071, 'u16', 10, 'voltage_c'),
    (32072, 's32', 1000, 'current_a'),
    (32074, 's32', 1000, 'current_b'),
    (32076, 's32', 1000, 'current_c'),
    (32078, 's32', 1, 'day_peak_power'),
    (32080, 's32', 1, 'active_power'),
    (32082, 's32', 1, 'reactive_power'),
    (32084, 's16', 1000, 'power_factor'),
    (32085, 'u16', 100, 'frequency'),
    (32086, 'u16', 100, 'efficiency'),
    (32087, 's16', 10, 'temperature'),
    (32088, 'u16', 1000, 'insulation'),
    (32089, 'u16', 1, 'status'),
    (32090, 'u16', 1, 'fault_code'),
    (32106, 'u32', 100, 'total_yield'),
    (32114, 'u32', 100, 'daily_yield'),
    (37113, 's32', 1, 'meter_power'),       # > 0 feeding into the grid
    (37119, 's32', 100, 'grid_export'),
    (37121, 's32', 100, 'grid_import'),
    (37760, 'u16', 10, 'battery_soc'),
    (37762, 'u16', 1, 'battery_status'),
    (37765, 's32', 1, 'battery_power'),     # > 0 charging
    (40125, 's16', 10, 'power_limit'),
]
WRITES = {40125: 'power_limit', 40200: 'startup', 40201: 'shutdown'}  # 40200/40201: write only

ST_STANDBY_NO_SUN, ST_ON_GRID, ST_LIMITED, ST_SHUTDOWN_CMD = 0xA000, 0x0200, 0x0201, 0x0301
BAT_STANDBY, BAT_RUNNING = 1, 2


def words(typ, raw):
    """the registers of a raw value"""
    if typ.startswith('str'):
        b = str(raw).encode()[:2 * int(typ[3:])].ljust(2 * int(typ[3:]), b'\0')
    else:
        n = 2 if typ[1:] == '32' else 1
        b = int(raw).to_bytes(2 * n, 'big', signed=typ[0] == 's')
    return [int.from_bytes(b[i:i + 2], 'big') for i in range(0, len(b), 2)]


class Model:
    """the inverter's quantities, stepped along simulated time"""

    def __init__(self, start, speed, rng):
        self.rng = rng
        self.speed = speed
        self.clock = start
        self.running = True
        self.cloud = 0.85
        self.load = 600.0
        self.spike = 0.0
        self.temp = 30.0
        self.soc = 55.0
        self.v = {
            'model': 'SUN2000-10KTL-M1', 'serial': 'HV2350123456', 'rated_power': RATED_W,
            'fault_code': 0, 'power_limit': 100.0, 'day_peak_power': 0,
            'total_yield': 18432.57, 'daily_yield': 0.0, 'grid_export': 9120.40, 'grid_import': 3075.12,
        }
        self.lock = threading.Lock()
        self.step(0)

    def sun(self):
        """0..1, a summer day in central Europe (sunrise 6:00, sunset 20:00, local time)"""
        h = self.clock.hour + self.clock.minute / 60 + self.clock.second / 3600
        return math.sin(math.pi * (h - 6) / 14) ** 1.3 if 6 < h < 20 else 0.0

    def step(self, dt):
        """advance dt simulated seconds"""
        r, v = self.rng, self.v
        day = self.clock.date()
        self.clock += timedelta(seconds=dt)
        if self.clock.date() != day:
            v['daily_yield'], v['day_peak_power'] = 0.0, 0
        k = math.sqrt(max(dt, 1) / 60)  # random walks scaled to the step
        self.cloud = min(1.0, max(0.25, self.cloud + r.gauss(0.01 * (0.8 - self.cloud), 0.04 * k)))
        self.load = min(3500.0, max(250.0, self.load + r.gauss(0.05 * (600 - self.load), 60 * k)))
        if self.spike > 0:
            self.spike = max(0.0, self.spike - dt)
        elif r.random() < 0.01 * k:
            self.spike = r.uniform(120, 600)  # kettle, oven: a few minutes
        load = self.load + (2000 if self.spike else 0)

        pv = PV_PEAK_W * self.sun() * self.cloud if self.running else 0.0
        limit = RATED_W * v['power_limit'] / 100
        eff = 0.985 - 0.035 * math.exp(-pv / 1500)
        charge = 0.0
        if self.running:
            surplus = pv * eff - load
            if surplus > 0 and self.soc < 100:
                charge = min(surplus, BATTERY_MAX_W)
            elif surplus < 0 and self.soc > 5:
                charge = -min(-surplus, BATTERY_MAX_W)
        active = pv * eff - charge
        status = ST_ON_GRID
        if active > limit:  # PV curtailed down to the limit
            active = limit
            pv = (active + charge) / eff
            status = ST_LIMITED
        if not self.running:
            status = ST_SHUTDOWN_CMD
        elif pv < 20 and charge == 0:
            status, active = ST_STANDBY_NO_SUN, 0.0
        self.soc = min(100.0, max(5.0, self.soc + charge * (0.95 if charge > 0 else 1) * dt / 3600 / BATTERY_WH * 100))
        meter = active - load

        # PV strings: about 55 % / 45 % of the power, voltage near the MPP
        if pv >= 20:
            v1 = 580 + 40 * self.sun() + r.gauss(0, 2)
            v2 = 470 + 35 * self.sun() + r.gauss(0, 2)
            v.update(pv1_voltage=v1, pv1_current=0.55 * pv / v1, pv2_voltage=v2, pv2_current=0.45 * pv / v2)
        else:
            v.update(pv1_voltage=0.0, pv1_current=0.0, pv2_voltage=0.0, pv2_current=0.0)

        phase = [230 + r.gauss(0.00025 * max(meter, 0), 1.0) for _ in range(3)]
        reactive = r.gauss(0, 60) if active > 0 else 0.0
        self.temp += (25 + 25 * active / RATED_W - self.temp) * min(1.0, dt / 600)
        v.update(
            input_power=round(pv), active_power=round(active), reactive_power=round(reactive),
            voltage_a=phase[0], voltage_b=phase[1], voltage_c=phase[2],
            line_voltage_ab=(phase[0] + phase[1]) / 2 * math.sqrt(3),
            line_voltage_bc=(phase[1] + phase[2]) / 2 * math.sqrt(3),
            line_voltage_ca=(phase[2] + phase[0]) / 2 * math.sqrt(3),
            current_a=active / 3 / phase[0], current_b=active / 3 / phase[1], current_c=active / 3 / phase[2],
            power_factor=active / math.hypot(active, reactive) if active > 0 else 1.0,
            frequency=50 + r.gauss(0, 0.015), efficiency=eff * 100 if active > 0 else 0.0,
            temperature=self.temp, insulation=3.0 + r.gauss(0, 0.05) if self.running else 0.0,
            status=status, meter_power=round(meter), battery_power=round(charge),
            battery_soc=self.soc, battery_status=BAT_RUNNING if charge else BAT_STANDBY,
        )
        v['day_peak_power'] = max(v['day_peak_power'], v['active_power'])
        v['daily_yield'] += active * dt / 3.6e6
        v['total_yield'] += active * dt / 3.6e6
        v['grid_export' if meter > 0 else 'grid_import'] += abs(meter) * dt / 3.6e6

    def registers(self):
        """address -> register value of everything readable"""
        regs = {}
        for addr, typ, gain, q in REGISTERS:
            raw = self.v[q] if typ.startswith('str') else round(self.v[q] * gain)
            for i, w in enumerate(words(typ, raw)):
                regs[addr + i] = w
        return regs

    def write(self, addr, values):
        """False: not writable (exception 2)"""
        if addr not in WRITES or len(values) != 1:
            return False
        what = WRITES[addr]
        if what == 'power_limit':
            pct = struct.unpack('>h', struct.pack('>H', values[0]))[0] / 10
            if not 0 <= pct <= 100:
                return False
            self.v['power_limit'] = pct
        else:
            self.running = what == 'startup'
        print(f'write {addr} = {values[0]} ({what})', flush=True)
        self.step(0)
        return True


class Handler(socketserver.BaseRequestHandler):
    model = None
    unit = 1

    def handle(self):
        sock = self.request
        while True:
            head = recv(sock, 7)
            if not head:
                return
            tid, _, length, unit = struct.unpack('>HHHB', head)
            pdu = recv(sock, length - 1)
            if pdu is None:
                return
            if unit != self.unit:
                continue  # no answer, like a dongle without that inverter
            with self.model.lock:
                answer = self.answer(pdu)
            sock.sendall(struct.pack('>HHHB', tid, 0, len(answer) + 1, unit) + answer)

    def answer(self, pdu):
        fc = pdu[0]
        exc = lambda code: bytes([fc | 0x80, code])
        if fc == 3:
            addr, count = struct.unpack('>HH', pdu[1:5])
            if not 1 <= count <= 125:
                return exc(3)
            regs = self.model.registers()
            if any(a not in regs for a in range(addr, addr + count)):
                return exc(2)
            return bytes([3, 2 * count]) + b''.join(struct.pack('>H', regs[a]) for a in range(addr, addr + count))
        if fc == 6:
            addr, value = struct.unpack('>HH', pdu[1:5])
            return pdu[:5] if self.model.write(addr, [value]) else exc(2)
        if fc == 16:
            addr, count = struct.unpack('>HH', pdu[1:5])
            values = list(struct.unpack(f'>{count}H', pdu[6:6 + 2 * count]))
            return pdu[:5] if self.model.write(addr, values) else exc(2)
        return exc(1)


def recv(sock, n):
    data = b''
    while len(data) < n:
        chunk = sock.recv(n - len(data))
        if not chunk:
            return None
        data += chunk
    return data


def run_clock(model, verbose):
    last = time.monotonic()
    shown = 0.0
    while True:
        time.sleep(1)
        now = time.monotonic()
        with model.lock:
            model.step((now - last) * model.speed)
            v = model.v
            line = (f'{model.clock:%H:%M} PV {v["input_power"]:5d} W  AC {v["active_power"]:5d} W  '
                    f'battery {v["battery_power"]:+5d} W {v["battery_soc"]:5.1f} %  grid {v["meter_power"]:+6d} W  '
                    f'status 0x{v["status"]:04x}')
        last = now
        if verbose and now - shown >= 10:
            print(line, flush=True)
            shown = now


def serve(port, unit, model):
    """the server (port 0: any free port); serve_forever() runs it"""
    Handler.model, Handler.unit = model, unit
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    socketserver.ThreadingTCPServer.daemon_threads = True
    return socketserver.ThreadingTCPServer(('0.0.0.0', port), Handler)


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', type=int, default=1502)
    p.add_argument('--unit', type=int, default=1, help='unit id answered (SDongle and inverter: 1)')
    p.add_argument('--speed', type=float, default=1, help='simulated seconds per second (60: a day in 24 minutes)')
    p.add_argument('--start', metavar='HH:MM', help='simulated time of day to start at (default: now)')
    p.add_argument('--seed', type=int, help='random seed (repeatable data)')
    p.add_argument('-v', '--verbose', action='store_true', help='print the state every 10 s')
    a = p.parse_args()
    start = datetime.now().replace(microsecond=0)
    if a.start:
        h, m = map(int, a.start.split(':'))
        start = start.replace(hour=h, minute=m, second=0)
    model = Model(start, a.speed, random.Random(a.seed))
    threading.Thread(target=run_clock, args=(model, a.verbose), daemon=True).start()
    with serve(a.port, a.unit, model) as srv:
        print(f'Huawei SUN2000 simulator on port {a.port}, unit {a.unit}, speed {a.speed:g}x', flush=True)
        try:
            srv.serve_forever()
        except KeyboardInterrupt:
            pass
    return 0


if __name__ == '__main__':
    sys.exit(main())
