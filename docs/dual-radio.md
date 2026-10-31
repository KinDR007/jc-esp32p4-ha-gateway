# Two radios: internal C6 and an external XIAO ESP32-C5 (plan)

Status: planned; build definitions for the C5 firmwares exist (`scripts/build_c6.sh all esp32c5`).
Decisions of the user (2026-10-30, changed 2026-10-31: XIAO ESP32-C5 instead of the C6, no moving
of networks between radios).

## Goal

A second radio, a Seeed XIAO ESP32-C5 (802.15.4 like the C6), on the JP1 header works at the same
time as the C6 on the board. Each radio gets its own role; the gateway detects the external one, tests its wiring, and
flashes either radio with the firmwares of the release (as far as the release supports the board).

## Decisions

| Topic | Decision |
|---|---|
| External module | XIAO ESP32-C5 (ESP32-C5 rev 1.0, 48 MHz crystal, 8 MB flash, 8 MB PSRAM, U.FL antenna connector); the C6 firmwares are built for it as well |
| Roles | free per radio: Thread BR / Thread router / Zigbee coordinator / Zigbee router / ESPHome / off; at most one Thread and one Zigbee network |
| ESPHome | becomes a radio role: a standalone ESPHome BLE proxy on the C6 (Wi-Fi + BLE) on either radio; the current ESPHome role (ESPHome image on the P4, esp-hosted over SDIO) is dropped |
| Detection | at start and with a "Test wiring" button in the UI, step by step |
| Networks and radios | no moving of networks between radios: each radio keeps its own network settings, and each can be backed up |
| Radio firmwares | not kept in the P4 flash (2026-10-31): the C6 and C5 packages are downloaded from the GitHub release (or uploaded in the web UI) into PSRAM and flashed into the chosen radio from there; role switches work offline (see F4) |
| PSRAM for packages | one 4 MB buffer that survives a software restart (flashing happens at start, an upload in the UI works without internet); one package at a time, C6 and C5 one after the other when both radios are flashed; its content is dropped after flashing (2026-10-31) |
| PSRAM of the C5 | the XIAO ESP32-C5 has 8 MB PSRAM: use it where a firmware gains from it (larger Zigbee tables and buffers in the NCP, ESPHome connections), each verified on the hardware (F4b) |
| Thread settings | the P4 keeps the Thread settings (dataset, OpenThread settings) per radio (2026-10-31): Thread on the other radio starts with that radio's own network |
| Role change | restarts the whole gateway (as today) |
| Antenna | the XIAO ESP32-C5 has only the U.FL connector (antenna included), nothing to switch |
| Power and wiring | the XIAO powered through its own USB-C (5 V from JP1 is too weak, see wiring); RST and BOOT are pads on the back of the XIAO |

## Wiring (JP1 → XIAO ESP32-C5)

| JP1 | P4 | XIAO ESP32-C5 | Purpose |
|---|---|---|---|
| 19 | GPIO32 (UART2 TX) | D7 / GPIO12 (U0RXD) | P4 → C5 |
| 21 | GPIO33 (UART2 RX) | D6 / GPIO11 (U0TXD) | C5 → P4 |
| 12 | GPIO46 | RST pad (back) | reset |
| 10 | GPIO47 | BOOT pad (back, GPIO28) | download mode |
| 6 | GND | GND | ground |

GPIO32, 33, 46 and 47 are free on JP1 and unused by the firmware ([hardware.md](hardware.md)).
Power the XIAO through its own USB-C (a 5 V supply), not from JP1: on the 5 V of JP1 the C5 resets
with RTC_PWR_GLITCH as soon as its radio starts (2026-10-31: the scanner restarted 34 times in 6 s,
also with 100 µF added; on USB power it scans normally and the Zigbee coordinator runs). Only GND
and the four signals go to JP1; never connect JP1 5V and the USB-C at the same time (the 5V pin of
the XIAO is its VBUS). The P4 drives the reset line open-drain (like the RESET button). Wires up to
30 cm (460800 Bd without flow control). Factory flash of the XIAO used here:
`backups/xiao_c5_factory.bin` (8 MB, read over its USB-C).

Differences to the C6 that the gateway handles: bootloader at 0x2000 (C6: 0x0), 8 MB flash, ROM
chip detection; the slots layout (`firmware/c6/slots.csv`, 4 MB) fits as it is.

## Phases

