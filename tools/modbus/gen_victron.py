#!/usr/bin/env python3
"""Built-in templates of a Victron GX device over Modbus TCP (firmware/gateway/components/modbus/templates/).
Registers from Victron's CCGX Modbus TCP register list 3.81 (sheet "Field list"), checked against a MultiPlus-II GX
(Venus OS): docs/modbus.md. Single phase (L1) only: a three phase system takes a copy with the L2/L3 addresses.
Usage: gen_victron.py"""
import json
from pathlib import Path

OUT = Path(__file__).resolve().parents[2] / 'firmware/gateway/components/modbus/templates'
SOURCE = 'https://www.victronenergy.com/upload/documents/CCGX-Modbus-TCP-register-list-3.81.xlsx'
ALARM = {'0': 'ok', '1': 'warning', '2': 'alarm'}


def e(key, name, name_cs, addr, typ, unit=None, dc=None, sc='measurement', prec=0, scale=None, **kw):
    """A holding register; u16 0xffff and s16 0x7fff mean "no value" on the GX."""
    x = {'key': key, 'name': name, 'name_cs': name_cs, 'reg': 'holding', 'addr': addr, 'type': typ, 'precision': prec}
    if scale is not None:
        x['scale'] = scale
    if unit:
        x['unit'] = unit
    if dc:
        x['class'] = dc
    if sc:
        x['state'] = sc
    if typ in ('u16', 's16') and 'map' not in kw:
        x['nan'] = 65535 if typ == 'u16' else 32767
    x.update(kw)
    return x


def V(key, name, cs, addr, typ='u16', scale=0.1, prec=1): return e(key, name, cs, addr, typ, 'V', 'voltage', prec=prec, scale=scale)
def A(key, name, cs, addr, typ='s16'): return e(key, name, cs, addr, typ, 'A', 'current', prec=1, scale=0.1)
def W(key, name, cs, addr, typ='s16', scale=None): return e(key, name, cs, addr, typ, 'W', 'power', scale=scale)
def HZ(key, name, cs, addr): return e(key, name, cs, addr, 's16', 'Hz', 'frequency', prec=2, scale=0.01)
def ENUM(key, name, cs, addr, opts, **kw): return e(key, name, cs, addr, 'u16', sc=None, map=opts, **kw)


T = []

T.append({'id': 'victron_gx_system', 'manufacturer': 'Victron Energy', 'model': 'GX system (Venus OS)', 'category': 'inverter',
          'notes': 'Modbus TCP of a GX device (Cerbo, MultiPlus-II GX, Venus OS): enable it under Settings › Integrations. Unit 100. Single phase (L1).',
          'notes_cs': 'Modbus TCP zařízení GX (Cerbo, MultiPlus-II GX, Venus OS): zapněte v Nastavení › Integrace. Jednotka 100. Jedna fáze (L1).',
          'source': SOURCE, 'slave': 100, 'poll_s': 5, 'max_regs': 30, 'max_gap': 10, 'entities': [
              W('consumption', 'AC consumption', 'Spotřeba AC', 817, 'u16'),
              W('grid', 'Grid power', 'Výkon ze sítě', 820),
              W('genset', 'Generator power', 'Výkon z generátoru', 823),
              W('pv_ac_output', 'PV on AC output', 'FV na výstupu AC', 808, 'u16'),
              W('pv_ac_input', 'PV on AC input', 'FV na vstupu AC', 811, 'u16'),
              ENUM('active_input', 'Active input', 'Aktivní vstup', 826, {'0': 'unknown', '1': 'grid', '2': 'generator', '3': 'shore', '240': 'not connected'}),
              V('battery_voltage', 'Battery voltage', 'Napětí baterie', 840, prec=2),
              A('battery_current', 'Battery current', 'Proud baterie', 841),
              W('battery_power', 'Battery power', 'Výkon baterie', 842),
              e('battery_soc', 'Battery charge', 'Nabití baterie', 843, 'u16', '%', 'battery'),
              ENUM('battery_state', 'Battery state', 'Stav baterie', 844, {'0': 'idle', '1': 'charging', '2': 'discharging'}),
              e('consumed_ah', 'Consumed charge', 'Odebraný náboj', 845, 'u16', 'Ah', None, prec=1, scale=-0.1, category='diagnostic'),
              e('time_to_go', 'Time to go', 'Zbývající čas', 846, 'u16', 's', 'duration', scale=100, category='diagnostic'),
              W('pv_dc', 'PV DC power', 'Výkon FV (DC)', 850, 'u16'),
              W('dc_system', 'DC system power', 'Výkon DC systému', 860),
              e('serial', 'Serial number', 'Sériové číslo', 800, 'str', sc=None, count=6, category='diagnostic', every=0)]})

