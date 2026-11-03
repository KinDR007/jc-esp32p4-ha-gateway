#!/usr/bin/env python3
"""Built-in templates of ORNO, Hiking, Chint and Peacefair meters (firmware/gateway/components/modbus/templates/).
Register facts and their sources: docs/modbus.md. Usage: gen_other.py"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'


def e(key, name, name_cs, reg, addr, typ, unit=None, dc=None, sc='measurement', prec=1, scale=None, order=None, every=None, **kw):
    x = {'key': key, 'name': name, 'name_cs': name_cs, 'reg': reg, 'addr': f'0x{addr:04x}', 'type': typ, 'precision': prec}
    if order:
        x['order'] = order
    if scale is not None:
        x['scale'] = scale
    if unit:
        x['unit'] = unit
    if dc:
        x['class'] = dc
    if sc:
        x['state'] = sc
    if every is not None:
        x['every'] = every
    x.update(kw)
    return x


def V(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'V', 'voltage', prec=1, **kw)
def A(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'A', 'current', prec=kw.pop('prec', 2), **kw)
def W(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'W', 'power', prec=0, **kw)
def VAR(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'var', 'reactive_power', prec=0, **kw)
def VA(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'VA', 'apparent_power', prec=0, **kw)
def PF(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, None, 'power_factor', prec=kw.pop('prec', 2), **kw)
def HZ(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, 'Hz', 'frequency', prec=kw.pop('prec', 2), **kw)
def KWH(key, name, cs, reg, addr, typ, **kw): return e(key, name, cs, reg, addr, typ, kw.pop('unit', 'kWh'), 'energy', 'total_increasing', prec=kw.pop('prec', 2), **kw)


T = []

# ORNO OR-WE-504: third party code only (no ORNO document reachable)
T.append({'id': 'orno_or_we_504', 'manufacturer': 'ORNO', 'model': 'OR-WE-504', 'category': 'energy_meter',
          'notes': 'Single phase. 9600 Bd 8N1, address 1. Registers from community code (no ORNO document available): check the values against the display.',
          'source': 'https://github.com/apichlinski/orno-reader-domoticz/blob/master/plugin.py',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 1}, 'max_regs': 16, 'entities': [
              V('voltage', 'Voltage', 'Napětí', 'holding', 0x00, 'u16', scale=0.1),
              A('current', 'Current', 'Proud', 'holding', 0x01, 'u16', scale=0.1, prec=1),
              HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x02, 'u16', scale=0.1, prec=1),
              W('power', 'Power', 'Výkon', 'holding', 0x03, 'u16'),
              VAR('reactive_power', 'Reactive power', 'Jalový výkon', 'holding', 0x04, 'u16'),
              VA('apparent_power', 'Apparent power', 'Zdánlivý výkon', 'holding', 0x05, 'u16'),
              PF('power_factor', 'Power factor', 'Účiník', 'holding', 0x06, 'u16', scale=0.001),
              KWH('energy', 'Energy', 'Energie', 'holding', 0x07, 'u32', scale=0.001, prec=3)]})

# ORNO OR-WE-514 / 515 (TapHome page reproducing the ORNO register PDF)
T.append({'id': 'orno_or_we_514', 'manufacturer': 'ORNO', 'model': 'OR-WE-514 / OR-WE-515', 'category': 'energy_meter',
          'notes': 'Single phase. 9600 Bd 8E1 (even parity), address 1. Tariff energies T1-T4 at 0xA002-0xA008.',
          'source': 'https://taphome.com/en/compatibility/orno-or-we-515/',
          'serial': {'baud': 9600, 'parity': 'E', 'stop': 1}, 'max_regs': 40, 'max_gap': 10, 'entities': [
              HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x130, 'u16', scale=0.01),
              V('voltage', 'Voltage', 'Napětí', 'holding', 0x131, 'u16', scale=0.01),
              A('current', 'Current', 'Proud', 'holding', 0x139, 'u32', scale=0.001, prec=3),
              W('power', 'Power', 'Výkon', 'holding', 0x140, 's32'),
              VAR('reactive_power', 'Reactive power', 'Jalový výkon', 'holding', 0x148, 's32'),
              VA('apparent_power', 'Apparent power', 'Zdánlivý výkon', 'holding', 0x150, 's32'),
              PF('power_factor', 'Power factor', 'Účiník', 'holding', 0x158, 's16', scale=0.001),
              KWH('energy', 'Energy', 'Energie', 'holding', 0xA000, 'u32', scale=0.01)]})

# ORNO OR-WE-516 / 517 (three phase)
three = []
for i, ph in enumerate(('L1', 'L2', 'L3')):
    three.append(V(f'voltage_{ph.lower()}', f'Voltage {ph}', f'Napětí {ph}', 'holding', 0x0E + 2 * i, 'f32'))
three.append(HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x14, 'f32'))
for i, ph in enumerate(('L1', 'L2', 'L3')):
    three.append(A(f'current_{ph.lower()}', f'Current {ph}', f'Proud {ph}', 'holding', 0x16 + 2 * i, 'f32'))
three.append(W('power', 'Total power', 'Celkový výkon', 'holding', 0x1C, 'f32', scale=1000))
for i, ph in enumerate(('L1', 'L2', 'L3')):
    three.append(W(f'power_{ph.lower()}', f'Power {ph}', f'Výkon {ph}', 'holding', 0x1E + 2 * i, 'f32', scale=1000))
three.append(PF('power_factor', 'Total power factor', 'Celkový účiník', 'holding', 0x34, 'f32'))
three.append(KWH('energy', 'Total energy', 'Celková energie', 'holding', 0x100, 'f32'))
T.append({'id': 'orno_or_we_516', 'manufacturer': 'ORNO', 'model': 'OR-WE-516 / OR-WE-517', 'category': 'energy_meter',
          'notes': 'Three phase. 9600 Bd 8E1 (even parity), address 1. Power is read in kW (shown in W); import and export energy registers are not confirmed and left out.',
          'source': 'https://github.com/volkszaehler/mbmd/blob/master/meters/rs485/orno3p.go',
          'serial': {'baud': 9600, 'parity': 'E', 'stop': 1}, 'max_regs': 60, 'max_gap': 10, 'entities': three})

# Hiking DDS238-2 ZN/S
T.append({'id': 'hiking_dds238_2', 'manufacturer': 'Hiking (TOMZN)', 'model': 'DDS238-2 ZN/S', 'category': 'energy_meter',
          'notes': 'Single phase. 9600 Bd 8N1, address 1. Writes only with function 16. The three phase DTS238-7 has another, unpublished map.',
          'source': 'https://github.com/fawno/Modbus/blob/master/DDS238-2%20ZN-S%20Modbus.md',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 1}, 'max_regs': 20, 'max_gap': 6, 'entities': [
              KWH('energy', 'Total energy', 'Celková energie', 'holding', 0x00, 'u32', scale=0.01),
              KWH('export_energy', 'Export energy', 'Dodaná energie', 'holding', 0x08, 'u32', scale=0.01),
              KWH('import_energy', 'Import energy', 'Odebraná energie', 'holding', 0x0A, 'u32', scale=0.01),
              V('voltage', 'Voltage', 'Napětí', 'holding', 0x0C, 'u16', scale=0.1),
              A('current', 'Current', 'Proud', 'holding', 0x0D, 'u16', scale=0.01),
              W('power', 'Power', 'Výkon', 'holding', 0x0E, 's16'),
              VAR('reactive_power', 'Reactive power', 'Jalový výkon', 'holding', 0x0F, 's16'),
              PF('power_factor', 'Power factor', 'Účiník', 'holding', 0x10, 'u16', scale=0.001),
              HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x11, 'u16', scale=0.01)]})

# Chint DTSU666 (manual table 10)
d = []
for i, ph in enumerate(('L1', 'L2', 'L3')):
    d.append(V(f'voltage_{ph.lower()}', f'Voltage {ph}', f'Napětí {ph}', 'holding', 0x2006 + 2 * i, 'f32', scale=0.1))
for i, ph in enumerate(('L1', 'L2', 'L3')):
    d.append(A(f'current_{ph.lower()}', f'Current {ph}', f'Proud {ph}', 'holding', 0x200C + 2 * i, 'f32', scale=0.001))
d.append(W('power', 'Total power', 'Celkový výkon', 'holding', 0x2012, 'f32', scale=0.1))
for i, ph in enumerate(('L1', 'L2', 'L3')):
    d.append(W(f'power_{ph.lower()}', f'Power {ph}', f'Výkon {ph}', 'holding', 0x2014 + 2 * i, 'f32', scale=0.1))
d.append(VAR('reactive_power', 'Total reactive power', 'Celkový jalový výkon', 'holding', 0x201A, 'f32', scale=0.1))
d.append(PF('power_factor', 'Total power factor', 'Celkový účiník', 'holding', 0x202A, 'f32', scale=0.001))
d.append(HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x2044, 'f32', scale=0.01))
d.append(KWH('import_energy', 'Import energy', 'Odebraná energie', 'holding', 0x101E, 'f32'))
d.append(KWH('export_energy', 'Export energy', 'Dodaná energie', 'holding', 0x1028, 'f32'))
T.append({'id': 'chint_dtsu666', 'manufacturer': 'Chint', 'model': 'DTSU666', 'category': 'energy_meter',
          'notes': 'Three phase. 9600 Bd 8N1, address 1. Values in raw units of the manual (0.1 V, 0.001 A, 0.1 W) scaled here. The DTSU666-H (Huawei) differs.',
          'source': 'https://www.chintglobal.com/content/dam/chint/global/product-center/instruments-meters/electricity-meter/din-rail-meter/dtsu666/manual/DTSU666%20DSSU666%20User%20Manual.pdf',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 1}, 'max_regs': 60, 'max_gap': 20, 'entities': d})

# Chint DDSU666
T.append({'id': 'chint_ddsu666', 'manufacturer': 'Chint', 'model': 'DDSU666', 'category': 'energy_meter',
          'notes': 'Single phase. 9600 Bd, address on the display (the manual shows 8N1 and mentions two stop bits: try 2 if no answer). Export energy not documented.',
          'source': 'https://github.com/lmatula/ha_chint_pm',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 1}, 'max_regs': 20, 'max_gap': 4, 'entities': [
              V('voltage', 'Voltage', 'Napětí', 'holding', 0x2000, 'f32'),
              A('current', 'Current', 'Proud', 'holding', 0x2002, 'f32', prec=3),
              W('power', 'Power', 'Výkon', 'holding', 0x2004, 'f32', scale=1000),
              VAR('reactive_power', 'Reactive power', 'Jalový výkon', 'holding', 0x2006, 'f32', scale=1000),
              PF('power_factor', 'Power factor', 'Účiník', 'holding', 0x200A, 'f32', prec=3),
              HZ('frequency', 'Frequency', 'Frekvence', 'holding', 0x200E, 'f32'),
              KWH('import_energy', 'Import energy', 'Odebraná energie', 'holding', 0x4000, 'f32')]})

# Peacefair PZEM-016 / PZEM-014 (AC)
T.append({'id': 'peacefair_pzem016', 'manufacturer': 'Peacefair', 'model': 'PZEM-016 / PZEM-014', 'category': 'energy_meter',
          'notes': 'AC module. 9600 Bd 8N1. Address 1-247 (holding 0x0002); a single module on the bus also answers 248 (0xF8).',
          'source': 'https://rcscomponents.kiev.ua/datasheets/pzem-014_16.pdf',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 1}, 'max_regs': 10, 'entities': [
              V('voltage', 'Voltage', 'Napětí', 'input', 0x00, 'u16', scale=0.1),
              A('current', 'Current', 'Proud', 'input', 0x01, 'u32', order='CDAB', scale=0.001, prec=3),
              e('power', 'Power', 'Výkon', 'input', 0x03, 'u32', 'W', 'power', prec=1, order='CDAB', scale=0.1),
              KWH('energy', 'Energy', 'Energie', 'input', 0x05, 'u32', order='CDAB', scale=0.001, prec=3),
              HZ('frequency', 'Frequency', 'Frekvence', 'input', 0x07, 'u16', scale=0.1, prec=1),
              PF('power_factor', 'Power factor', 'Účiník', 'input', 0x08, 'u16', scale=0.01),
              e('alarm', 'Power alarm', 'Výkonový alarm', 'input', 0x09, 'u16', sc=None, prec=0, map={'0': 'off', '65535': 'on'}),
              e('alarm_threshold', 'Power alarm threshold', 'Práh výkonového alarmu', 'holding', 0x01, 'u16', 'W', 'power', sc=None, prec=0,
                category='config', every=0, write={'kind': 'number', 'min': 1, 'max': 23000, 'step': 1}),
              {'key': 'reset_energy', 'name': 'Reset energy', 'name_cs': 'Nulovat energii', 'reg': 'none', 'category': 'config',
               'write': {'kind': 'button', 'fc': 0x42}}]})

# Peacefair PZEM-017 / PZEM-003 (DC)
T.append({'id': 'peacefair_pzem017', 'manufacturer': 'Peacefair', 'model': 'PZEM-017 / PZEM-003', 'category': 'energy_meter',
          'notes': 'DC module with external shunt. 9600 Bd 8N2 (two stop bits). Set the shunt (current range) to the one fitted.',
          'source': 'https://raw.githubusercontent.com/esphome/esphome/dev/esphome/components/pzemdc/pzemdc.cpp',
          'serial': {'baud': 9600, 'parity': 'N', 'stop': 2}, 'max_regs': 8, 'entities': [
              e('voltage', 'Voltage', 'Napětí', 'input', 0x00, 'u16', 'V', 'voltage', prec=2, scale=0.01),
              e('current', 'Current', 'Proud', 'input', 0x01, 'u16', 'A', 'current', prec=2, scale=0.01),
              e('power', 'Power', 'Výkon', 'input', 0x02, 'u32', 'W', 'power', prec=1, order='CDAB', scale=0.1),
              KWH('energy', 'Energy', 'Energie', 'input', 0x04, 'u32', order='CDAB', scale=0.001, prec=3),
              e('high_voltage_alarm', 'High voltage alarm', 'Alarm vysokého napětí', 'input', 0x06, 'u16', sc=None, prec=0, map={'0': 'off', '65535': 'on'}),
              e('low_voltage_alarm', 'Low voltage alarm', 'Alarm nízkého napětí', 'input', 0x07, 'u16', sc=None, prec=0, map={'0': 'off', '65535': 'on'}),
              e('high_voltage_threshold', 'High voltage threshold', 'Práh vysokého napětí', 'holding', 0x00, 'u16', 'V', 'voltage', sc=None, prec=2,
                scale=0.01, category='config', every=0, write={'kind': 'number', 'min': 5, 'max': 350, 'step': 0.01}),
              e('low_voltage_threshold', 'Low voltage threshold', 'Práh nízkého napětí', 'holding', 0x01, 'u16', 'V', 'voltage', sc=None, prec=2,
                scale=0.01, category='config', every=0, write={'kind': 'number', 'min': 1, 'max': 350, 'step': 0.01}),
              e('shunt', 'Shunt', 'Bočník', 'holding', 0x03, 'u16', sc=None, prec=0, category='config', every=0,
                map={'0': '100 A', '1': '50 A', '2': '200 A', '3': '300 A'}, write={'kind': 'select'}),
              {'key': 'reset_energy', 'name': 'Reset energy', 'name_cs': 'Nulovat energii', 'reg': 'none', 'category': 'config',
               'write': {'kind': 'button', 'fc': 0x42}}]})

NOTES_CS = {'orno_or_we_504': 'Jednofázový. 9600 Bd 8N1, adresa 1. Registry z komunitního kódu (dokument ORNO nedostupný): ověřte hodnoty podle displeje.', 'orno_or_we_514': 'Jednofázový. 9600 Bd 8E1 (sudá parita), adresa 1. Energie tarifů T1-T4 na 0xA002-0xA008.', 'orno_or_we_516': 'Třífázový. 9600 Bd 8E1 (sudá parita), adresa 1. Výkon se čte v kW (zobrazen ve W); registry odebrané a dodané energie nejsou potvrzené a chybí.', 'hiking_dds238_2': 'Jednofázový. 9600 Bd 8N1, adresa 1. Zápis jen funkcí 16. Třífázový DTS238-7 má jinou, nezveřejněnou mapu.', 'chint_dtsu666': 'Třífázový. 9600 Bd 8N1, adresa 1. Hodnoty v jednotkách návodu (0,1 V, 0,001 A, 0,1 W) zde přepočtené. DTSU666-H (Huawei) se liší.', 'chint_ddsu666': 'Jednofázový. 9600 Bd, adresa na displeji (návod uvádí 8N1 i dva stop bity: bez odpovědi zkuste 2). Dodaná energie není zdokumentovaná.', 'peacefair_pzem016': 'Modul AC. 9600 Bd 8N1. Adresa 1-247 (holding 0x0002); jediný modul na sběrnici odpovídá i na 248 (0xF8).', 'peacefair_pzem017': 'Modul DC s externím bočníkem. 9600 Bd 8N2 (dva stop bity). Nastavte bočník (rozsah proudu) podle osazeného.'}

for t in T:
    t = {'schema': 1, **t, 'notes_cs': NOTES_CS[t['id']]}
    t.setdefault('slave', 1)
    t.setdefault('timeout_ms', 500)
    t.setdefault('gap_ms', 50)
    t.setdefault('poll_s', 10)
    (OUT / f"{t['id']}.json").write_text(json.dumps(t, ensure_ascii=False, indent=1) + '\n')
    print(t['id'], len(t['entities']), 'entities')
