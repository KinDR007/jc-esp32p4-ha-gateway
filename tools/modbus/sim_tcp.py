#!/usr/bin/env python3
"""Modbus TCP device simulator for the gateway's Modbus tests (no third party modules).

Each unit id plays one template (firmware/gateway/components/modbus/templates/<id>.json): every entity
gets a known value, encoded here independently of the gateway code (type, word/byte order, scale).
Addresses no entity covers answer exception 2 (like a strict device), unless --lenient. Writes
(functions 5, 6, 15, 16) change the registers and are logged; function 0x42 answers (PZEM reset).

  sim_tcp.py [--port 1502] [--lenient] eastron_sdm630:1 peacefair_pzem016:2 ...
  sim_tcp.py --udp [--port 8899] [--split] goodwe_xs:127   a GoodWe Wi-Fi module: Modbus RTU frames over UDP,
                                          answers with AA 55 (--split: long ones in two datagrams); discovery on 48899
  sim_tcp.py --expect eastron_sdm630      print the values the gateway must show, as JSON
  --log-reads                             also log every read (tests of the gateway's register cache)
  --delay MS                              answer every request after MS (a slow device, e.g. on RS485)

The value of an entity: a number derived from its key (stable), within the range its type can hold;
bool entities alternate; map entities take their first option."""
import argparse
import json
import socket
import socketserver
import struct
import sys
import threading
import time
import zlib
from pathlib import Path

TEMPLATES = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'
REGS = {'bool': 1, 'u16': 1, 's16': 1, 'u32': 2, 's32': 2, 'u64': 4, 's64': 4, 'f32': 2, 'f64': 4}


def num(v):
    return int(v, 16) if isinstance(v, str) and v.lower().startswith('0x') else float(v) if isinstance(v, str) else v


def raw_value(t, e):
    """The raw value of an entity: what is encoded in the registers."""
    h = zlib.crc32((t['id'] + e['key']).encode())
    typ = e.get('type', 'bool' if e['reg'] in ('coil', 'discrete') else 'u16')
    if 'bit' in e or typ == 'bool':
        return h & 1
    if 'map' in e:
        return num(next(iter(e['map'])))
    scale = e.get('scale', 1)
    prec = e.get('precision', 2 if typ in ('f32', 'f64') else 0)
    if typ in ('f32', 'f64'):
        # a value that survives float32 and the precision: few decimals
        value = round((h % 20000) / 10.0 + 1, max(0, min(prec, 1)))
        return value / scale if scale != 1 else value
    top = {'u16': 60000, 's16': 30000, 'u32': 4000000, 's32': 2000000, 'u64': 9000000, 's64': 9000000}[typ]
    raw = h % top + 1
    return -raw if typ.startswith('s') and h & 2 else raw


def encode(typ, order, value):
    """registers (ints) of value as they go over the wire"""
    n = REGS[typ]
    if typ == 'f32':
        b = struct.pack('>f', value)
    elif typ == 'f64':
        b = struct.pack('>d', value)
    else:
        v = int(round(value))
        b = v.to_bytes(2 * n, 'big', signed=typ.startswith('s'))
    words = [b[i:i + 2] for i in range(0, len(b), 2)]
    if order in ('CDAB', 'DCBA'):
        words.reverse()
    if order in ('BADC', 'DCBA'):
        words = [w[::-1] for w in words]
    return [int.from_bytes(w, 'big') for w in words]


def expected(t, e, raw):
    """what the gateway shows for the entity"""
    typ = e.get('type', 'bool' if e['reg'] in ('coil', 'discrete') else 'u16')
    if 'map' in e:
        return e['map'].get(str(int(raw)), raw)
    if 'bit' in e or typ == 'bool':
        return bool(raw)
    if typ == 'f32':
        raw = struct.unpack('>f', struct.pack('>f', raw))[0]
    v = raw * e.get('scale', 1) + e.get('offset', 0)
    prec = e.get('precision', 2 if typ in ('f32', 'f64') else 0)
    return round(v, prec)


