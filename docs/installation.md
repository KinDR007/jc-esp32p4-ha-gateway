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
| `c6fw.bin` | C6 bundles: slots layout (ot_rcp, zigbee_router, zigbee_ncp, scanner), esp_hosted ([c6-slots.md](c6-slots.md)) |
| `SHA256SUMS` | checksums |

Single parts: `scripts/build_c6.sh <name>`, `scripts/build_gateway.sh`, `scripts/build_esphome.sh`,
`scripts/pack_c6fw.py`.

## Flash layout (v2)

[firmware/partitions.csv](../firmware/partitions.csv): `nvs` (settings, Thread network), `gw_a`/`gw_b`
(3 MB each, gateway A/B with rollback), `esphome` (2 MB), `c6fw` (2 MB, deflate-compressed C6
bundles), `c6data` (Zigbee network snapshots, one slot per Zigbee firmware), `model` (4 MB, YAMNet),
`sounds` (1.56 MB, learning mode recordings), `coredump`.

Migration from v1 (first install on 2026-10-28): `scripts/flash_p4.sh <port>` writes every image but
keeps NVS, so the role, Thread network, password and MQTT survive. The C6 is reflashed on the next
start (new bundle format). Zigbee snapshots of v1 are not carried over.

## Sound recognition model

`scripts/build_model.sh` creates the pinned Python 3.11 toolchain (`tools/yamnet/requirements.lock`),
fetches the official YAMNet weights (checksum checked), checks the device log-mel code against the
reference and exports `tools/yamnet/out/yamnet_p4.espdl`. `build_all.sh` adds it to `dist/` and the
factory image. To replace the model on a running board: turn sound off, restart, upload
`yamnet_p4.espdl` in the Sound tab, turn sound on.

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
| Gateway | UI System → Gateway firmware (`p4gw.bin`) | inactive slot, confirmed after 60 s or by a requested restart, else rollback |
| Sound model | UI Sound → Upload model (`yamnet_p4.espdl`) | ESP-DL header checked before the partition is erased; sound must be off |
| ESPHome image | UI System → ESPHome image (`esphome.bin`) | project name checked before the slot is erased |
| Gateway, online | UI System → Firmware → Online update, or the *Firmware* update entity in Home Assistant (MQTT): manifest URL, checked every 6 h | as above: the other gateway slot, image name checked before it is erased |
| C6 firmware | UI System → C6 firmware bundle (`c6fw.bin`), after the gateway (format 3 needs gateway v1.5) | header checked before erase, sha256 per bundle; C6 updated on next start if its bundle changed |
| C6 firmware, online | UI System → Firmware → Online update → **Install C6 package**, the *C6 radio firmware* update entity in Home Assistant, or Telegram `/install c6` | downloaded into PSRAM, sha256 of the file compared with the manifest and the header checked before the c6fw partition is erased; then a restart updates the C6 |

Uploads are refused for the first 60 s after a gateway update, until the new image is confirmed.

Online update: `scripts/make_update_manifest.py [--notes TEXT] [--release-url URL] [--base-url URL]`
writes `dist/manifest.json` next to `dist/p4gw.bin` and `dist/c6fw.bin`; copy the three to a web
server the gateway reaches, e.g. `/config/www/p4gw/` of Home Assistant, and set
`http://<ha>:8123/local/p4gw/manifest.json` as the manifest URL.

```json
{"version": "v1.5", "p4gw": "p4gw.bin", "notes": "...", "release_url": "...",
 "build": {"commit": "abc1234", "date": "2026-10-30", "idf": "v5.5.5", "ci": "..."},
 "c6fw": {"url": "c6fw.bin", "sha256": "...", "size": 2015232, "format": 3,
          "bundles": {"slots": "4 firmware slots", "ot_rcp": "esp_ot_rcp v5.5.5-p4gw.3", "...": "..."}}}
```

URLs may be absolute. The gateway compares `version` with its own and the sha256 of the C6 package
with the file in its c6fw partition (versions shown as the first 8 hex digits of that sha256);
nothing is downloaded until someone installs. A package of another `format` is offered only to a
gateway that reads it, so the gateway goes first. HTTPS works with the ESP-IDF certificate bundle.
The ESPHome image is still uploaded in the UI (CI does not build it, it needs the API key).

