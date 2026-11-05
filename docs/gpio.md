# GPIO on JP1

Free pins of header JP1 configured and controlled from System › GPIO (JP1), the API and Home Assistant
(device "P4 GPIO" through the integration or MQTT). Status: G1 and G2 done (pins, reservations, input,
output, PWM, counter, API, web UI, Telegram `/gpio`), G3 (Home Assistant through the integration
1.7.0 and MQTT) and G4 (UART-TCP bridge, second I2C bus, 1-Wire DS18B20, WS2812 strip); G5 (tests with a
jumper on the pins) open.

## Use

System › GPIO (JP1) draws the header: power pins, radio pins locked with their reason, free pins (green)
and configured ones (blue, with their value). A free pin opens its settings: name, mode and what the mode
offers; *Free the pin* turns it off. The list of configured pins switches outputs (and a 1 s pulse),
moves a PWM duty and clears counters. API `GET`/`PUT /api/gpio`, `POST /api/gpio/set`
([api.md](api.md#gpio-of-header-jp1)); changes also go out as MQTT topic `gpio` and over `/api/ws`.

| Mode | Settings | Value |
|---|---|---|
| input | pull, inverted, debounce 0-1000 ms (sampled every 10 ms) | level |
| output | pull, inverted, open drain, after a restart off/on/last | level (and `pin_level` read back on the pad); `pulse_ms` flips it for a time |
| pwm | frequency 1 Hz-1 MHz (resolution from 20 bits at 1 Hz to 6 bits at 1 MHz), inverted, after a restart | duty 0-100 % |
| counter | edges rising/falling/both, pull, inverted, glitch filter 0-10000 ns | count, rate per second, frequency |
| uart (two pins) | RX pin, 300-3000000 Bd, 7/8 data bits, parity, 1/2 stop bits, TCP port (default 6639) | client, bytes each way |
| i2c (two pins) | SCL pin, 100 or 400 kHz | the sensors on it: System › Hardware, bus 2 |
| onewire | read every 5-3600 s | temperature per DS18B20 (DS1822, DS18S20) by its ROM code |
| ws2812 | LEDs 1-512, colour order (GRB, RGB, BRG, RBG), after a restart | on, colour, brightness 0-255, effect (none, rainbow) |

At most 4 PWM pins (LEDC timers), 4 counters (PCNT units), 1 UART (UART4, the only one left), 1 I2C bus, 2
strips (RMT) and 2 1-Wire buses.

- **UART-TCP bridge**: a TCP server on the port; one client at a time, a new one replaces the previous
  (usually the same program reconnecting); bytes pass unchanged. 3.3 V levels: RS232 needs a converter,
  RS485 a transceiver (the board's own RS485 is System › RS485).
- **Second I2C bus**: internal pull-ups on; longer wires need 2.2-4.7 kΩ to 3.3 V. Sensors on it are
  scanned when the bus is set up and at every start, ids `i2c2_<address>` ([sensors.md](sensors.md)).
- **1-Wire**: bit-banged with the standard timings (open drain, external 4.7 kΩ pull-up to 3.3 V); a ROM
  search at start, every 10 readings and on *Search*; all devices convert at once (750 ms), each is read
  with a CRC check; 85.0 °C (the power-on value) counts as no reading.
- **WS2812**: 800 kHz from the RMT; one colour for the whole strip or a moving rainbow (a frame every
  50 ms). Data at 3.3 V: most strips need a level shifter (74AHCT125) or a first LED at a lower supply;
  the strip has its own 5 V supply with GND joined to the gateway's. An output's level is set before its driver
turns on; the last level or duty is saved 5 s after the last change. While radio B's pins are GPIO, radio B
is not started, tested, flashed or backed up (409); a role given to radio B turns them off at the next
start with a notification.

## Home Assistant

Per pin *integration*, *MQTT*, both or none (`expose`). Both show the device "<host> GPIO" connected via
the gateway:

| Mode | Entity | MQTT commands below the base topic |
|---|---|---|
| input | binary sensor | |
| output | switch | `gpio/<n>/set`: `ON`, `OFF`, `PULSE <ms>` |
| pwm | light, brightness = duty | `gpio/<n>/duty/set`: 0-100; `gpio/<n>/set`: `ON` (100 %), `OFF` |
| counter | sensors count (total increasing) and frequency (Hz) | |
| onewire | a temperature sensor per device, named after the pin and the end of its ROM code | |
| ws2812 | light with RGB colour, brightness and effects (MQTT: JSON schema, state topic `gpio/<n>/light`) | `gpio/<n>/light/set`: `{"state", "color": {r, g, b}, "brightness", "effect"}` |
| uart, i2c | none (the I2C sensors are entities of the gateway, [sensors.md](sensors.md)) | |

State: topic `gpio` (retained) `{"<n>": level or duty, "<n>": {"count", "rate", "frequency"}}`, published on
every change, a running counter at most every 5 s; the integration gets it over `/api/ws`. MQTT discovery
group `gpio` (a stored selection of all eight groups of v1.11-v1.12 gets it too).

## Pins

| JP1 | GPIO | State |
|---|---|---|
| 7 | GPIO1 | free; XTAL_32K_N of the chip, no crystal on the board (to verify on the board) |
| 9 | GPIO2 | free; touch channel |
| 11 | GPIO3 | free; touch channel |
| 14 | GPIO45 | free with a caveat: switches the SD card power on the board (the firmware does not use the SD card) |
| 13, 15, 17 | GPIO4, 5, 20 | radio A (UART1, BOOT): always reserved |
| 19, 21, 12, 10 | GPIO32, 33, 46, 47 | radio B (UART2, RST, BOOT): reserved while radio B has a role or is detected, free otherwise |
| 23, 25 | GPIO7, 8 | shared I2C bus (codec, sensors): not a GPIO, devices go on the bus |
| 1, 3, 18 / 2, 4 / 5, 6, 16 | | 3.3 V / 5 V / GND |
| 20, 22, 24, 26 | | the C6 side of the radio A jumpers |
| 8 | | not connected |

Free pins: 4 with radio B in use, 8 without. 3.3 V logic, not 5 V tolerant. None of them has an ADC
(the P4 has ADC only on GPIO16-23 and 49-54).

Free peripherals of the P4 (any pin through the GPIO matrix): UART4 (UART0 console, 1 radio A, 2 radio
B, 3 RS485), one HP I2C port, LEDC (8 PWM channels), PCNT, RMT, SPI2/3.

## Decisions (2026-11-05)

- Radio B pins are free while radio B has no role and nothing is detected on it; giving radio B a role
  turns their configuration off with a notice.
- Functions: digital input, output, PWM, pulse counter (count and frequency); over more pins or a
  peripheral: UART-TCP bridge (UART4), a second I2C bus for the sensors component, 1-Wire DS18B20
  (RMT), WS2812 LED strip (RMT).
- Output and PWM after a restart: chosen per pin (off, on, last state), default off.
- Hardware tests with a jumper JP1 9-11 (GPIO2-GPIO3) later; until then tests without measuring levels.

## Design

- Component `gpio_ctl`: JP1 table with the capabilities of each pin, reservations computed at run time
  (radio A, radio B, I2C, power); a reserved pin cannot be configured and the API says why. Configuration
  in NVS (one JSON blob), part of the settings export.
- Per pin: off (high impedance); input (pull-up/down, inverted, debounce, edge events); output
  (push-pull or open drain, inverted, pulse of a given length, state after restart); PWM (frequency,
  duty, resolution from the frequency); counter (PCNT, total and frequency over a window).
- Functions over pins: UART bridge (TX, RX, baud, framing, TCP port, one client like the Zigbee
  bridge); I2C bus 2 (SDA, SCL, speed; the sensors component scans and reads it like the first);
  1-Wire (search, DS18B20 per ROM id, interval); WS2812 (pin, LED count, colour order; colour,
  brightness, simple effects).
- API `/api/gpio` (configuration, state, writes), changes pushed over the WebSocket. Permission
  `system/hw` (read 1, control and settings 2).
- Web UI, System › Hardware: the 2×13 header drawn, pins coloured by state (power, reserved with lock
  and reason, free, configured), a pin opens its settings; live level, output switch, PWM slider.
- Home Assistant, device "P4 GPIO" (via the gateway), per pin integration, MQTT, both or none: input
  binary_sensor, output switch, PWM light (brightness) or number, counter sensor (total_increasing) and
  frequency, DS18B20 temperature sensors, WS2812 light with colour and effects; MQTT group `gpio` with
  command topics. Needs an integration release (1.7.0).
- Safety: pins are high impedance from power on until the configuration applies (about 1-2 s) and after
  a crash: a relay needs an external pull-down (documented in the UI). Current per pin within the P4
  limits; a warning in the UI.

## Phases (G1-G4 done 2026-11-05)

| Phase | Content | Done when |
|---|---|---|
| G1 | pin table and reservations, JP1 drawing read only, GPIO1 and GPIO45 checked on the board | UI shows the header with reasons; radio B role frees and takes its pins |
| G2 | input, output, PWM, counter; API, UI control, NVS, state after restart | hw test without jumper, UI test |
| G3 | P4 GPIO device in the integration and MQTT (group `gpio`) | HA tests, integration 1.7.0 |
| G4 | UART-TCP bridge, I2C bus 2, 1-Wire DS18B20, WS2812 | each with its test |
| G5 | hw tests with the jumper JP1 9-11 (output to input, PWM to counter, UART loop), docs | TC with measured levels and frequency |

## Open

- GPIO1 and GPIO45 (2026-11-05): as inputs both follow the internal pull-up and pull-down, so nothing on
  the board holds them (no 32 kHz crystal on GPIO1, the SD power switch does not load GPIO45).
- Radio B's pins as GPIO is untested on hardware: it needs radio B without a role and without the C5.
- A UART loop test needs TX and RX on the jumpered pins.