def load(tid):
    return json.loads((TEMPLATES / f'{tid}.json').read_text())


class Unit:
    def __init__(self, tid, strict):
        self.t = load(tid)
        self.strict = strict
        self.space = {'coil': {}, 'discrete': {}, 'input': {}, 'holding': {}}
        self.expect = {}
        self.lock = threading.Lock()
        for e in self.t['entities']:
            if e['reg'] == 'none':
                continue
            addr = num(e['addr'])
            typ = e.get('type', 'bool' if e['reg'] in ('coil', 'discrete') else 'u16')
            raw = 0 if typ == 'str' else raw_value(self.t, e)
            sp = self.space[e['reg']]
            if e['reg'] in ('coil', 'discrete'):
                sp[addr] = raw
            elif 'bit' in e:
                sp[addr] = (sp.get(addr, 0) & ~(1 << e['bit'])) | (raw << e['bit'])
            elif typ == 'str':
                text = (e['key'].upper() + '-' + '0123456789' * 26)[:2 * e['count']]  # no spaces: a serial number
                for i in range(e['count']):
                    sp[addr + i] = ord(text[2 * i]) << 8 | ord(text[2 * i + 1])
                self.expect[e['key']] = text
                continue
            else:
                for i, w in enumerate(encode(typ, e.get('order', 'ABCD'), raw)):
                    sp[addr + i] = w
            self.expect[e['key']] = expected(self.t, e, raw)

    def read(self, space, addr, count):
        with self.lock:
            sp = self.space[space]
            if self.strict and any(a not in sp for a in range(addr, addr + count)):
                return None
            return [sp.get(a, 0) for a in range(addr, addr + count)]

    def write(self, space, addr, values):
        with self.lock:
            sp = self.space[space]
            if self.strict and any(a not in sp for a in range(addr, addr + len(values))):
                return False
            for i, v in enumerate(values):
                sp[addr + i] = v
            print(f'write unit {self.t["id"]} {space} 0x{addr:04x} = {values}', flush=True)
            return True


