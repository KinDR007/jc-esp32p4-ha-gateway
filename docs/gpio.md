# GPIO on JP1 (plan)

Free pins of header JP1 configured and controlled from System › Hardware, the API and Home Assistant
(device "P4 GPIO" through the integration or MQTT). Status: planned 2026-11-05, not implemented.

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

## Phases

| Phase | Content | Done when |
|---|---|---|
| G1 | pin table and reservations, JP1 drawing read only, GPIO1 and GPIO45 checked on the board | UI shows the header with reasons; radio B role frees and takes its pins |
| G2 | input, output, PWM, counter; API, UI control, NVS, state after restart | hw test without jumper, UI test |
| G3 | P4 GPIO device in the integration and MQTT (group `gpio`) | HA tests, integration 1.7.0 |
| G4 | UART-TCP bridge, I2C bus 2, 1-Wire DS18B20, WS2812 | each with its test |
| G5 | hw tests with the jumper JP1 9-11 (output to input, PWM to counter, UART loop), docs | TC with measured levels and frequency |

## Open

- GPIO1: confirm on the board that nothing (32 kHz crystal) is on it.
- GPIO45: measure what the SD power switch loads the pin with.
- A UART loop test needs TX and RX on the jumpered pins.
