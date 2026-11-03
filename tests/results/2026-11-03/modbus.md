# Modbus, 2026-11-03

Firmware v1.10.1-9 + Modbus (dirty build), board 192.168.88.243. No RS485 device connected; Modbus TCP
against tools/modbus/sim_tcp.py on the Mac (192.168.88.237:1502).

| Test | Result | Notes |
|---|---|---|
| tests/unit/modbus_test.c | PASS | protocol vectors, word orders, template checks, planning, 13 built-in templates |
| TC48 (with --mqtt-broker) | PASS | first runs failed on: template PUT answer (fixed in firmware), two test bugs (MQTT wildcard filter, device lookup by unit) |
| tests/ha_integration | PASS | 26 tests, new: test_modbus_device, test_firmware_without_modbus |
| UI32 | PASS | also exercised RS485 (UART3) without a device: "no answer" |
| UI24 | PASS | group list with modbus |
| ci_checks | PASS | with ESP-IDF's cJSON; the download branch for GitHub CI (no IDF) not run locally |

Values: all entities of SDM630 (37), PZEM-016 (8), DTSU666 (15), PZEM-017 (9) equal the simulator's
independent encoding. Writes: number, select, switch path, button with function 0x42, MQTT command.
Internal heap free after boot: 203 kB (was 219 kB before I2C and Modbus).
