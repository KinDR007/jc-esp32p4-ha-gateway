# Build, installation and maintenance

## Requirements

- ESP-IDF **v5.5.5** (`IDF_PATH`, default `~/esp/esp-idf`); checked by `scripts/env.sh`
- ESPHome 2026.10.x (`ESPHOME=` or the venv of `../esphome/ble_proxy`)
- Python 3, git, bash

Third-party sources are pinned in [deps.lock](../deps.lock) and fetched into `third_party/`
(ignored by git) by `scripts/fetch_deps.sh`.

## Build

```sh
cp firmware/esphome/secrets.yaml.example firmware/esphome/secrets.yaml   # openssl rand -base64 32
./scripts/build_all.sh
```

| Output in `dist/` | Content |
|---|---|
| `p4gw-factory.bin` | complete flash image (bootloader, partition table, gateway, ESPHome, C6 bundles) |
| `p4gw.bin` | gateway application for OTA |
| `esphome.bin` | ESPHome BLE proxy role image |
| `c6fw.bin` | C6 bundles: ot_rcp, zigbee_ncp, zigbee_router, esp_hosted |
| `SHA256SUMS` | checksums |

Single parts: `scripts/build_c6.sh <name>`, `scripts/build_gateway.sh`, `scripts/build_esphome.sh`,
`scripts/pack_c6fw.py`.

## Flash layout

[firmware/partitions.csv](../firmware/partitions.csv): `nvs` (settings, Thread network), `gw_a`/`gw_b`
(gateway A/B with rollback), `esphome`, `c6fw` (C6 bundles), `c6data` (Zigbee network snapshots,
one slot per Zigbee firmware), `coredump`. Offsets must not change on deployed boards.

## First installation

1. Back up the P4 (keeps the factory firmware, which also embeds the factory C6 app):
   `scripts/backup_flash.sh /dev/cu.wchusbserial* esp32p4 backups/p4_backup.bin`
2. Wire the JP1 jumpers ([hardware.md](hardware.md)).
3. `scripts/flash_p4.sh /dev/cu.wchusbserial*` (resets NVS).
4. Open `http://p4gw-xxxxxx.local/`. The gateway is in maintenance mode.
5. **System → Back up C6 flash** (about 3.5 min) and keep the file.
6. **System → Settings**: password. **Role**: choose the role.

The gateway never overwrites a C6 it has not flashed itself until a backup is done or skipped
explicitly. The ESPHome role works with the factory C6 firmware and without jumpers.

## Updates

| What | How | Safety |
|---|---|---|
| Gateway | UI System → Gateway firmware (`p4gw.bin`) | inactive slot, confirmed after 60 s, else rollback |
| ESPHome image | UI System → ESPHome image (`esphome.bin`) | project name checked before the slot is erased |
| C6 firmware | UI System → C6 firmware bundle (`c6fw.bin`) | header checked before erase, sha256 per bundle; C6 reflashed on next start if its bundle changed |

Uploads are refused for the first 60 s after a gateway update, until the new image is confirmed.

## Role switching

Web UI (Role), MQTT (`p4gw/<host>/role/set`), API `POST /api/role {"role": ...}` or, in the ESPHome
role, the *Gateway role* select and *Return to gateway* button. Typical times measured on the
board: Thread roles 13–15 s, Zigbee roles 25–27 s (C6 flash), ESPHome hand-over about 60 s.

## Recovery

| Situation | Action |
|---|---|
| Role fails to start | UI shows maintenance with the reason; pick another role or fix and restart |
| C6 not responding | check jumpers, **System → Reflash C6 now** |
| Bad gateway image | automatic rollback; otherwise `scripts/flash_p4.sh` |
| Repeated crashes | after several crashes in a row the role is not started, UI stays reachable; download the core dump |
| Restore factory C6 | write the full C6 backup with esptool over a USB-UART on JP1 20/22, IO9 (pin 24) to GND; the factory app can also be extracted from the P4 factory backup (embedded `network_adapter` image at 0x5f0000 of the tested board) |
| Restore factory P4 | `esptool --chip esp32p4 write-flash 0 backups/p4_backup.bin` |

## Tests

[tests/TESTCASES.md](../tests/TESTCASES.md), automation [tests/hw_test.py](../tests/hw_test.py),
results in [tests/results/](../tests/results/).
