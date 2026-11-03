#!/usr/bin/env python3
"""Built-in Eastron SDM templates (firmware/gateway/components/modbus/templates/eastron_*.json) from one register
table, so the models stay consistent. Sources and checks: docs/modbus.md. Usage: gen_sdm.py"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'

# key: (address, name, name_cs, unit, device class, state class, precision, every)
REG = {
    'voltage': (0x00, 'Voltage', 'Napětí', 'V', 'voltage', 'measurement', 1, 1),
    'voltage_l1': (0x00, 'Voltage L1', 'Napětí L1', 'V', 'voltage', 'measurement', 1, 1),
    'voltage_l2': (0x02, 'Voltage L2', 'Napětí L2', 'V', 'voltage', 'measurement', 1, 1),
    'voltage_l3': (0x04, 'Voltage L3', 'Napětí L3', 'V', 'voltage', 'measurement', 1, 1),
    'current': (0x06, 'Current', 'Proud', 'A', 'current', 'measurement', 2, 1),
    'current_l1': (0x06, 'Current L1', 'Proud L1', 'A', 'current', 'measurement', 2, 1),
    'current_l2': (0x08, 'Current L2', 'Proud L2', 'A', 'current', 'measurement', 2, 1),
    'current_l3': (0x0A, 'Current L3', 'Proud L3', 'A', 'current', 'measurement', 2, 1),
    'power': (0x0C, 'Power', 'Výkon', 'W', 'power', 'measurement', 0, 1),
    'power_l1': (0x0C, 'Power L1', 'Výkon L1', 'W', 'power', 'measurement', 0, 1),
    'power_l2': (0x0E, 'Power L2', 'Výkon L2', 'W', 'power', 'measurement', 0, 1),
    'power_l3': (0x10, 'Power L3', 'Výkon L3', 'W', 'power', 'measurement', 0, 1),
    'apparent_power': (0x12, 'Apparent power', 'Zdánlivý výkon', 'VA', 'apparent_power', 'measurement', 0, 1),
    'reactive_power': (0x18, 'Reactive power', 'Jalový výkon', 'var', 'reactive_power', 'measurement', 0, 1),
    'power_factor': (0x1E, 'Power factor', 'Účiník', None, 'power_factor', 'measurement', 2, 1),
    'power_factor_l1': (0x1E, 'Power factor L1', 'Účiník L1', None, 'power_factor', 'measurement', 2, 1),
    'power_factor_l2': (0x20, 'Power factor L2', 'Účiník L2', None, 'power_factor', 'measurement', 2, 1),
    'power_factor_l3': (0x22, 'Power factor L3', 'Účiník L3', None, 'power_factor', 'measurement', 2, 1),
    'phase_angle': (0x24, 'Phase angle', 'Fázový úhel', '°', None, 'measurement', 1, 1),
    'total_power': (0x34, 'Total power', 'Celkový výkon', 'W', 'power', 'measurement', 0, 1),
    'total_apparent_power': (0x38, 'Total apparent power', 'Celkový zdánlivý výkon', 'VA', 'apparent_power', 'measurement', 0, 1),
    'total_reactive_power': (0x3C, 'Total reactive power', 'Celkový jalový výkon', 'var', 'reactive_power', 'measurement', 0, 1),
    'total_power_factor': (0x3E, 'Total power factor', 'Celkový účiník', None, 'power_factor', 'measurement', 2, 1),
    'frequency': (0x46, 'Frequency', 'Frekvence', 'Hz', 'frequency', 'measurement', 2, 1),
    'import_energy': (0x48, 'Import energy', 'Odebraná energie', 'kWh', 'energy', 'total_increasing', 3, 1),
    'export_energy': (0x4A, 'Export energy', 'Dodaná energie', 'kWh', 'energy', 'total_increasing', 3, 1),
    'import_reactive_energy': (0x4C, 'Import reactive energy', 'Odebraná jalová energie', 'kvarh', 'reactive_energy', 'total_increasing', 3, 6),
    'export_reactive_energy': (0x4E, 'Export reactive energy', 'Dodaná jalová energie', 'kvarh', 'reactive_energy', 'total_increasing', 3, 6),
    'demand': (0x54, 'Power demand', 'Průměrný výkon', 'W', 'power', 'measurement', 0, 6),
    'max_demand': (0x56, 'Maximum power demand', 'Maximální průměrný výkon', 'W', 'power', 'measurement', 0, 6),
    'voltage_l1_l2': (0xC8, 'Voltage L1-L2', 'Napětí L1-L2', 'V', 'voltage', 'measurement', 1, 1),
    'voltage_l2_l3': (0xCA, 'Voltage L2-L3', 'Napětí L2-L3', 'V', 'voltage', 'measurement', 1, 1),
    'voltage_l3_l1': (0xCC, 'Voltage L3-L1', 'Napětí L3-L1', 'V', 'voltage', 'measurement', 1, 1),
    'neutral_current': (0xE0, 'Neutral current', 'Proud nulovým vodičem', 'A', 'current', 'measurement', 2, 1),
    'total_energy': (0x156, 'Total energy', 'Celková energie', 'kWh', 'energy', 'total_increasing', 3, 1),
    'total_reactive_energy': (0x158, 'Total reactive energy', 'Celková jalová energie', 'kvarh', 'reactive_energy', 'total_increasing', 3, 6),
    'import_energy_l1': (0x15A, 'Import energy L1', 'Odebraná energie L1', 'kWh', 'energy', 'total_increasing', 3, 6),
    'import_energy_l2': (0x15C, 'Import energy L2', 'Odebraná energie L2', 'kWh', 'energy', 'total_increasing', 3, 6),
    'import_energy_l3': (0x15E, 'Import energy L3', 'Odebraná energie L3', 'kWh', 'energy', 'total_increasing', 3, 6),
    'export_energy_l1': (0x160, 'Export energy L1', 'Dodaná energie L1', 'kWh', 'energy', 'total_increasing', 3, 6),
    'export_energy_l2': (0x162, 'Export energy L2', 'Dodaná energie L2', 'kWh', 'energy', 'total_increasing', 3, 6),
    'export_energy_l3': (0x164, 'Export energy L3', 'Dodaná energie L3', 'kWh', 'energy', 'total_increasing', 3, 6),
    'net_energy': (0x18C, 'Net energy', 'Čistá energie', 'kWh', 'energy', 'total', 3, 1),
    'import_power': (0x500, 'Import power', 'Odebíraný výkon', 'W', 'power', 'measurement', 0, 1),
    'export_power': (0x502, 'Export power', 'Dodávaný výkon', 'W', 'power', 'measurement', 0, 1),
}

SERIAL = {'key': 'serial_number', 'name': 'Serial number', 'name_cs': 'Výrobní číslo', 'reg': 'holding', 'addr': '0xfc00',
          'type': 'u32', 'precision': 0, 'category': 'diagnostic', 'every': 0}
RESET_DEMAND = {'key': 'reset_max_demand', 'name': 'Reset maximum demand', 'name_cs': 'Nulovat maximum', 'reg': 'holding',
                'addr': '0xf010', 'type': 'u16', 'category': 'config', 'write': {'kind': 'button', 'value': 0, 'fc': 16}}

SINGLE = ['voltage', 'current', 'power', 'apparent_power', 'reactive_power', 'power_factor', 'frequency', 'import_energy',
          'export_energy', 'import_reactive_energy', 'export_reactive_energy', 'total_energy', 'total_reactive_energy', 'demand', 'max_demand']
THREE = ['voltage_l1', 'voltage_l2', 'voltage_l3', 'current_l1', 'current_l2', 'current_l3', 'power_l1', 'power_l2', 'power_l3',
         'power_factor_l1', 'power_factor_l2', 'power_factor_l3', 'total_power', 'total_apparent_power', 'total_reactive_power',
         'total_power_factor', 'frequency', 'import_energy', 'export_energy', 'total_energy', 'voltage_l1_l2', 'voltage_l2_l3',
         'voltage_l3_l1', 'neutral_current']

MODELS = [
    ('eastron_sdm120', 'SDM120-Modbus', 2400, 40, SINGLE, [SERIAL],
     'Single phase. Default 2400 Bd, 8N1, address 1; the SDM120-M (the -M variant) ships with 9600 Bd. Address and baud rate on the meter\'s menu.',
     'https://www.eastroneurope.com/images/uploads/products/protocol/SDM120-MODBUS_Protocol.pdf'),
    ('eastron_sdm230', 'SDM230-Modbus', 9600, 40, SINGLE + ['phase_angle'], [RESET_DEMAND],
     'Single phase. Default 9600 Bd (manual v1.8; meters with manual v1.6 and older 2400 Bd), 8N1, address 1.',
     'https://www.eastroneurope.com/images/uploads/products/protocol/SDM230-MODBUS_Protocol.pdf'),
    ('eastron_sdm630', 'SDM630-Modbus V2', 9600, 80,
     THREE + ['import_reactive_energy', 'export_reactive_energy', 'total_reactive_energy', 'demand', 'max_demand',
              'import_energy_l1', 'import_energy_l2', 'import_energy_l3', 'export_energy_l1', 'export_energy_l2', 'export_energy_l3'],
     [SERIAL, RESET_DEMAND],
     'Three phase (3P4W). Default 9600 Bd, 8N1, address 1. Up to 80 registers per request.',
     'https://www.eastroneurope.com/images/uploads/products/protocol/SDM630_MODBUS_Protocol.pdf'),
    ('eastron_sdm72d_v2', 'SDM72D-M V2', 9600, 40, THREE + ['net_energy', 'import_power', 'export_power'], [SERIAL],
     'Three phase. Default 9600 Bd, 8N1, address 1. Reboots on requests in quick succession: 200 ms between frames.',
     'https://xn--stromzhler-v5a.eu/media/pdf/93/17/d7/SDM72DM-V2.pdf'),
    ('eastron_sdm72d', 'SDM72D-M (V1)', 9600, 40, ['total_power', 'import_energy', 'export_energy', 'total_energy', 'import_power', 'export_power'], [],
     'First version: only total power and energy (no voltages); the registers come from open source drivers (no Eastron map found).',
     'https://github.com/volkszaehler/mbmd/blob/master/meters/rs485/sdm72.go'),
]

for tid, model, baud, max_regs, keys, extra, notes, source in MODELS:
    ents = []
    for k in keys:
        addr, name, name_cs, unit, dc, sc, prec, every = REG[k]
        e = {'key': k, 'name': name, 'name_cs': name_cs, 'reg': 'input', 'addr': f'0x{addr:04x}', 'type': 'f32', 'precision': prec}
        if unit:
            e['unit'] = unit
        if dc:
            e['class'] = dc
        e['state'] = sc
        if every != 1:
            e['every'] = every
        ents.append(e)
    ents.sort(key=lambda e: int(e['addr'], 16))
    t = {'schema': 1, 'id': tid, 'manufacturer': 'Eastron', 'model': model, 'category': 'energy_meter', 'notes': notes, 'source': source,
         'serial': {'baud': baud, 'parity': 'N', 'stop': 1}, 'slave': 1, 'timeout_ms': 500,
         'gap_ms': 200 if 'sdm72' in tid else 50, 'poll_s': 10, 'max_regs': max_regs, 'max_gap': 20, 'entities': ents + extra}
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / f'{tid}.json').write_text(json.dumps(t, ensure_ascii=False, indent=1) + '\n')
    print(tid, len(t['entities']), 'entities')