UNITS = {}
LOG_READS = False
DELAY_S = 0
SPACES = {1: 'coil', 2: 'discrete', 3: 'holding', 4: 'input'}


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        sock = self.request
        while True:
            head = self.recv(sock, 7)
            if not head:
                return
            tid, proto, length, unit = struct.unpack('>HHHB', head)
            pdu = self.recv(sock, length - 1)
            if pdu is None:
                return
            answer = self.answer(UNITS.get(unit), pdu)
            if DELAY_S:
                time.sleep(DELAY_S)
            if answer is None:
                continue  # no such unit: no answer, like a gateway with a dead target
            sock.sendall(struct.pack('>HHHB', tid, 0, len(answer) + 1, unit) + answer)

    @staticmethod
    def recv(sock, n):
        data = b''
        while len(data) < n:
            chunk = sock.recv(n - len(data))
            if not chunk:
                return None
            data += chunk
        return data

    @staticmethod
    def answer(u, pdu):
        if u is None:
            return None
        fc = pdu[0]
        exc = lambda code: bytes([fc | 0x80, code])
        if fc in SPACES:
            addr, count = struct.unpack('>HH', pdu[1:5])
            if LOG_READS:
                print(f'read unit {u.t["id"]} {SPACES[fc]} 0x{addr:04x} {count}', flush=True)
            regs = u.read(SPACES[fc], addr, count)
            if regs is None:
                return exc(2)
            if fc <= 2:
                bits = bytearray((count + 7) // 8)
                for i, v in enumerate(regs):
                    bits[i // 8] |= (1 if v else 0) << (i % 8)
                return bytes([fc, len(bits)]) + bytes(bits)
            return bytes([fc, 2 * count]) + b''.join(struct.pack('>H', r) for r in regs)
        if fc in (5, 6):
            addr, value = struct.unpack('>HH', pdu[1:5])
            ok = u.write('coil' if fc == 5 else 'holding', addr, [1 if (fc == 5 and value == 0xff00) else value if fc == 6 else 0])
            return pdu[:5] if ok else exc(2)
        if fc in (15, 16):
            addr, count = struct.unpack('>HH', pdu[1:5])
            if fc == 16:
                values = list(struct.unpack(f'>{count}H', pdu[6:6 + 2 * count]))
            else:
                values = [(pdu[6 + i // 8] >> (i % 8)) & 1 for i in range(count)]
            ok = u.write('holding' if fc == 16 else 'coil', addr, values)
            return pdu[:5] if ok else exc(2)
        if fc == 0x42:
            print(f'function 0x42 unit {u.t["id"]}', flush=True)
            return bytes([fc])
        return exc(1)


def crc16(data):
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return struct.pack('<H', c)


class UdpHandler(socketserver.BaseRequestHandler):
    """GoodWe: an RTU request (unit, PDU, CRC) in, 0xAA 0x55 + an RTU answer out."""
    split = False

    def handle(self):
        data, sock = self.request
        if len(data) < 4 or crc16(data[:-2]) != data[-2:]:
            return
        answer = Handler.answer(UNITS.get(data[0]), data[1:-2])
        if answer is None:
            return
        frame = bytes([data[0]]) + answer
        out = b'\xaa\x55' + frame + crc16(frame)
        if self.split and len(out) > 32:  # long answers only, as a module does
            sock.sendto(out[:5], self.client_address)
            sock.sendto(out[5:], self.client_address)
        else:
            sock.sendto(out, self.client_address)


class DiscoveryHandler(socketserver.BaseRequestHandler):
    def handle(self):
        data, sock = self.request
        if data == b'WIFIKIT-214028-READ':
            probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            probe.connect(self.client_address)  # the address the client reaches us on
            sock.sendto(f'{probe.getsockname()[0]},000000000000,Solar-WiFi-SIM'.encode(), self.client_address)
            probe.close()


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('--port', type=int, default=1502)
    p.add_argument('--lenient', action='store_true', help='unknown addresses read as 0 (no exception)')
    p.add_argument('--udp', action='store_true', help='GoodWe Modbus RTU over UDP (AA 55 answers)')
    p.add_argument('--split', action='store_true', help='--udp: answers over 32 bytes in two datagrams')
    p.add_argument('--expect', metavar='TEMPLATE')
    p.add_argument('--log-reads', action='store_true', help='log every read')
    p.add_argument('--delay', type=int, default=0, metavar='MS', help='answer every request after MS')
    p.add_argument('units', nargs='*', metavar='TEMPLATE:UNIT')
    a = p.parse_args()
    if a.expect:
        print(json.dumps(Unit(a.expect, True).expect, ensure_ascii=False))
        return 0
    global LOG_READS, DELAY_S
    LOG_READS = a.log_reads
    DELAY_S = a.delay / 1000
    for spec in a.units:
        tid, unit = spec.rsplit(':', 1)
        UNITS[int(unit)] = Unit(tid, not a.lenient)
    if a.udp:
        UdpHandler.split = a.split
        socketserver.UDPServer.allow_reuse_address = True
        disc = socketserver.UDPServer(('0.0.0.0', 48899), DiscoveryHandler)
        threading.Thread(target=disc.serve_forever, daemon=True).start()
        with socketserver.ThreadingUDPServer(('0.0.0.0', a.port), UdpHandler) as srv:
            print(f'GoodWe UDP simulator on port {a.port}: ' + ', '.join(f'{u}={x.t["id"]}' for u, x in sorted(UNITS.items())), flush=True)
            srv.serve_forever()
        return 0
    socketserver.ThreadingTCPServer.allow_reuse_address = True
    with socketserver.ThreadingTCPServer(('0.0.0.0', a.port), Handler) as srv:
        print(f'Modbus TCP simulator on port {a.port}: ' + ', '.join(f'{u}={x.t["id"]}' for u, x in sorted(UNITS.items())), flush=True)
        srv.serve_forever()


if __name__ == '__main__':
    sys.exit(main())
