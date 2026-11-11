#!/usr/bin/env python3
"""Built-in templates of ABB EQ B-series meters B21 (single phase) and B23 / B24 (three phase)
(firmware/gateway/components/modbus/templates/). One map for all three (B21 and B23/B24 user manuals,
chapter 9), checked against mbmd, Victron dbus-modbus-client and openWB: docs/modbus.md. Function 3 only,
big endian; an absent quantity reads all ones (unsigned) or the largest positive value (signed). B24 values
are primary values (the meter applies the CT ratio). Usage: gen_abb.py"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'
NAN = {'u16': 65535, 's16': 32767, 'u32': 4294967295, 's32': 2147483647, 'u64': 18446744073709551615}


def e(key, name, name_cs, addr, typ, unit=None, dc=None, sc='measurement', prec=0, scale=None, **kw):
    x = {'key': key, 'name': name, 'name_cs': name_cs, 'reg': 'holding', 'addr': f'0x{addr:04x}', 'type': typ, 'precision': prec}
    if scale is not None:
        x['scale'] = scale
    if unit:
        x['unit'] = unit
    if dc:
        x['class'] = dc
    if sc:
        x['state'] = sc
    if typ in NAN:
        x['nan'] = NAN[typ]
    x.update(kw)
    return x


def V(key, name, cs, addr): return e(key, name, cs, addr, 'u32', 'V', 'voltage', prec=1, scale=0.1)
def A(key, name, cs, addr): return e(key, name, cs, addr, 'u32', 'A', 'current', prec=2, scale=0.01)
def W(key, name, cs, addr): return e(key, name, cs, addr, 's32', 'W', 'power', scale=0.01)
def PF(key, name, cs, addr): return e(key, name, cs, addr, 's16', None, 'power_factor', prec=3, scale=0.001)
def KWH(key, name, cs, addr): return e(key, name, cs, addr, 'u64', 'kWh', 'energy', 'total_increasing', prec=2, scale=0.01)
def KVARH(key, name, cs, addr): return e(key, name, cs, addr, 'u64', 'kvarh', 'reactive_energy', 'total_increasing', prec=2, scale=0.01)


def common_tail(model):
    return [
        e('reactive_power', 'Total reactive power', 'Celkový jalový výkon', 0x5B1C, 's32', 'var', 'reactive_power', scale=0.01),
        e('apparent_power', 'Total apparent power', 'Celkový zdánlivý výkon', 0x5B24, 's32', 'VA', 'apparent_power', scale=0.01),
        e('frequency', 'Frequency', 'Frekvence', 0x5B2C, 'u16', 'Hz', 'frequency', prec=2, scale=0.01),
        PF('power_factor', 'Total power factor', 'Celkový účiník', 0x5B3A),
        KWH('import_energy', 'Import energy', 'Odebraná energie', 0x5000),
        KWH('export_energy', 'Export energy', 'Dodaná energie', 0x5004),
        KVARH('import_reactive_energy', 'Import reactive energy', 'Odebraná jalová energie', 0x500C),
        KVARH('export_reactive_energy', 'Export reactive energy', 'Dodaná jalová energie', 0x5010),
        e('serial', 'Serial number', 'Sériové číslo', 0x8900, 'u32', sc=None, category='diagnostic', every=0),
        e('firmware', 'Firmware', 'Firmware', 0x8908, 'str', sc=None, count=8, category='diagnostic', every=0),
        e('type', 'Type', 'Typ', 0x8960, 'str', sc=None, count=6, category='diagnostic', every=0, emulate=model),
    ]


SERIAL = {'baud': 19200, 'parity': 'E', 'stop': 1}
SOURCE = 'https://library.e.abb.com/public/216665241d8a4d17b50906705d5a9207/2CMC485003M0201_en_B23_B24_User_Manual.pdf'
T = []

b21 = [V('voltage', 'Voltage', 'Napětí', 0x5B00), A('current', 'Current', 'Proud', 0x5B0C),
       W('power', 'Power', 'Výkon', 0x5B14)] + common_tail('B21 112-100')
T.append({'id': 'abb_b21', 'manufacturer': 'ABB', 'model': 'B21 (EQ meter)', 'category': 'energy_meter',
          'notes': 'Single phase. Usually 19200 Bd 8E1, address 1 (not in the ABB manual): check the meter menu. '
                   'Steel variants do not measure export.',
          'notes_cs': 'Jednofázový. Obvykle 19200 Bd 8E1, adresa 1 (návod ABB to neuvádí): ověřte v menu elektroměru. '
                      'Varianty Steel neměří dodávku.',
          'source': 'https://library.e.abb.com/public/7937d0050237478eb6a2b28432c626cc/2CMC485004M0201_C_en_B21_User_Manual.pdf',
          'entities': b21})

b23 = [V(f'voltage_l{i + 1}', f'Voltage L{i + 1}', f'Napětí L{i + 1}', 0x5B00 + 2 * i) for i in range(3)]
b23 += [A(f'current_l{i + 1}', f'Current L{i + 1}', f'Proud L{i + 1}', 0x5B0C + 2 * i) for i in range(3)]
b23 += [W('power', 'Total power', 'Celkový výkon', 0x5B14)]
b23 += [W(f'power_l{i + 1}', f'Power L{i + 1}', f'Výkon L{i + 1}', 0x5B16 + 2 * i) for i in range(3)]
b23 += [PF(f'power_factor_l{i + 1}', f'Power factor L{i + 1}', f'Účiník L{i + 1}', 0x5B3B + i) for i in range(3)]
b23 += common_tail('B23 112-100')
b23 += [KWH(f'import_energy_l{i + 1}', f'Import energy L{i + 1}', f'Odebraná energie L{i + 1}', 0x5460 + 4 * i) for i in range(3)]
b23 += [KWH(f'export_energy_l{i + 1}', f'Export energy L{i + 1}', f'Dodaná energie L{i + 1}', 0x546C + 4 * i) for i in range(3)]
T.append({'id': 'abb_b23', 'manufacturer': 'ABB', 'model': 'B23 / B24 (EQ meter)', 'category': 'energy_meter',
          'notes': 'Three phase; B24 via CTs (primary values). Usually 19200 Bd 8E1, address 1 (not in the ABB manual): '
                   'check the meter menu. 3-wire: no L-N voltages, no L2.',
          'notes_cs': 'Třífázový; B24 přes MTP (primární hodnoty). Obvykle 19200 Bd 8E1, adresa 1 (návod ABB neuvádí): '
                      'ověřte v menu elektroměru. Zapojení bez N: chybí fázová napětí a L2.',
          'source': SOURCE, 'entities': b23})

for t in T:
    # the meter answers every read within 0x1000-0x8eff, gaps included, up to 125 registers; unused ones read
    # 0xffff (fill, when another meter plays this one); the type an inverter checks (0x8960) is fixed then
    t = {'schema': 1, **t, 'serial': SERIAL, 'slave': 1, 'timeout_ms': 500, 'gap_ms': 20, 'poll_s': 10,
         'max_regs': 125, 'max_gap': 40, 'fill': 65535}
    (OUT / f"{t['id']}.json").write_text(json.dumps(t, ensure_ascii=False, indent=1) + '\n')
    print(t['id'], len(t['entities']), 'entities')
