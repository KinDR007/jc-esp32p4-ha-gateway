#!/usr/bin/env python3
"""Built-in template of GoodWe single phase grid inverters (XS, MS, D-NS) (firmware/gateway/components/modbus/templates/).
Registers from the goodwe library (DT family, unit 0x7F), checked against a GW3000-XS whose Wi-Fi module speaks
Modbus RTU over UDP 8899 only (transport "udp"): docs/modbus.md. Usage: gen_goodwe.py"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'
SOURCE = 'https://github.com/marcelblijleven/goodwe/blob/master/goodwe/dt.py'
NAN = {'u16': 65535, 's16': 32767, 'u32': 4294967295}


def e(key, name, name_cs, addr, typ, unit=None, dc=None, sc='measurement', prec=0, scale=None, **kw):
    """A holding register; all ones means "no value" (the library reads it as 0 or None)."""
    x = {'key': key, 'name': name, 'name_cs': name_cs, 'reg': 'holding', 'addr': addr, 'type': typ, 'precision': prec}
    if scale is not None:
        x['scale'] = scale
    if unit:
        x['unit'] = unit
    if dc:
        x['class'] = dc
    if sc:
        x['state'] = sc
    if typ in NAN and 'map' not in kw:
        x['nan'] = NAN[typ]
    x.update(kw)
    return x


def V(key, name, cs, addr): return e(key, name, cs, addr, 'u16', 'V', 'voltage', prec=1, scale=0.1)
def A(key, name, cs, addr): return e(key, name, cs, addr, 'u16', 'A', 'current', prec=1, scale=0.1)
def W(key, name, cs, addr, typ): return e(key, name, cs, addr, typ, 'W', 'power')
def KWH(key, name, cs, addr, typ): return e(key, name, cs, addr, typ, 'kWh', 'energy', 'total_increasing', prec=1, scale=0.1)
def TEMP(key, name, cs, addr): return e(key, name, cs, addr, 's16', '°C', 'temperature', prec=1, scale=0.1)


T = [{'id': 'goodwe_xs', 'manufacturer': 'GoodWe', 'model': 'XS / MS / D-NS (single phase)', 'category': 'inverter',
      'notes': 'DT family, unit 127. A Wi-Fi module answers only on UDP 8899: connection GoodWe UDP. LAN or Kit-20 module: Modbus TCP 502. Offline at night.',
      'notes_cs': 'Řada DT, jednotka 127. Modul Wi-Fi odpovídá jen na UDP 8899: připojení GoodWe UDP. Modul LAN nebo Kit-20: Modbus TCP 502. V noci offline.',
      'source': SOURCE, 'slave': 127, 'poll_s': 10, 'max_regs': 50, 'max_gap': 10, 'timeout_ms': 2000, 'entities': [
          W('power', 'Power', 'Výkon', 30127, 'u32'),
          W('pv_power', 'PV power', 'Výkon FV', 30137, 's16'),
          V('pv1_voltage', 'PV1 voltage', 'Napětí FV1', 30103),
          A('pv1_current', 'PV1 current', 'Proud FV1', 30104),
          V('pv2_voltage', 'PV2 voltage', 'Napětí FV2', 30105),
          A('pv2_current', 'PV2 current', 'Proud FV2', 30106),
          V('grid_voltage', 'Grid voltage', 'Napětí sítě', 30118),
          A('grid_current', 'Grid current', 'Proud do sítě', 30121),
          e('grid_frequency', 'Grid frequency', 'Frekvence sítě', 30124, 'u16', 'Hz', 'frequency', prec=2, scale=0.01),
          KWH('energy_today', 'Energy today', 'Energie dnes', 30144, 'u16'),
          KWH('energy_total', 'Energy total', 'Energie celkem', 30145, 'u32'),
          TEMP('temperature', 'Temperature', 'Teplota', 30141),
          e('work_mode', 'Work mode', 'Režim', 30129, 'u16', sc=None, map={'0': 'wait', '1': 'normal', '2': 'error', '4': 'check'}),
          e('error_codes', 'Error codes', 'Kódy chyb', 30130, 'u32', sc=None, category='diagnostic'),
          e('warning_code', 'Warning code', 'Kód varování', 30132, 'u16', sc=None, category='diagnostic'),
          e('hours_total', 'Hours total', 'Hodiny provozu', 30147, 'u32', 'h', 'duration', 'total_increasing', category='diagnostic'),
          e('serial', 'Serial number', 'Sériové číslo', 30004, 'str', sc=None, count=8, category='diagnostic', every=0),
          e('model', 'Model', 'Model', 30012, 'str', sc=None, count=5, category='diagnostic', every=0)]}]

for t in T:
    t = {'schema': 1, **t}
    t.setdefault('gap_ms', 0)
    (OUT / f"{t['id']}.json").write_text(json.dumps(t, ensure_ascii=False, indent=1) + '\n')
    print(t['id'], len(t['entities']), 'entities')
