# Hardware: Guition JC-ESP32P4-M3-DEV

Module JC-ESP32P4-M3: ESP32-P4NRW32 (16 MB Boya flash, 32 MB PSRAM) and an ESP32-C6 co-processor.
Schematics: [schematics/](schematics/), source
[makergo1024/JC-ESP32P4-M3-DEV-v3.2](https://github.com/makergo1024/JC-ESP32P4-M3-DEV-v3.2/tree/main/5-Schematic).

## Chip revision

The tested board reports **ESP32-P4 v1.3** (engineering sample, `esptool flash-id`). Builds are set
for revision < 3.0:

- gateway: `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`, `CONFIG_ESP32P4_REV_MIN_1`, 360 MHz
  ([sdkconfig.defaults](../firmware/gateway/sdkconfig.defaults))
- ESPHome: `engineering_sample: true` ([ble-proxy-p4gw.yaml](../firmware/esphome/ble-proxy-p4gw.yaml))

A v3.x board needs those lines removed and a clean build.

## USB

Use the USB-C next to the Ethernet jack (CH340, P4 UART0, console and flashing). On macOS the
CH340 drops bytes when reading flash above 115200 Bd; `scripts/backup_flash.sh` reads in chunks
with retries. Writing at 460800 Bd is reliable (esptool verifies the hash). Opening the port
resets the board (DTR/RTS).

## Ethernet (IP101)

| Signal | P4 GPIO |
|---|---|
| MDC / MDIO | 31 / 52 |
| PHY reset | 51 |
| RMII clock (50 MHz from the PHY) | 50 |
| TXD0 / TXD1 / TX_EN | 34 / 35 / 49 |
| RXD0 / RXD1 / CRS_DV | 29 / 30 / 28 |
| PHY address | 1 |

## P4 ↔ C6

Inside the module only SDIO (P4 GPIO14–19) and the C6 reset (CHIP_PU = P4 GPIO54) are wired.
SDIO is unused since the ESPHome role runs on the radio itself (v1.8). The radio roles need a UART,
which exists only via header **JP1**, so three jumper wires are required for them and for flashing
the C6. A second radio (XIAO ESP32-C5) also goes on JP1: wiring and power in
[dual-radio.md](dual-radio.md#wiring-jp1--xiao-esp32-c5).

### JP1 pinout (2×13, odd pins in one row, even in the other)

Net labels in the schematic sit **above** the wire they name.

```
 1  VCC3V3            2  VCC5V
 3  VCC3V3            4  VCC5V
 5  GND               6  GND
 7  GPIO1             8  -
 9  GPIO2            10  GPIO47
11  GPIO3            12  GPIO46
13  GPIO4   (A)      14  GPIO45
15  GPIO5   (B)      16  GND
17  GPIO20  (C)      18  VCC3V3   <- power, keep clear
19  GPIO32           20  C6_U0RXD (A)
21  GPIO33           22  C6_U0TXD (B)
23  ES_I2C_SDA       24  C6_IO9   (C)
25  ES_I2C_SCL       26  C6_CHIP_PU
```

### Jumpers (female-female dupont)

| From | To | Purpose |
|---|---|---|
| 13 (P4 GPIO4, TX) | 20 (C6 U0RXD) | UART P4 → C6 (Spinel, ZBOSS NCP, ROM loader) |
| 15 (P4 GPIO5, RX) | 22 (C6 U0TXD) | UART C6 → P4 |
| 17 (P4 GPIO20) | 24 (C6 IO9 / BOOT, 5k1 pull-up) | P4 puts the C6 into its ROM bootloader |

Pin 26 stays unconnected: the P4 drives C6 CHIP_PU through GPIO54 inside the module. Check pin 1
with a multimeter (3.3 V against GND, pin 2 shows 5 V) before wiring. The working wiring is
confirmed by TC03 (full C6 flash read over JP1).

## C6 telemetry

Every C6 firmware of a gateway role reports temperature, free RAM, uptime and reset reason
(`firmware/c6/common/p4gw_telemetry`, refreshed by its own task every 10 s). Delivery depends on
the link the role already uses, without changing what the host sees:

| Role | C6 → P4 | Notes |
|---|---|---|
| Thread (RCP) | spinel vendor property `0x3cf0`, read by the P4 every 30 s | needs the openthread patch on both sides (`firmware/patches/idf_openthread_telemetry.patch`, applied to a copy of the ESP-IDF component); an older RCP answers "not found". A request that does not finish in 3 s (RCP fails on it) turns telemetry off for that RCP image and restarts the P4. |
| Zigbee coordinator (ZBOSS NCP) | ZBOSS low-level frame of type `0x7e` every 30 s | the P4 bridge removes these frames before the stream reaches Zigbee2MQTT (`role_zigbee/zb_filter.c`, host unit test `tests/unit/zb_filter_test.c`) |
| Zigbee router | `@ZT {json}` line every 30 s | own line protocol |
| ESPHome Bluetooth proxy | none | the radio runs on its own over Wi-Fi; the gateway reads its Improv state at the start of the role |

The RCP links newlib nano printf: telemetry code must not use `%llu` or float conversions (that
crashed the first RCP builds). The router console runs at the link rate (460800 Bd), so C6 panics
in that role appear in the P4 log as `c6:` lines.

## Network surroundings scan

System › Network surroundings rates the 2.4 GHz band for placing a new Thread, Zigbee or Wi-Fi
network.

- **Quick** (Thread roles, no outage): OpenThread energy scan, active scan (beacons, also Zigbee
  PANs) and Thread discovery on channels 11–26, about 13 s with the radio off its channel.
- **Full** (any role, about a minute without the role): the gateway restarts, flashes the
  `scanner` image (`firmware/c6/scanner`) to the C6, which scans Wi-Fi (two passes) and every
  802.15.4 channel (energy, beacon request with Zigbee/Thread payload parsing, PANs in traffic),
  then flashes the role image back; Zigbee network data is kept by the usual snapshot.

The last result is stored in NVS with its time (SNTP). Ratings are computed in the web UI: Wi-Fi
overlap (a 20 MHz channel covers about four 802.15.4 channels), measured noise and networks
already on the channel. The C6 bundle with the scanner uses 2.04 of 2.10 MB of the `c6fw`
partition: another C6 image needs a bigger partition or a smaller image.

## Radio limits

The C6 has one 2.4 GHz radio. BLE scanning and a Thread/Zigbee router cannot run together
([coexistence](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)),
hence one role at a time. The module has a PCB antenna and an IPEX connector.
