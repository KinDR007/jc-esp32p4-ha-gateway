# C6 firmware slots

Research and result, 2026-10-29: can the C6 switch between Thread and Zigbee without being
reflashed on every role switch?

## Short answer

Yes, with all role firmwares in their own slots of one C6 layout. A role switch then writes only
otadata (8 kB) and, for the Zigbee roles, the network data (16 kB); the C6 restarts into the
selected slot. Switching without a C6 restart is not possible, and one combined firmware is not
worth it. Implemented since the slots layout (c6fw format 3).

## Options

| Option | Result |
|---|---|
| One firmware with both stacks, role chosen at run time | Not worth it. OpenThread and ZBOSS both define the same 7 strong 802.15.4 driver callbacks (`esp_ieee802154_receive_done`, `transmit_done`, ...; the IDF driver only has weak defaults for one user), so the binary Espressif libraries would need renamed symbols and a dispatcher. esp-zigbee-lib 1.6.8 has no stack deinit, so a switch needs a C6 restart anyway. Saving: ~170 kB. |
| Slots: every firmware in its own app partition, the gateway selects one | Implemented. No change in any C6 firmware: the gateway writes otadata through the ROM loader it uses for flashing anyway. |

## Flash use

App sizes (C6 flash 4 MB): ot_rcp 233 kB (Thread border router and Thread router use the same
RCP, OpenThread runs on the P4), zigbee_ncp 590 kB, zigbee_router 587 kB, scanner 571 kB,
esp_hosted 1198 kB.

A combined Thread + Zigbee image would share ~170 kB (PHY 29 kB, hw support 27 kB, UART 13 kB,
NVS 12 kB, system 10 kB, HAL 10 kB, 802.15.4 driver 9 kB, SPI flash 8 kB, ...; measured with
`esp_idf_size` over the map files): ot_rcp + zigbee_router 818 kB as two images, ~650 kB as one.
That is ~4 % of the C6 flash, which holds all four slot firmwares with room to spare.

Layout ([firmware/c6/slots.csv](../firmware/c6/slots.csv)), 2.8 MB of 4 MB:

| Partition | Offset | Size | Content |
|---|---|---|---|
| nvs, otadata, phy_init | 0x9000 | 36 kB | shared |
| ota_0 | 0x20000 | 384 kB | ot_rcp (Thread border router and Thread router) |
| ota_1 | 0x80000 | 768 kB | zigbee_router |
| ota_2 | 0x140000 | 768 kB | zigbee_ncp (coordinator) |
| ota_3 | 0x200000 | 768 kB | scanner (full network scan) |
| zb_storage, zb_fct | 0x2C0000 | 17 kB | Zigbee network data |

The rollback pair of the C6 is not needed: the gateway keeps every image and reinstalls it.
ESPHome keeps its own two-slot layout (esp-hosted updates the C6 over SDIO into its OTA slots), so
switching to ESPHome replaces the slots layout and returning installs it again (~70 s).

ZBOSS finds its data by the label `zb_storage`, so the coordinator and the router share one
region; the gateway keeps each one's network in its `c6data` snapshot as before (16 kB copy per
switch). Snapshots taken in the old per-firmware layouts are restored by size.

## How it works

- `scripts/pack_c6fw.py --slots ...` packs a bundle `slots` (bootloader, partition table, all slot
  firmwares) and, per slot firmware, a bundle that only writes otadata (+ PRESERVE of the Zigbee
  data) with a SLOTS marker carrying the first 16 bytes of the `slots` sha256.
- The gateway installs `slots` when the C6 does not hold that exact layout (sha256 kept in NVS),
  then writes the role's otadata. A new c6fw.bin changes the marker, so the role bundle is
  "different" and the new layout gets installed at the next start. "Reflash C6" installs the whole
  layout again.
- Update order: `p4gw.bin` first, then `c6fw.bin` (format 3); an older gateway rejects format 3.

## Measured (board, 2026-10-29)

| | Before (one firmware at a time) | Slots |
|---|---|---|
| Data written to the C6 per switch | 232–604 kB | 8 kB (+16 kB Zigbee data) |
| C6 write phase | ~6 s (Thread) to ~15 s (Zigbee) | 1.6–2.8 s |
| Whole role switch incl. P4 restart, Zigbee | 27–28 s | 14–19 s |
| Whole role switch, Thread | 12–16 s | 12–17 s |
| One-time layout install | | 68 s |

Hardware tests TC04–TC09, TC12, TC13, TC16, TC19–TC23 pass (Thread BR and router, coordinator
with ZBOSS frames and network data restore, router back in its Zigbee2MQTT network, ESPHome round
trip, full network scan); 16 further switches thread_br ↔ zigbee_router without an error.

Open: twice in a quick succession of switches (TC23 started right after an aborted run) the RCP
stopped answering after the Thread start; the gateway recovered by itself (forced reflash). Not
reproduced in the next 16 switches, see TODO.md.
