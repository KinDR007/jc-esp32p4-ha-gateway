# Wi-Fi uplink through a radio

Goal: a radio (internal C6 = radio A, external XIAO ESP32-C5 = radio B) gives the gateway a Wi-Fi
connection to the home network, for a place without Ethernet or as a fallback when the LAN link is
down. A new radio role `wifi_uplink`, next to Thread, Zigbee and ESPHome.

Status: analysis and spike (2026-11-02). Nothing of it is in the firmware yet.

## How

ESP-Hosted-MCU: the radio runs the ESP-Hosted co-processor firmware (Wi-Fi only, no Bluetooth), the
P4 runs the host part with `esp_wifi_remote`, so the P4 gets an ordinary Wi-Fi station netif (own
IP, IPv6, mDNS, multicast). A true L2 interface matters: the Thread border router needs IPv6 router
advertisements and multicast on its backbone link, Home Assistant finds the gateway by mDNS. A
PPP or NAT bridge on the radio would break both.

Transport: UART for both radios. ESP-Hosted carries Wi-Fi over UART ("Hosted HCI", byte credit flow
control, checksums, no hardware flow control) and is tested with an ESP32-P4 host and an ESP32-C5
co-processor up to 16 Mbaud on a board. The transport is chosen at build time, one per firmware, so
SDIO (only radio A has it, inside the module) would rule out radio B. UART uses the wiring the
gateway already has: radio A on JP1 (P4 GPIO4/5, reset GPIO54), radio B on JP1 (P4 GPIO32/33, reset
by the flasher pins). The host is pointed at the radio's pins at start (role of radio A or B).

