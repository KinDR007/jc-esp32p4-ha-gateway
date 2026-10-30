# Two radios: internal C6 and an external XIAO ESP32-C6 (plan)

Status: planned, nothing implemented. Decisions of the user (2026-10-30) are marked **decided**.

## Goal

A second ESP32-C6 (Seeed XIAO ESP32-C6) on the JP1 header works at the same time as the C6 on the
board. Each radio gets its own role; the gateway detects the external one, tests its wiring, and
flashes either radio with the firmwares of the release (as far as the release supports the board).

## Decisions

| Topic | Decision |
|---|---|
| External module | XIAO ESP32-C6 (4 MB flash, RF switch GPIO3/GPIO14, U.FL connector) |
| Roles | free per radio: Thread BR / Thread router / Zigbee coordinator / Zigbee router / ESPHome / off; at most one Thread and one Zigbee network |
| ESPHome | becomes a radio role: a standalone ESPHome BLE proxy on the C6 (Wi-Fi + BLE) on either radio; the current ESPHome role (ESPHome image on the P4, esp-hosted over SDIO) is dropped |
| Detection | at start and with a "Test wiring" button in the UI, step by step |
| Moving the Zigbee network between radios | yes, through the network snapshot (`c6data`) |
| Role change | restarts the whole gateway (as today) |
| XIAO antenna | chosen in the UI, external U.FL by default |
| Power and wiring | 5 V from JP1, wires soldered to the RESET and BOOT button pads |

## Wiring (JP1 → XIAO)

| JP1 | P4 | XIAO | Purpose |
|---|---|---|---|
| 19 | GPIO32 (UART2 TX) | D7 / GPIO17 (U0RXD) | P4 → C6 |
| 21 | GPIO33 (UART2 RX) | D6 / GPIO16 (U0TXD) | C6 → P4 |
| 12 | GPIO46 | RESET button pad (EN) | reset |
| 10 | GPIO47 | BOOT button pad (GPIO9) | download mode |
| 4 | 5V | 5V | power |
| 6 | GND | GND | ground |

GPIO32, 33, 46 and 47 are free on JP1 and unused by the firmware ([hardware.md](hardware.md)).
Never connect the XIAO's USB-C while it is powered from JP1 (its 5V pin is VBUS). Wires up to
30 cm (460800 Bd without flow control).

## Phases

| Phase | Content | Done when |
|---|---|---|
| F0 hardware | wiring; EN/GPIO9 pads measured; XIAO factory flash backed up over its USB-C; does the XIAO transmit without GPIO3 set; does a Zigbee network move to another chip with the coordinator's IEEE address | findings written here |
| F1 refactoring | radio context (UART, pins, NVS keys, progress, chip info, slots, snapshots) in `c6_flasher`, `role_thread`, `role_zigbee`, `netscan`; radio A keeps today's NVS keys (upgrade without migration), radio B uses `c6b_*` | full hw_test with radio A only passes, no behaviour change |
| F2 detection | BOOT+RESET then ROM sync over UART2 (TX, RX, RESET, BOOT at once); if that fails, RESET only and listen for the ROM banner (tells RX/RESET from TX/BOOT faults, crossed TX/RX, no chip); chip must be an ESP32-C6 with 4 MB flash (MAC, revision); result per step with the wire to check; never resets radio A | TC38: connected, not connected, one wire off |
| F3 roles per radio | NVS `role` (A) and `role_b`; validation; `apply_role`/`ensure_c6` per radio; Thread and Zigbee on UART1 or UART2, TCP 6638 unchanged; network scan on a radio without a role | TC39: Thread on A and Zigbee on B at once (ping, device join) |
| F4 firmware and releases | board flag (`module`/`xiao`, antenna) in the slot selection sector, the C6 firmwares set the RF switch from it; `c6fw.boards` in the manifest and package; flashing B only from a package that supports the XIAO; flash/reflash per radio | external radio flashed from the release |
| F5 Zigbee move | on a role move: snapshot from the source radio, restored on the target | TC40: roles swapped, devices still answer |
| F6 ESPHome on the C6 | ESPHome BLE proxy image for the C6 (Wi-Fi + BLE) as a slot or whole-flash bundle; Wi-Fi credentials and the API key set from the gateway (research: Improv serial over the UART, ESPHome key provisioning from HA), never in a public release; the P4 ESPHome role and its image removed, gateways with role `esphome` fall back with a notification | BLE proxy on radio B while radio A runs Thread |
| F7 interfaces | System › Radios page (state, wiring test, flash, factory backup, antenna), roles of both radios, header tags, REST, MQTT, HA integration (radio B entities), Telegram (`/role b …`) | UI, HA and Telegram tests |
| F8 tests and docs | TC38–TC40 and regression without radio B; hardware.md, installation.md, home-assistant.md, HTML documentation | 24 h soak when the user asks |

Estimate: 3–4 weeks including F6.

## Risks

| Risk | Impact | Handling |
|---|---|---|
| The Zigbee coordinator's IEEE address is the chip MAC; moving the network to the other C6 changes it | devices bound or reporting to the coordinator stop reporting | check in F0; set the address in the NCP from the snapshot (`esp_zb_set_long_address`), otherwise move with a warning |
| Two 2.4 GHz radios next to each other | interference, CCA busy | channels far apart (suggested from the network scan), XIAO antenna away from the board |
| ESPHome on the C6 needs Wi-Fi and a per-installation API key | the image cannot be published as is | provisioning from the gateway (F6 research) |
| Dropping the P4 ESPHome role | users of that role lose it on update | release notes, fallback with a notification |
| Two power sources (XIAO USB-C and JP1 5V) | back-powering | UI and documentation warning |
| Refactoring the flasher and roles | regressions on radio A | F1 alone, full hw_test before F2 |
