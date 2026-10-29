# P4 gateway: Thread / Zigbee / BLE proxy on Guition JC-ESP32P4-M3-DEV

LAN gateway for Home Assistant on the JC-ESP32P4-M3-DEV board (ESP32-P4 + ESP32-C6 in the
module, Ethernet IP101). One board runs one role at a time; the role is switched from the web UI,
MQTT, the REST API or the ESPHome device in Home Assistant.

| Role | P4 runs | C6 runs | Home Assistant side |
|---|---|---|---|
| Thread border router | OpenThread BR (esp-thread-br), REST on port 8081 | `ot_rcp` | OTBR + Thread integrations, Matter |
| Thread router | OpenThread FTD joined to an existing network | `ot_rcp` | nothing, extends the mesh |
| Zigbee coordinator | UART-TCP bridge, port 6638 | `zigbee_ncp` (ZBOSS NCP, esp-coordinator) | Zigbee2MQTT `adapter: zboss` |
| Zigbee router | monitor and commands for the C6 | `zigbee_router` (this repo) | joins via permit join |
| ESPHome BLE proxy | ESPHome image (own slot) | `esp_hosted` (SDIO) | ESPHome integration |

Independent of the role (except ESPHome): **sound recognition** with the onboard microphone,
YAMNet on the P4 (521 AudioSet classes, about 60 ms per 0.96 s of audio, off by default), see
[docs/sound-detection.md](docs/sound-detection.md).

Status: all roles and transitions pass the automated hardware tests (10/10 regression, see
[tests/results/2026-10-28/report.md](tests/results/2026-10-28/report.md)); the OTBR integration
runs in Home Assistant. Open manual tests are listed in [TODO.md](TODO.md).

## Documentation

| Document | Content |
|---|---|
| [docs/hardware.md](docs/hardware.md) | board, chip revision, **JP1 jumpers**, pinout, USB notes |
| [docs/installation.md](docs/installation.md) | build, first installation, updates, role switching, recovery |
| [docs/home-assistant.md](docs/home-assistant.md) | Home Assistant setup per role, MQTT |
| [docs/zadani.md](docs/zadani.md) | original assignment and feasibility analysis (Czech) |
| [docs/sound-detection.md](docs/sound-detection.md) | sound event detection: research and proposal (Czech, pending) |
| [tests/TESTCASES.md](tests/TESTCASES.md) | test cases (automated and manual) |
| [tests/results/](tests/results/) | test runs, logs and reports |

## Quick start

```sh
./scripts/fetch_deps.sh
cp firmware/esphome/secrets.yaml.example firmware/esphome/secrets.yaml   # API key
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
  esphome   ESPHome BLE proxy image
  c6fw      C6 firmware bundles
  c6data    Zigbee network snapshots taken before leaving a Zigbee role
  coredump
```

- Switching stores the role and restarts. On boot the gateway compares the C6 bundle with the
  one the role needs and updates the C6 through its ROM bootloader over JP1. The Thread and Zigbee
  firmwares share one C6 layout with a slot each, so a switch only selects the slot (8 kB,
  [docs/c6-slots.md](docs/c6-slots.md)). For ESPHome it selects the ESPHome slot and restarts into it.
- Zigbee network data survives switching (snapshot to `c6data`, restored on return). The Thread
  network lives in P4 NVS and is not touched by C6 flashing.
- Stability: gateway images confirm themselves after 60 s (else rollback), repeated crashes stop
  role start but keep the UI, RCP failures trigger a limited number of C6 reflashes (a recovery
  restart counts as an unstable boot), core dumps are kept in flash. Sound recognition pauses while
  an upload or C6 flashing writes the P4 flash (both cores stall on every flash write).
- C6 telemetry (temperature, memory, uptime) reaches the web UI over each role's own link, see
  [docs/hardware.md](docs/hardware.md#c6-telemetry). The ESP-IDF openthread component is built from
  a patched copy (`firmware/patches/`, applied by `firmware/cmake/patched_openthread.cmake`).

## Repository layout

```
firmware/gateway/      P4 gateway (ESP-IDF): gw_core, c6_flasher, role_thread, role_zigbee, web UI
firmware/c6/           C6 builds: sdkconfig overlays, patches, shared telemetry, own zigbee_router firmware
firmware/patches/      patch of the ESP-IDF openthread component (C6 telemetry)
firmware/esphome/      ESPHome BLE proxy role and hand-over header
firmware/partitions.csv
scripts/               env, fetch, build, pack, flash, backup
tests/                 hardware test automation, test cases, results
docs/                  hardware, installation, Home Assistant, assignment, schematics
```

## Web UI and API

Port 80, single page embedded in the firmware with a left menu (sections and sub pages, deep links
`#section/page`), in Czech or English with a light, dark or system
theme (switches in the header, remembered by the browser). HTTP Basic auth (user `admin`) once a password is
set. Main endpoints: `/api/status`, `/api/role`, `/api/logs`, `/api/ota/{gateway,esphome,c6fw}`,
`/api/c6/{backup,reflash,zigbee-snapshot}`, `/api/thread/{status,dataset,cli}`,
`/api/zigbee/{status,reset,command}`, `/api/net/scan`, `/api/mqtt`, `/api/settings`, `/api/tools/ping`, `/api/coredump`.
The OTBR REST API for Home Assistant is on port 8081 (Thread border router role).

## Licenses

Third-party sources are fetched at build time and not included: esp-thread-br and esp-hosted-mcu
(Apache-2.0), esp-coordinator (MIT), esp-serial-flasher (Apache-2.0), esp-zigbee-lib /
esp-zboss-lib (Espressif license, binary). Web UI icons: Material Design Icons (Apache-2.0), Thread
and Matter logos from selfh.st/icons (CC BY 4.0; trademarks of Thread Group and the Connectivity
Standards Alliance). Own code: no license file yet (all rights reserved).
