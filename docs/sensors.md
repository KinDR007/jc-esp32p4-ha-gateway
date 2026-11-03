# I2C sensors

The gateway reads environmental sensors on the board's I2C bus, recognises most of them by
itself and hands their values to Home Assistant (integration and MQTT). Web UI: System › Hardware.
API: [api.md](api.md#i2c-sensors). Code: `firmware/gateway/components/sensors`.

## Wiring

The bus (GPIO7 SDA, GPIO8 SCL, 100 kHz) also serves the ES8311 audio codec at 0x18 and the display
and camera connectors ([hardware.md](hardware.md#i2c-bus-es_i2c-gpio7-sda--gpio8-scl)).

| Connector | GND | 3.3 V | SCL | SDA |
|---|---|---|---|---|
| **CN3** (4 pins) | 1 | 2 | 3 | 4 |
| JP1 | 5 | 1 or 3 | 25 | 23 |

- 3.3 V sensors only. Breakout boards with a 5 V regulator and level shifter (most GY-xx boards)
  work from 3.3 V too.
- The board has 2.2 kΩ pull-ups; the pull-ups of breakout boards add to them, which is fine for a
  few boards. Keep the wires short (up to about 30 cm).
- Connect with the gateway off. A scan runs at every start and from the page.
- Check CN3 pin 2 with a multimeter (3.3 V against pin 1) before wiring: the pin numbers come
  from the schematic.

## Supported sensors

| Chip | Vendor | Measures | Addresses | Recognised by |
|---|---|---|---|---|
| BME280 | Bosch | temperature, humidity, pressure | 0x76, 0x77 | chip ID 0x60 (reg 0xD0) |
| BMP280 | Bosch | temperature, pressure | 0x76, 0x77 | chip ID 0x58 (0x56/0x57 samples) |
| BME680, BME688 | Bosch | temperature, humidity, pressure, gas resistance | 0x77, 0x76 | chip ID 0x61, variant reg 0xF0 |
| BMP388, BMP390 | Bosch | temperature, pressure | 0x77, 0x76 | chip ID 0x50 / 0x60 (reg 0x00) |
| BMP180 (BMP085) | Bosch | temperature, pressure | 0x77 | chip ID 0x55 |
| BMP581, BMP585 | Bosch | temperature, pressure | 0x47, 0x46 | chip ID 0x50 / 0x51 (reg 0x01) |
| SHT3x (SHT30/31/35) | Sensirion | temperature, humidity | 0x44, 0x45 | status register with CRC (0xF32D) |
| SHT4x (SHT40/41/45) | Sensirion | temperature, humidity | 0x44, 0x45, 0x46 | serial number with CRC (0x89) |
| AHT10/20/21/25/30, DHT20 | Aosong | temperature, humidity | 0x38, 0x39 | status byte only (likely) |
| Si7021, Si7013, HTU21D, SHT21 | Silicon Labs, TE, Sensirion | temperature, humidity | 0x40 | electronic ID (0xFC 0xC9), else user register (likely) |
| HDC1080 | TI | temperature, humidity | 0x40 | manufacturer 0x5449, device 0x1050 |
| SCD40, SCD41 | Sensirion | CO2, temperature, humidity | 0x62 | serial number with CRC |
| SGP30 | Sensirion | eCO2, TVOC | 0x58 | feature set (product type 0) |
| ENS160 | ScioSense | AQI 1-5, TVOC, eCO2 | 0x53, 0x52 | part ID 0x0160 |
| PMSA003I | Plantower | PM1, PM2.5, PM10 | 0x12 | frame header 0x42 0x4D and checksum |
| BH1750 (GY-302, GY-30) | ROHM | illuminance | 0x23, 0x5C | answers a read (likely) |
| VEML7700 (VEML6030) | Vishay | illuminance | 0x10 | ID 0x81 (reg 0x07); without it likely, unless a VEML6075 (ID 0x26 at 0x0C) |
| TSL2591 | ams | illuminance | 0x29 | ID 0x50 (reg 0x12) |
| TSL2561 | ams | illuminance | 0x39, 0x29, 0x49 | ID reg 0x0A upper nibble 0x5 / 0x1 |
| LTR390 | Lite-On | UV index, illuminance | 0x53 | part ID 0xB2 |
| OPT3001 | TI | illuminance | 0x44-0x47 | manufacturer 0x5449, device 0x3001 |
| MAX44009 (GY-49) | Maxim | illuminance | 0x4A, 0x4B | threshold registers at reset values (likely) |
| TMP117 | TI | temperature | 0x48-0x4B | device ID 0x117 |
| MCP9808 | Microchip | temperature | 0x19-0x1F | manufacturer 0x0054, device 0x04 |

"Likely": the chip has no ID register, it only answers the way the chip does. MCP9808 cannot use
0x18 (the codec). BME680/688: the gas resistance in Ω; the air quality index (IAQ) needs Bosch's
closed BSEC library and is not included. ENS160: warm-up 3 min, full accuracy after about an hour.
SGP30: read every second (its baseline needs it), 400 ppm / 0 ppb for the first 15 s.

### Not supported, labelled by the scan

DPS310 and SPL06 (0x76/0x77, register-compatible, cannot be told apart by their ID), MS5611 (no ID),
SCD30 (clock stretching beyond the bus timeout), SGP40/41 (needs Sensirion's VOC algorithm),
CCS811 (end of life, needs its nWAKE pin low), AM2320. Other devices the scan may meet are named by
address: OLED displays 0x3C/0x3D, port expanders 0x20-0x27 and 0x38-0x3F, current sensors and PWM
drivers 0x40-0x4F, ADCs 0x48-0x4B, EEPROMs 0x50-0x57, RTCs and IMUs 0x68/0x69, multiplexers
0x70-0x77, a display's touch controller (GT911, 0x14/0x5D) and backlight controller (0x45), cameras.

## Recognition

1. Every address 0x08-0x77 is probed (an address-only write).
2. For an address that answers and is not configured yet, every driver listing that address tries
   to identify the chip, read-only where the address is shared with a port expander (0x23, 0x38).
   The order avoids side effects: the OPT3001 register read comes before the Sensirion commands at
   0x44/0x45, the HDC1080 ID before the Si70xx commands at 0x40 (0xFE resets a Si70xx).
3. The sensor is added when exactly one type is sure, or when only one type answers at all. Its
   name is the chip (BME280; BMP280 also for boards sold as BME280 that carry a BMP280).
4. Otherwise the scan result lists the candidates (sure or likely) and the user picks one. A sensor
   at an address the scan cannot settle, or one without an answer yet, can be added by hand with
   any address its type allows.

Address conflicts and how they resolve:

| Address | Chips | Resolved by |
|---|---|---|
| 0x76/0x77 | BMP280, BME280, BME680/688, BMP388/390, BMP180, DPS310/SPL06, MS5611 | chip ID at 0xD0, then at 0x00 |
| 0x44/0x45 | SHT3x, SHT4x, OPT3001, display backlight (0x45) | OPT3001 IDs, SHT4x serial, SHT3x status, each with CRC |
| 0x40 | HDC1080, Si7021/HTU21D, INA219/INA226 | TI IDs (INA226 has 0x5449 too but device 0x2260), Si70xx serial |
| 0x29 | TSL2591, TSL2561, VL53L0X | ID registers |
| 0x53 | ENS160, LTR390 | part IDs |
| 0x38 | AHT2x, PCF8574A | AHT status byte (a PCF8574A reads 0xFF with its pins high) |
| 0x48-0x4B | TMP117, ADS1115, LM75, MAX44009 (0x4A/0x4B) | TMP117 ID; MAX44009 by its reset values |

## Runtime

- One task reads every enabled sensor at the configured interval (5-3600 s, default 30), drivers
  measure on demand (forced mode, single shot) so a value is fresh when read.
- Three failed reads in a row: the sensor is started again every 60 s, its values disappear
  (Home Assistant: unavailable). A value older than three periods is not reported.
- Up to 16 sensors; their state lives in PSRAM (7 kB), the task stack (4 kB) in internal RAM.
- The configuration is stored in NVS (key `i2c_sens`) and part of the settings backup.

## Home Assistant

Per sensor: **Expose** (default on) and a name (default the chip, up to 32 characters).

- Integration P4 Gateway (1.6.0): one sensor entity per exposed sensor and quantity
  (`sensor.<gateway>_<name>_<quantity>`), with the device class and unit, attributes chip and
  address; sensors added later appear without reloading.
- MQTT discovery group `sensors` (Home Assistant › MQTT): the same entities; unavailable when the
  gateway is offline or the sensor gives no value. The preset "Only what the integration lacks"
  leaves them out, since the integration has them.
- Telegram: `/sensors` lists the values.

## Verification

- `tests/unit/sensor_drv_test.c` (CI): every driver against simulated chips: identification at
  every shared address (exactly one sure candidate), byte order, CRC errors, conversions; the
  BMP280 and BMP180 compensation against the datasheet examples (25.08 °C / 1006.53 hPa,
  15.0 °C / 699.64 hPa).
- `tests/unit/sensor_xcheck/` (manual): BME680/BME688 and BMP388/BMP390, whose datasheets have no
  worked example, against the Adafruit CircuitPython libraries on the same register bytes:
  temperature, pressure, gas resistance and heater setting match. Adafruit composes the BME680
  humidity parameters H1/H2 with the nibbles of register 0xE2 swapped against the Bosch BME68x API,
  which the gateway follows; with equal nibbles the humidity matches.
- Hardware: TC47 (scan finds the codec, configuration, a missing sensor), TC46 (MQTT discovery),
  UI31. **Not yet tested with real sensors**: the conversions rest on the simulations above.

## Sources

Datasheets of the manufacturers: Bosch BST-BMP280-DS001, BST-BME280-DS002, BST-BME680-DS001,
BST-BMP388-DS001, BST-BMP180-DS000, BMP581 DS004; Sensirion SHT3x-DIS, SHT4x, SCD4x, SGP30;
TI HDC1080 (SNAS672), OPT3001 (SBOS681), TMP117 (SNOSD82); Silicon Labs Si7021-A20; ROHM BH1750FVI;
Vishay VEML7700 and its application note "Designing the VEML7700 into an application"; ams
TSL2561, TSL2591; Lite-On LTR-390UV-01; Maxim MAX44009; Microchip MCP9808 (DS25095); ScioSense
ENS160; Plantower PMSA003; Aosong AHT20. Reference code: Bosch BME68x SensorAPI (float
compensation), Adafruit CircuitPython BME680 and BMP3XX (cross-check), Adafruit TSL2591 (lux
coefficient 408). Board: schematic sheets 3, 6 and 8 of
[unofficial_guition_esp32p4_repo](https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo/tree/main/JC-ESP32P4-M3-Dev).
