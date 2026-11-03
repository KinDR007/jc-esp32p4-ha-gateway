#!/usr/bin/env python3
"""Checks of the Huawei SUN2000 simulator and its template: python3 tests/modbus_tcp/test_sim.py"""
import json
import random
import socket
import struct
import sys
import threading
import unittest
from datetime import datetime
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import huawei_sun2000_sim as sim  # noqa: E402

TEMPLATE = json.loads((HERE / 'huawei_sun2000.json').read_text())
SIZE = {'u16': 1, 's16': 1, 'u32': 2, 's32': 2}


class Client:
    """minimal Modbus TCP master"""

    def __init__(self, port, unit=1):
        self.sock = socket.create_connection(('127.0.0.1', port), timeout=2)
        self.unit = unit
        self.tid = 0

    def request(self, pdu):
        self.tid += 1
        self.sock.sendall(struct.pack('>HHHB', self.tid, 0, len(pdu) + 1, self.unit) + pdu)
        tid, _, length, _ = struct.unpack('>HHHB', sim.recv(self.sock, 7))
        assert tid == self.tid
        return sim.recv(self.sock, length - 1)

    def read(self, addr, count):
        """registers, or the exception code (int)"""
        pdu = self.request(struct.pack('>BHH', 3, addr, count))
        if pdu[0] & 0x80:
            return pdu[1]
        return list(struct.unpack(f'>{count}H', pdu[2:]))

    def write(self, addr, value, fc=6):
        """None, or the exception code"""
        if fc == 6:
            pdu = self.request(struct.pack('>BHH', 6, addr, value))
        else:
            pdu = self.request(struct.pack('>BHHBH', 16, addr, 1, 2, value))
        return pdu[1] if pdu[0] & 0x80 else None


def decode(e, regs):
    if e['type'] == 'str':
        return b''.join(struct.pack('>H', r) for r in regs).rstrip(b'\0').decode()
    b = b''.join(struct.pack('>H', r) for r in regs)
    raw = int.from_bytes(b, 'big', signed=e['type'][0] == 's')
    return raw * e.get('scale', 1)


class SimTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.model = sim.Model(datetime(2026, 6, 21, 12, 30), 1, random.Random(1))
        cls.srv = sim.serve(0, 1, cls.model)
        threading.Thread(target=cls.srv.serve_forever, daemon=True).start()
        cls.port = cls.srv.server_address[1]

    @classmethod
    def tearDownClass(cls):
        cls.srv.shutdown()
        cls.srv.server_close()

    def setUp(self):
        self.c = Client(self.port)

    def tearDown(self):
        self.c.sock.close()

    def value(self, key):
        e = next(e for e in TEMPLATE['entities'] if e['key'] == key)
        regs = self.c.read(int(e['addr']), e.get('count') or SIZE[e['type']])
        self.assertIsInstance(regs, list, key)
        return decode(e, regs)

    def test_template_entities_readable(self):
        """every entity the gateway reads answers, decoded with the template's type and scale"""
        q = {addr: name for addr, _, _, name in sim.REGISTERS}
        for e in TEMPLATE['entities']:
            if e.get('write', {}).get('kind') == 'button':
                self.assertEqual(self.c.read(int(e['addr']), 1), 2, f'{e["key"]}: write only')
                continue
            got = self.value(e['key'])
            want = self.model.v[q[int(e['addr'])]]
            if isinstance(want, str):
                self.assertEqual(got, want, e['key'])
            else:
                self.assertAlmostEqual(got, want, delta=e.get('scale', 1), msg=e['key'])

    def test_plausible_midday(self):
        self.assertEqual(self.value('model_name'), 'SUN2000-10KTL-M1')
        self.assertGreater(self.value('input_power'), 1000)
        self.assertTrue(225 < self.value('voltage_l1') < 236)
        self.assertTrue(49.9 < self.value('frequency') < 50.1)
        self.assertIn(self.value('status'), (0x0200, 0x0201))
        balance = self.value('active_power') - self.value('grid_power')  # = household load
        self.assertTrue(250 <= balance <= 5500, balance)

    def test_exceptions(self):
        self.assertEqual(self.c.read(32020, 1), 2)       # PV3: not there
        self.assertEqual(self.c.read(32064, 30), 2)      # runs past 32090
        self.assertEqual(self.c.read(30000, 126), 3)     # too many registers
        self.assertEqual(self.c.write(32080, 1), 2)      # read only
        self.assertEqual(self.c.write(40125, 1500), 2)   # 150 %
        self.assertEqual(self.c.request(bytes([4, 0, 0, 0, 1]))[1], 1)  # input registers: no such function

    def test_other_unit_no_answer(self):
        c = Client(self.port, unit=2)
        c.sock.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            c.read(32080, 2)
        c.sock.close()

    def test_power_limit(self):
        self.assertIsNone(self.c.write(40125, 200, fc=16))  # 20 %
        try:
            self.assertAlmostEqual(self.value('power_limit'), 20)
            self.assertLessEqual(self.value('active_power'), 2000)
            self.assertEqual(self.value('status'), 0x0201)
        finally:
            self.assertIsNone(self.c.write(40125, 1000))

    def test_shutdown_startup(self):
        self.assertIsNone(self.c.write(40201, 1))
        try:
            self.assertEqual(self.value('status'), 0x0301)
            self.assertEqual(self.value('active_power'), 0)
            self.assertEqual(self.value('input_power'), 0)
        finally:
            self.assertIsNone(self.c.write(40200, 1))
        self.assertIn(self.value('status'), (0x0200, 0x0201))

    def test_day(self):
        """a whole day: energy counts up, night standby or battery, daily yield back to 0 after midnight"""
        m = sim.Model(datetime(2026, 6, 21, 0, 0), 1, random.Random(2))
        total, statuses = m.v['total_yield'], set()
        for _ in range(24 * 60 - 1):
            m.step(60)
            statuses.add(m.v['status'])
            self.assertTrue(5 <= m.v['battery_soc'] <= 100)
            self.assertLessEqual(m.v['active_power'], sim.RATED_W)
        self.assertGreater(m.v['daily_yield'], 20)
        self.assertAlmostEqual(m.v['total_yield'] - total, m.v['daily_yield'], places=6)
        self.assertIn(0x0200, statuses)
        m.step(120)
        self.assertLess(m.v['daily_yield'], 0.5)


if __name__ == '__main__':
    unittest.main(verbosity=2)