| Phase | Content | Done when |
|---|---|---|
| F0 hardware | done: C5 build definitions, factory backup, the scanner runs on the XIAO (Wi-Fi and 802.15.4 networks found); open: wiring to JP1, ot_rcp and zigbee_ncp on the C5 | findings written here |
| F1 refactoring | radio context (UART, pins, NVS keys, progress, chip info, slots, snapshots) in `c6_flasher`, `role_thread`, `role_zigbee`, `netscan`; radio A keeps today's NVS keys (upgrade without migration), radio B uses `rb_*` (NVS namespace `radiob`) | full hw_test with radio A only passes, no behaviour change |
| F2 detection | BOOT+RESET then ROM sync over UART2 (TX, RX, RESET, BOOT at once); if that fails, RESET only and listen for the ROM banner (tells RX/RESET from TX/BOOT faults, crossed TX/RX, no chip); chip must be an ESP32-C6 with 4 MB flash (MAC, revision); result per step with the wire to check; never resets radio A | TC38: connected, not connected, one wire off |
| F3 roles per radio | NVS `role` (A) and `role_b`; validation; `apply_role`/`ensure_c6` per radio; Thread and Zigbee on UART1 or UART2, TCP 6638 unchanged; network scan on a radio without a role | TC39: Thread on A and Zigbee on B at once (ping, device join) |
| F4 firmware and releases | separate package `c5fw.bin` per release (same format, bootloader at 0x2000), `c5fw` in the manifest; the gateway keeps no radio firmware in its flash: packages only in PSRAM (one 4 MB noinit buffer, one package at a time, dropped after flashing), downloaded from the release when a radio needs one (first install, new release, reflash) or uploaded in the UI; per radio NVS keeps the installed slots layout (sha), the slot of each firmware and the Zigbee data regions, so a role switch writes otadata computed by the gateway without any package (offline); flasher for the C5 (ROM detection, 8 MB); flashing B only from a release with `c5fw`; flash/reflash per radio; the then unused `c6fw` partition (64 kB) holds the Zigbee snapshots of radio B (no partition table change) | both radios flashed from the release, role switches without internet |
| F4b PSRAM on the C5 | `CONFIG_SPIRAM` in the C5 builds; larger network tables and buffers of the Zigbee NCP and router (esp-zigbee `esp_zb_overall_network_size_set`, `esp_zb_io_buffer_size_set`, `esp_zb_scheduler_queue_size_set`); ESPHome later | a Zigbee network with the larger tables runs on the C5 |
| F5 settings per radio | Zigbee: in the radio's own flash, snapshots per radio in `c6data`. Thread: OpenThread keeps its settings in the NVS namespace `openthread` of the P4; before the Thread role starts, the gateway parks the settings of the radio that ran Thread last in `ot_a`/`ot_b` and brings back the chosen radio's (a flag makes the swap safe against a power cut; no partition table change, unlike a separate NVS partition through `storage_partition_name`). Backup and restore per radio | TC40: Thread on A, then on B (own network), back on A (first network again); backup and restore of radio B |
| F6 ESPHome on the C6 | ESPHome BLE proxy image for the C6 (Wi-Fi + BLE) as a slot or whole-flash bundle; Wi-Fi credentials and the API key set from the gateway (research: Improv serial over the UART, ESPHome key provisioning from HA), never in a public release; the P4 ESPHome role and its image removed, gateways with role `esphome` fall back with a notification | BLE proxy on radio B while radio A runs Thread |
| F7 interfaces | System › Radios page (state, wiring test, flash, factory backup, antenna), roles of both radios, header tags, REST, MQTT, HA integration (radio B entities), Telegram (`/role b …`) | UI, HA and Telegram tests |
| F8 tests and docs | TC38–TC40 and regression without radio B; hardware.md, installation.md, home-assistant.md, HTML documentation | 24 h soak when the user asks |

Estimate: 3–4 weeks including F6.

## Risks

| Risk | Impact | Handling |
|---|---|---|
| C5 support of the stacks (esp-zboss-lib 1.6.8 and esp-zigbee-lib 1.6.4 ship esp32c5 libraries; ot_rcp lists the C5) | a stack misbehaves on the C5 | F0: run each firmware on the XIAO before F1 |
| Two 2.4 GHz radios next to each other | interference, CCA busy | channels far apart (suggested from the network scan), XIAO antenna away from the board |
| ESPHome on the C6 needs Wi-Fi and a per-installation API key | the image cannot be published as is | provisioning from the gateway (F6 research) |
| Dropping the P4 ESPHome role | users of that role lose it on update | release notes, fallback with a notification |
| Two power sources (XIAO USB-C and JP1 5V) | back-powering | UI and documentation warning |
| Refactoring the flasher and roles | regressions on radio A | F1 alone, full hw_test before F2 |