Sources: [ESP-Hosted-MCU, getting started MCU, UART](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/getting-started-mcu.md#4-uart),
[UART design](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/design/uart.md).

## Spike (build only, no hardware)

Gateway built with `espressif/esp_hosted` 3.0.10 and `espressif/esp_wifi_remote`, UART transport,
co-processor ESP32-C5, nothing calling the Wi-Fi API yet:

| | today | with esp-hosted | slot |
|---|---|---|---|
| image | 2 832 152 B | 2 977 584 B (+145 kB) | 3 145 728 B (gw_a/gw_b) |
| static internal RAM | 273 028 B | 277 684 B (+4.6 kB) | 576 kB |

The radio side builds from `third_party/esp-hosted-mcu` (pinned v2.12.13, `scripts/build_c6.sh
esp_hosted`): host and co-processor must run the same major version, so the pin moves to 3.0.x.

## Risks

| Risk | Impact | Mitigation |
|---|---|---|
| Flash of the P4 app slot: 168 kB left after the spike, the Wi-Fi calls add more | the image no longer fits, no OTA | measure in W2; if short: layout v4, gw_a/gw_b 4 MB each from the unused `esphome` partition (USB migration like v3) |
| Internal RAM of the P4: 103 kB free, minimum 53 kB today | Wi-Fi buffers starve TLS, HTTP, OpenThread | ESP-Hosted buffers from PSRAM (`ESP_CACHE_MALLOC`), lwIP pbufs measured under load, soak |
| UART throughput: below 1 Mbit/s recommended, jumper wires on JP1 | slow web UI, lost frames at high baud | start at 921600, measure 2/3/4 Mbaud on the real wiring, checksums on; the gateway's own traffic (OTBR, Zigbee2MQTT TCP, HA API) is small |
| A radio is spent on Wi-Fi | with radio A only: no Thread or Zigbee | the role is clear about it in the UI; the intended setup: radio A Thread/Zigbee, radio B (C5, 5 GHz, Wi-Fi 6) the uplink |
| C6 slot size: the co-processor image (Wi-Fi only) may exceed the 768 kB slots | no slot for it on the C6 | measure in W1; else a C6 slots layout with a larger slot (the C6 has 1.2 MB unused); the C5 has 8 MB |
| Thread backbone bound at start | the border router stays on the interface it started on | backbone = Ethernet when the link is up at start, else Wi-Fi; a lasting change of the link restarts the gateway onto the other one (hysteresis, see W4) |
| Another IP and MAC over Wi-Fi | Home Assistant entries set up with the LAN IP fail | same hostname over mDNS, integration rediscovers by name; docs: DHCP reservation for the Wi-Fi MAC |
| 2.4 GHz Wi-Fi next to Thread/Zigbee on the same board | interference with the 802.15.4 radio | prefer 5 GHz on the C5; show the Wi-Fi channel next to the 802.15.4 channel (radio surroundings page) |
| Wi-Fi password on the gateway | secret in NVS | the secret settings the ESPHome Wi-Fi already uses (never returned by the API) |
| Two default routes (LAN and Wi-Fi up together) | traffic on the wrong interface | route priority: Ethernet first; Wi-Fi only for the default route when Ethernet has no link |

## Phases

| Phase | Content | Done when |
|---|---|---|
| W0 spike | host build footprint (above) | done |
| W1 co-processor | `wifi_uplink` bundle: ESP-Hosted 3.0.x slave, UART on the radio's UART pins (C6: GPIO16/17, C5: GPIO11/12, UART1 through the GPIO matrix, console off), Wi-Fi only, for C6 and C5; in the radio packages | both images built, sizes known, slots decided |
| W2 host | `role_wifi` on the P4: ESP-Hosted host on the pins of the radio that has the role, Wi-Fi settings (SSID, password secret, band preference), station netif, route priority, status in `/api/status` (`wifi`: state, SSID, RSSI, channel, band, IP, link speed of the transport) | the gateway gets an IP over radio B, web UI reachable over Wi-Fi |
| W3 roles | the role in Radios › Roles for A and B (at most one uplink), conflicts (radio A uplink → no Thread/Zigbee unless radio B runs it), Telegram `/role`, the overview diagram shows the uplink | every allowed combination switches |
| W4 fallback | backbone choice at start, failover with hysteresis (LAN down > 60 s → Wi-Fi; LAN back > 5 min → Ethernet at the next quiet moment or immediately when Thread BR is off), notifications | pulling the LAN cable keeps the gateway reachable |
| W5 tests and docs | test matrix below in `tests/hw_test.py` (TC), `tests/ui/ui_test.mjs` (UI), HA integration over Wi-Fi; docs/hardware, installation, CHANGELOG | matrix green on the board |

## Test matrix

Radios: A = internal C6, B = external C5. "uplink" = `wifi_uplink`.

| # | Radio A | Radio B | LAN | Expected |
|---|---|---|---|---|
| 1 | Thread BR | Zigbee router | up | today's behaviour unchanged (regression) |
| 2 | Thread BR | uplink | up | Wi-Fi connected as standby, Ethernet carries traffic and the Thread backbone |
| 3 | Thread BR | uplink | down at start | backbone and IP over Wi-Fi, OTBR REST, HA, web UI reachable by hostname |
| 4 | Thread BR | uplink | pulled while running | gateway reachable over Wi-Fi after the failover; Thread network keeps running |
| 5 | Thread BR | uplink | plugged back | back on Ethernet after the hysteresis |
| 6 | Zigbee coordinator | uplink | down | Zigbee2MQTT connects over Wi-Fi (TCP 6638) |
| 7 | uplink | Thread BR | down | the same with the radios swapped (radio A as uplink) |
| 8 | uplink | none | down | gateway alone on Wi-Fi |
| 9 | uplink | uplink | – | refused: one uplink at a time |
| 10 | uplink | ESPHome | up | both Wi-Fi roles run (ESPHome on its own Wi-Fi) |
| 11 | any → uplink → any | | | role switch both ways keeps the Thread dataset and the Zigbee network data |
| 12 | uplink, wrong password / no AP | | down | clear error in status and UI, retries, LAN still used when it comes |
| 13 | uplink, radio B unplugged | | | radio B shown missing, gateway on LAN as before |
| 14 | uplink, online update over Wi-Fi | | down | gateway update and radio packages download over Wi-Fi |
| 15 | uplink, throughput | | down | web UI, log download, backup download; iperf-like numbers recorded |
| 16 | soak over Wi-Fi | | down | 24 h, no restart, memory stable (only on request) |