Releases: pushing a tag `v*` runs `.github/workflows/release.yml`: it builds `p4gw.bin` and `c6fw.bin`,
takes the notes from the CHANGELOG section of the tag (or *Unreleased*, `scripts/release_notes.py`)
and publishes a GitHub release with `manifest.json` (absolute URLs of the tag). The gateway reads
`https://github.com/KinDR007/jc-esp32p4-ha-gateway/releases/latest/download/manifest.json`: the
default while no URL is saved (button *GitHub Releases* puts it back; a saved empty URL turns the
online update off). The release text holds the CHANGELOG section and the commits since the previous
tag (`scripts/release_notes.py`); the manifest only the section.

CI (GitHub Actions, `.github/workflows/ci.yml`): `scripts/ci_checks.sh` (unit test, Python and web
UI script syntax, icon block) and `scripts/ci_build.sh` in `espressif/idf:v5.5.5` (C6 firmwares,
gateway, `c6fw.bin`; artifacts `p4gw.bin` and `c6fw.bin`). The ESPHome image is not built there, it
needs the API key. Run the build locally the same way:
`docker run --rm -v "$PWD":/src espressif/idf:v5.5.5 bash -c 'cp -a /src /w && /w/scripts/ci_build.sh'`.

## Role switching

Web UI (Role), MQTT (`p4gw/<host>/role/set`), API `POST /api/role {"role": ...}` or, in the ESPHome
role, the *Gateway role* select and *Return to gateway* button. Typical times measured on the
board: Thread roles 13–15 s, Zigbee roles 25–27 s (C6 flash), ESPHome hand-over about 60 s.

## LAN settings

**System › LAN** (or `POST /api/net/lan`): DHCP (default) or a static IPv4 address with netmask, gateway
and up to two DNS servers, and the link mode (autonegotiation or a fixed 100/10 Mb/s, full or half
duplex). A change applies at once and has to be confirmed within 120 s (`POST /api/net/lan/confirm`,
the banner in the web UI), from the new address when it changes; otherwise the previous settings come
back, also after a restart in between. Thread and Zigbee keep running; a new link mode restarts the
link for a few seconds. Zigbee2MQTT connects to the address, so a new one has to go into its
configuration too; the P4 Gateway integration follows it over mDNS.

The same page shows the Ethernet traffic since boot and its rate, in total and per service (web UI and
API, OTBR REST, Zigbee2MQTT, MQTT, mDNS, DNS/NTP, HTTPS, Thread traffic routed by the border router).
Frames are counted by port in the input and output path of the interface, without copying or delaying
them.

## Recovery

| Situation | Action |
|---|---|
| Role fails to start | UI shows maintenance with the reason; pick another role or fix and restart |
| C6 not responding | check jumpers, **System → Reflash C6 now** |
| Bad gateway image | automatic rollback; otherwise `scripts/flash_p4.sh` |
| Repeated crashes | after several crashes in a row the role is not started, UI stays reachable; download the core dump |
| Repeated crashes, cause unknown | **System → Diagnostics → Log before the last restart** (last 16 KiB of the previous run, gone after a power cut) |
| New board, erased NVS | **System → Settings → Settings backup**: export (with passwords if wanted) beforehand, import on the new board. Zigbee network data is not part of it: it lives on the C6 and in the old board's `c6data` (downloadable under Zigbee › Network backups, not restorable on another board yet) |
| Restore factory C6 | write the full C6 backup with esptool over a USB-UART on JP1 20/22, IO9 (pin 24) to GND; the factory app can also be extracted from the P4 factory backup (embedded `network_adapter` image at 0x5f0000 of the tested board) |
| Static LAN address that does not work | not confirmed: it reverts after 120 s by itself; confirmed: the gateway is still reachable over IPv6 as `<host>.local`, or import a settings backup without the `lan` key |
| Restore factory P4 | `esptool --chip esp32p4 write-flash 0 backups/p4_backup.bin` |

## Tests

[tests/TESTCASES.md](../tests/TESTCASES.md), automation [tests/hw_test.py](../tests/hw_test.py),
results in [tests/results/](../tests/results/).
