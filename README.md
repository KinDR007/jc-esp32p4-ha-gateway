# P4 gateway: Thread / Zigbee / BLE proxy on Guition JC-ESP32P4-M3-DEV

LAN gateway for Home Assistant on the JC-ESP32P4-M3-DEV board (ESP32-P4 + ESP32-C6 in the
module, Ethernet IP101). Each radio runs one role: radio A is the C6 of the board, radio B an optional
XIAO ESP32-C5 on the JP1 header ([docs/dual-radio.md](docs/dual-radio.md)), so Thread and Zigbee run
at the same time. Roles are switched from the web UI, the Home Assistant integration, MQTT, Telegram
or the REST API.

| Role (per radio) | P4 runs | The radio runs | Home Assistant side |
|---|---|---|---|
| Thread border router | OpenThread BR (esp-thread-br), REST on port 8081 | `ot_rcp` | OTBR + Thread integrations, Matter |
| Thread router | OpenThread FTD joined to an existing network | `ot_rcp` | nothing, extends the mesh |
| Zigbee coordinator | UART-TCP bridge, port 6638 | `zigbee_ncp` (ZBOSS NCP, esp-coordinator) | Zigbee2MQTT `adapter: zboss` |
| Zigbee router | monitor and commands for the radio | `zigbee_router` (this repo) | joins via permit join |
| ESPHome BLE proxy | Wi-Fi provisioning over Improv | ESPHome Bluetooth proxy, on its own over Wi-Fi | ESPHome integration (adopt) |

At most one Thread network, one Zigbee network and one ESPHome proxy at a time. Independent of the
roles: **sound recognition** with the onboard microphone,
YAMNet on the P4 (521 AudioSet classes, about 60 ms per 0.96 s of audio, off by default) and
sounds learned on the board (samples, tone or embedding profiles), see
[docs/sound-detection.md](docs/sound-detection.md).

Status: all roles and transitions pass the automated hardware tests (10/10 regression, see
[tests/results/2026-10-28/report.md](tests/results/2026-10-28/report.md)); the OTBR integration
runs in Home Assistant. Open manual tests are listed in [TODO.md](TODO.md).

## Home Assistant integration