T.append({'id': 'victron_vebus', 'manufacturer': 'Victron Energy', 'model': 'MultiPlus / Quattro (VE.Bus)', 'category': 'inverter',
          'notes': 'VE.Bus inverter/charger via a GX device (Modbus TCP). Unit 228 on MultiPlus-II GX, 227 Cerbo, 242 Venus GX, 246 CCGX: see the GX services list. Read only.',
          'notes_cs': 'Měnič/nabíječ VE.Bus přes zařízení GX (Modbus TCP). Jednotka 228 u MultiPlus-II GX, 227 Cerbo, 242 Venus GX, 246 CCGX: viz seznam služeb v GX. Jen čtení.',
          'source': SOURCE, 'slave': 228, 'poll_s': 5, 'max_regs': 40, 'max_gap': 10, 'entities': [
              V('ac_in_voltage', 'AC input voltage', 'Napětí vstupu AC', 3),
              A('ac_in_current', 'AC input current', 'Proud vstupu AC', 6),
              HZ('ac_in_frequency', 'AC input frequency', 'Frekvence vstupu AC', 9),
              W('ac_in_power', 'AC input power', 'Výkon vstupu AC', 12, scale=10),
              V('ac_out_voltage', 'AC output voltage', 'Napětí výstupu AC', 15),
              A('ac_out_current', 'AC output current', 'Proud výstupu AC', 18),
              HZ('ac_out_frequency', 'AC output frequency', 'Frekvence výstupu AC', 21),
              W('ac_out_power', 'AC output power', 'Výkon výstupu AC', 23, scale=10),
              e('input_current_limit', 'Input current limit', 'Omezení proudu vstupu', 22, 's16', 'A', 'current', None, prec=1, scale=0.1, category='diagnostic'),
              V('battery_voltage', 'Battery voltage', 'Napětí baterie', 26, scale=0.01, prec=2),
              A('battery_current', 'Battery current', 'Proud baterie', 27),
              e('battery_temperature', 'Battery temperature', 'Teplota baterie', 61, 's16', '°C', 'temperature', prec=1, scale=0.1),
              e('soc', 'State of charge', 'Nabití', 30, 'u16', '%', 'battery', prec=1, scale=0.1),
              ENUM('active_input', 'Active input', 'Aktivní vstup', 29, {'0': 'AC input 1', '1': 'AC input 2', '240': 'disconnected'}),
              ENUM('state', 'State', 'Stav', 31, {'0': 'off', '1': 'low power', '2': 'fault', '3': 'bulk', '4': 'absorption', '5': 'float',
                                                  '6': 'storage', '7': 'equalize', '8': 'passthru', '9': 'inverting', '10': 'power assist',
                                                  '11': 'power supply', '244': 'sustain', '252': 'external control'}),
              ENUM('mode', 'Switch position', 'Poloha přepínače', 33, {'1': 'charger only', '2': 'inverter only', '3': 'on', '4': 'off'}),
              ENUM('error', 'VE.Bus error', 'Chyba VE.Bus', 32, {'0': 'none', **{str(i): f'error {i}' for i in (1, 2, 3, 4, 5, 6, 7, 10, 14, 16, 17, 18, 22, 24, 25, 26)}},
                   category='diagnostic'),
              ENUM('alarm_temperature', 'Temperature alarm', 'Alarm teploty', 34, ALARM, category='diagnostic'),
              ENUM('alarm_low_battery', 'Low battery alarm', 'Alarm slabé baterie', 35, ALARM, category='diagnostic'),
              ENUM('alarm_overload', 'Overload alarm', 'Alarm přetížení', 36, ALARM, category='diagnostic'),
              ENUM('alarm_grid_lost', 'Grid lost alarm', 'Alarm výpadku sítě', 64, {'0': 'ok', '2': 'alarm'}, category='diagnostic')]})

for t in T:
    t = {'schema': 1, **t}
    t.setdefault('timeout_ms', 1000)
    t.setdefault('gap_ms', 0)
    (OUT / f"{t['id']}.json").write_text(json.dumps(t, ensure_ascii=False, indent=1) + '\n')
    print(t['id'], len(t['entities']), 'entities')
