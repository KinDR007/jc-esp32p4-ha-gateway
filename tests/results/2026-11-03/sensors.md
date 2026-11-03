# I2C sensors, 2026-11-03

Firmware v1.10.1-3 + I2C sensors (dirty build), board 192.168.88.243, no external sensor connected
(the bus holds only the ES8311 codec at 0x18).

| Test | Result | Notes |
|---|---|---|
| tests/unit/sensor_drv_test.c | PASS | 24 drivers, simulated chips; BMP280/BMP180 datasheet examples |
| tests/unit/sensor_xcheck | PASS | BME680/688, BMP388/390 against Adafruit CircuitPython (temperature, pressure, gas, heater); humidity equal with equal 0xE2 nibbles (Adafruit composes H1/H2 differently from Bosch) |
| tests/ha_integration | PASS | 24 tests, new: test_i2c_sensors, test_firmware_without_sensors |
| TC47 | PASS | scan, validation, missing sensor error, microphone unaffected |
| TC46 | PASS | first run FAILED: discovery stopped after reconnect (deadlock MQTT client lock vs. component locks); fixed by publishing discovery from the publish task only |
| TC04, TC17, TC30 | PASS | |
| TC14 | FAIL as expected in a quiet room (level -68 dBFS under the -65 dBFS gate, inference skipped); PASS with the gate at -90 dBFS, gate restored |
| UI31, UI24, UI28, UI30 | PASS | UI24 updated for the sensors group |

Not tested: real sensors (conversions verified by simulation only).