[![Open your Home Assistant instance and open this repository inside HACS](https://my.home-assistant.io/badges/hacs_repository.svg)](https://my.home-assistant.io/redirect/hacs_repository/?owner=KinDR007&repository=jc-esp32p4-ha-gateway&category=integration)
[![Open your Home Assistant instance and start setting up the P4 Gateway integration](https://my.home-assistant.io/badges/config_flow_start.svg)](https://my.home-assistant.io/redirect/config_flow_start/?domain=p4gw)

Native integration **P4 Gateway** (HACS, no MQTT broker needed): see
[docs/home-assistant.md](docs/home-assistant.md#p4-gateway-integration-native-without-mqtt).

## Documentation

| Document | Content |
|---|---|
| [docs/hardware.md](docs/hardware.md) | board, chip revision, **JP1 jumpers**, pinout, USB notes |
| [docs/installation.md](docs/installation.md) | build, first installation, updates, role switching, recovery |
| [documentation/](documentation/index.html) | user documentation in Czech and English with screenshots of every page (HTML) |
| [docs/users.md](docs/users.md) | users, roles, permissions, 2FA, API tokens |
| [docs/notifications.md](docs/notifications.md) | notifications, Home Assistant, Telegram bot and its commands |
| [docs/home-assistant.md](docs/home-assistant.md) | Home Assistant setup per role, P4 Gateway integration, MQTT |
| [docs/zadani.md](docs/zadani.md) | original assignment and feasibility analysis (Czech) |
| [docs/sound-detection.md](docs/sound-detection.md) | sound event detection: research and proposal (Czech, pending) |
| [tests/TESTCASES.md](tests/TESTCASES.md) | test cases (automated and manual) |
| [tests/results/](tests/results/) | test runs, logs and reports |

## Quick start

```sh
./scripts/fetch_deps.sh
./scripts/build_all.sh                                                   # -> dist/
./scripts/backup_flash.sh /dev/cu.wchusbserial* esp32p4 backups/p4_backup.bin
./scripts/flash_p4.sh /dev/cu.wchusbserial*
```

Then open `http://p4gw-xxxxxx.local/`, back up the C6 (System tab), set a password and pick a role.

## Architecture

```
P4 flash (16 MB, firmware/partitions.csv)
  nvs       settings: role, Thread network, MQTT, password
  gw_a/gw_b gateway firmware A/B, OTA with rollback
  esphome   unused (was the ESPHome image of the P4 before v1.8)
  c6fw      Zigbee network snapshots of radio B (64 kB); radio firmwares are not kept in the P4
            flash: the packages (c6fw.bin, c5fw.bin) come from the release or an upload into PSRAM
  c6data    Zigbee network snapshots of radio A
  coredump
```

- Switching stores the role and restarts. On boot the gateway compares each radio's bundle with the
  one its role needs and updates the radio through its ROM bootloader. The Thread and Zigbee
  firmwares share one layout with a slot each, so a switch only selects the slot (8 kB, offline,
  [docs/c6-slots.md](docs/c6-slots.md)); ESPHome is a whole-flash bundle from the package.
- Each radio keeps its own networks: Zigbee data survives switching (snapshot per radio, restored
  on return; also weekly at night or on request), the Thread settings are kept per radio in P4 NVS.
- Stability: gateway images confirm themselves after 60 s (else rollback), repeated crashes stop
  role start but keep the UI, RCP failures trigger a limited number of C6 reflashes (a recovery
  restart counts as an unstable boot), core dumps are kept in flash and the last 16 KiB of log survive a restart (not a power cut). Sound recognition pauses while
  an upload or C6 flashing writes the P4 flash (both cores stall on every flash write).
- C6 telemetry (temperature, memory, uptime) reaches the web UI over each role's own link, see
  [docs/hardware.md](docs/hardware.md#c6-telemetry). The ESP-IDF openthread component is built from
  a patched copy (`firmware/patches/`, applied by `firmware/cmake/patched_openthread.cmake`).

## Repository layout

```
firmware/gateway/      P4 gateway (ESP-IDF): gw_core, c6_flasher, role_thread, role_zigbee, web UI
firmware/c6/           C6 builds: sdkconfig overlays, patches, shared telemetry, own zigbee_router firmware
firmware/patches/      patch of the ESP-IDF openthread component (C6 telemetry)
firmware/esphome/      ESPHome Bluetooth proxy of the radio role (radio_proxy)
firmware/partitions.csv
scripts/               env, fetch, build, pack, flash, backup
tests/                 hardware test automation, test cases, results
docs/                  hardware, installation, Home Assistant, assignment, schematics
custom_components/p4gw Home Assistant integration (HACS), tests in tests/ha_integration
```

## Web UI and API

Port 80, single page embedded in the firmware with a left menu (sections and sub pages, deep links
`#section/page`), in Czech or English with a light, dark or system
theme (switches in the header, remembered by the browser). Sign-in once an account exists (users, roles, 2FA and API tokens: [docs/users.md](docs/users.md)). Main endpoints: `/api/status`, `/api/role`, `/api/logs`, `/api/logs/previous` (each line starts with unix ms or `+<ms since boot>`), `/api/ota/{gateway,c6fw}`, `/api/radios` (and `/api/radios/b/{probe,flash,console}`, `/api/radios/thread/forget`), `/api/esphome/wifi`,
`/api/c6/{backup,reflash,zigbee-snapshot}` (GET download, POST back up now), `/api/thread/{status,dataset,cli}`,
`/api/zigbee/{status,reset,command}`, `/api/net/scan`, `/api/net/{lan,lan/confirm,traffic}`, `/api/reboot` (`{"full": true}` resets the C6 too), `/api/mqtt`, `/api/settings`, `/api/settings/{export,import}`, `/api/update/{check,install}`, `/api/tools/ping`, `/api/coredump`, `/api/ws` (WebSocket push for the Home Assistant integration).
The OTBR REST API for Home Assistant is on port 8081 (Thread border router role).

## Licenses

Third-party sources are fetched at build time and not included: esp-thread-br and esp-hosted-mcu
(Apache-2.0), esp-coordinator (MIT), esp-serial-flasher (Apache-2.0), espressif/qrcode (Apache-2.0, qrcodegen MIT), esp-zigbee-lib /
esp-zboss-lib (Espressif license, binary). Web UI icons: Material Design Icons (Apache-2.0), Thread
and Matter logos from selfh.st/icons (CC BY 4.0; trademarks of Thread Group and the Connectivity
Standards Alliance). Own code: no license file yet (all rights reserved).
