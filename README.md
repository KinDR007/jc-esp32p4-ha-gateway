# P4 gateway: Thread / Zigbee / BLE proxy on Guition JC-ESP32P4-M3-DEV

LAN gateway for Home Assistant on the JC-ESP32P4-M3-DEV board (ESP32-P4 + ESP32-C6 in the
module, Ethernet IP101). One board runs one role at a time; the role is switched from the web UI,
from MQTT or from the ESPHome device in Home Assistant.

| Role | P4 runs | C6 runs | Home Assistant side |
|---|---|---|---|
| Thread border router | OpenThread BR (esp-thread-br), REST on port 8081 | `ot_rcp` | OTBR + Thread integrations, Matter |
| Thread router | OpenThread FTD joined to an existing network | `ot_rcp` | nothing, extends the mesh |
| Zigbee coordinator | UART-TCP bridge, port 6638 | `zigbee_ncp` (ZBOSS NCP, esp-coordinator) | Zigbee2MQTT `adapter: zboss` |
| Zigbee router | monitor / commands for the C6 | `zigbee_router` (this repo) | joins via permit join |
| ESPHome BLE proxy | ESPHome image (own slot) | `esp_hosted` (SDIO) | ESPHome integration |

Why one role at a time: the C6 has one 802.15.4/BLE radio and cannot run a Thread router and a
BLE scanner together ([coexistence matrix](https://docs.espressif.com/projects/esp-idf/en/latest/esp32c6/api-guides/coexist.html)).
Background and rejected variants: [zadani.md](zadani.md).

## Architecture

```
P4 flash (16 MB, firmware/partitions.csv)
  nvs       shared settings: role, Thread network, MQTT, passwords
  gw_a/gw_b gateway firmware A/B, OTA with rollback
  esphome   ESPHome BLE proxy image
  c6fw      C6 firmware bundles (ot_rcp, zigbee_ncp, zigbee_router, esp_hosted)
  c6data    Zigbee network snapshots taken from the C6 before leaving a Zigbee role
  coredump
```

- **Switching** writes the role to NVS and restarts. On boot the gateway compares the bundle on
  the C6 with the one the role needs and reflashes the C6 through its ROM bootloader over the
  JP1 UART (about 30 s). For ESPHome it then selects the ESPHome slot and restarts into it.
- **Zigbee data survives switching**: before the C6 leaves a Zigbee firmware its `zb_storage` and
  `zb_fct` regions are copied to `c6data` (one slot per Zigbee firmware) and written back when
  that firmware returns. The Thread network lives in the P4 NVS and is never touched by C6 flashing.
- **Stability**: gateway images confirm themselves after 60 s, otherwise the bootloader rolls back.
  After repeated crashes during start the role is not started (maintenance mode, web UI stays up). An RCP failure
  triggers a C6 reflash, limited to three attempts. Core dumps are kept in flash and downloadable.
- **First start never overwrites the C6** until a role is chosen in the UI, so its factory
  esp-hosted firmware can be backed up first (System -> Back up C6 flash).

## Hardware preparation

Three jumpers on header JP1 (no soldering), see [zadani.md](zadani.md) and [docs/](docs/):

| From | To | Purpose |
|---|---|---|
| JP1 11 (P4 GPIO4, TX) | JP1 18 (C6 U0RXD) | UART P4 -> C6 |
| JP1 13 (P4 GPIO5, RX) | JP1 20 (C6 U0TXD) | UART C6 -> P4 |
| JP1 15 (P4 GPIO20) | JP1 22 (C6 IO9, BOOT) | P4 can put the C6 into its bootloader |

C6 reset (CHIP_PU) is P4 GPIO54 inside the module. The ESPHome role uses the internal SDIO lines
instead and ignores the jumpers.

The tested board reports chip revision **v1.3** (engineering sample). Both the gateway
(`sdkconfig.defaults`) and the ESPHome config (`engineering_sample: true`) are set for it. For a
v3.x board remove those lines and rebuild.

## Build

Requirements: ESP-IDF **v5.5.5** (`IDF_PATH`, default `~/esp/esp-idf`), ESPHome 2026.10.x
(default: the venv of `../esphome/ble_proxy`, override with `ESPHOME=`), Python 3.

```sh
cp firmware/esphome/secrets.yaml.example firmware/esphome/secrets.yaml   # API key: openssl rand -base64 32
./scripts/build_all.sh        # fetch pinned sources, build everything, assemble dist/
```

Single parts: `scripts/build_c6.sh <ot_rcp|zigbee_ncp|zigbee_router|esp_hosted>`,
`scripts/build_gateway.sh`, `scripts/build_esphome.sh`. Third-party sources are pinned in
[deps.lock](deps.lock) and fetched into `third_party/` by `scripts/fetch_deps.sh`;
`esp_ot_br_server` is built with [a small patch](firmware/gateway/patches/esp_ot_br_server.patch)
(REST port, explicit start, no Wi-Fi provisioning code).

`dist/` contents:

| File | Use |
|---|---|
| `p4gw-factory.bin` | first USB flash, everything at once |
| `p4gw.bin` | gateway update (web UI -> System) |
| `esphome.bin` | ESPHome role image (web UI -> System) |
| `c6fw.bin` | C6 firmware bundle (web UI -> System) |

## First installation

1. Back up the P4 flash (the CH340 link is only reliable at 115200 Bd):
   `scripts/backup_flash.sh /dev/cu.wchusbserial* esp32p4 backups/p4_backup.bin`
2. Flash: `scripts/flash_p4.sh /dev/cu.wchusbserial*` (USB-C next to the Ethernet jack).
3. Open `http://p4gw-xxxxxx.local/` (or the DHCP address). The gateway is in maintenance mode.
4. **System -> Back up C6 flash** and keep the file: it is the only copy of the factory C6 firmware.
5. **System -> Settings**: set a password. **Role**: pick a role.

## Home Assistant

The **Home Assistant** tab of the web UI shows the exact steps and URLs for the active role.
Short version:

- **Thread border router**: add the *OpenThread Border Router* integration with
  `http://<ip>:8081`, mark the network preferred in the *Thread* integration, sync credentials to
  the phone with the Companion app. The HA host needs IPv6 with RA/RIO accepted and no multicast
  filtering towards the gateway.
- **Thread router**: copy the active dataset TLVs of the HA preferred network and apply them in
  the *Thread* tab.
- **Zigbee coordinator**: Zigbee2MQTT `serial: {port: tcp://<ip>:6638, adapter: zboss}`. ZHA only
  with the experimental `zha-zboss-esp` custom component.
- **Zigbee router**: permit join in the Zigbee host, then *Join network* in the *Zigbee* tab.
- **ESPHome**: discovered automatically; return to a gateway role with the *Gateway role* select
  and *Return to gateway* button of the ESPHome device.
- **MQTT (optional)**: broker settings in the *Home Assistant* tab. The gateway then appears via
  MQTT discovery with role select, restart button and diagnostics.

## Web UI and API

Port 80, single page (`firmware/gateway/web/index.html`, embedded gzipped). HTTP Basic auth
(user `admin`) once a password is set. Tabs: overview, role switch, Thread (status, neighbors,
routers, dataset, OpenThread CLI), Zigbee (bridge counters or router control, network backups),
Home Assistant (setup guide, MQTT), tools (ping, live log, core dump), system (slots, uploads,
settings, C6 backup/reflash).

| Endpoint | Method | Purpose |
|---|---|---|
| `/api/status` | GET | everything the overview shows |
| `/api/role` | POST `{"role": "thread_br"}` | switch role and restart |
| `/api/logs?since=N` | GET | log ring buffer, header `X-Log-Seq` |
| `/api/ota/gateway`, `/api/ota/esphome`, `/api/ota/c6fw` | POST binary | uploads |
| `/api/c6/backup` | GET | full C6 flash dump (maintenance mode only) |
| `/api/c6/reflash` | POST | reflash the C6 on next start |
| `/api/c6/zigbee-snapshot?bundle=zigbee_ncp` | GET | Zigbee network snapshot |
| `/api/thread/status`, `/api/thread/dataset`, `/api/thread/cli` | GET/POST | Thread tools |
| `/api/zigbee/status`, `/api/zigbee/reset`, `/api/zigbee/command` | GET/POST | Zigbee tools |
| `/api/mqtt`, `/api/settings`, `/api/tools/ping`, `/api/coredump` | GET/POST/DELETE | misc |

The OTBR REST API for Home Assistant is served by esp-thread-br on port 8081 (Thread border
router role only).

## Troubleshooting

| Symptom | Check |
|---|---|
| Maintenance: "C6 bootloader not responding" | JP1 jumpers 11-18, 13-20, 15-22 |
| Thread RCP failures counting up | *System -> Reflash C6 now*; antenna / IPEX connector |
| HA cannot reach Thread devices | IPv6 RA/RIO on the HA host, multicast snooping on switches |
| Zigbee2MQTT cannot connect | only one TCP client at a time; *Zigbee* tab shows the current one |
| ESPHome image does not boot | the gateway comes back (rollback) and shows the reason |
| Restore C6 factory firmware | `esptool --chip esp32c6 write-flash 0 c6_flash_backup.bin` over a USB-UART adapter on JP1 18/20, IO9 to GND |

## Licenses

Own code: see repository. Third party: esp-thread-br and esp-hosted-mcu (Apache-2.0),
esp-coordinator (MIT), esp-serial-flasher (Apache-2.0), esp-zigbee-lib / esp-zboss-lib (Espressif
license, binary).
