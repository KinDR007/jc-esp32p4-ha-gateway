# Wi-Fi uplink through a radio

Goal: a radio (internal C6 = radio A, external XIAO ESP32-C5 = radio B) gives the gateway a Wi-Fi
connection to the home network, for a place without Ethernet or as a fallback when the LAN link is
down. A new radio role `wifi_uplink`, next to Thread, Zigbee and ESPHome.

Status: W0–W4 done, W5 tests (2026-11-02).

## Decisions (2026-11-02)

- Without LAN and without a working Wi-Fi setting the radio opens an access point with a captive
  portal: the setup page for the Wi-Fi of the gateway.
- In Wi-Fi mode one radio carries the Wi-Fi, the other one takes a role. By default the internal
  radio A is the Wi-Fi and radio B (external) is free for a role; the user can swap them.
- Wi-Fi gets the same settings and statistics as the LAN: DHCP or static IPv4, traffic counters
  and throughput per service.
- Implementation order: radio B (C5) first, then radio A (same firmware, other pins).
- Setup access point: open, only for a few minutes after start (no password to print or show).
- LAN and Wi-Fi up together: Ethernet carries the default route, Wi-Fi stays as standby.

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
| W1 co-processor | `wifi_uplink` bundle: ESP-Hosted 3.0.10 co-processor (example `wifi/apsta`, station and access point), UART1 on the radio's UART0 pads (C6 GPIO16/17, C5 GPIO11/12, console off), 921600 Bd, no Bluetooth, optimized for size; in both radio packages with its own whole-flash layout like ESPHome | done: C6 926 kB, C5 965 kB (too large for the 768 kB slots, so a switch to or from the uplink rewrites the slots layout, ~70 s); packed 550 kB, c6fw.bin 2.8 MB, c5fw.bin 2.9 MB of 4 MB; 7 of 8 package entries used |
| W2 host | `role_wifi` on the P4 (`espressif/esp_hosted` 3.0.10, same version as the co-processor): ESP-Hosted host on the pins of the radio that has the role, Wi-Fi settings (SSID, password secret, band preference), station netif, route priority, status in `/api/status` (`wifi`: state, SSID, RSSI, channel, band, IP, link speed of the transport) | done: radio B joins in 12 s after the start, own IPv4 (DHCP) and IPv6 link-local, web UI over Wi-Fi 81 kB/s (LAN 1 MB/s), Ethernet keeps the default route; image 2 987 824 B (158 kB left in the slot), internal heap free 108 kB, minimum 58 kB |
| W3 roles | the role in Radios › Roles for A and B (at most one uplink), swap of the roles of A and B in one restart (`POST /api/role {"swap": true}`), System › Wi-Fi (settings, scan, state, traffic), Telegram `/role`, the overview diagram, HA integration | done |
| W3b portal | open access point named like the gateway on the uplink radio, only after a start without LAN and without Wi-Fi (from 45 s to 10 min after the start, open longer while a phone is on it, 30 min at most): DHCP with the gateway as DNS, every name answered with the gateway, unknown pages (phone connectivity checks) redirected to the web UI; no station scans while a phone is on it | done: a Mac joins, gets 192.168.4.x, is sent to the web UI, sets the network, the gateway joins it |
| W4 fallback | the default route follows the route priority (Ethernet 50, Wi-Fi 20) at once; the Thread backbone is chosen at the start (Ethernet when it has a link and an address within 30 s, else Wi-Fi) and moves by a restart (LAN down 60 s with Wi-Fi up; LAN back 5 min); `POST /api/net/lan/outage` stops Ethernet for a test, across a software restart | done: reachable over Wi-Fi at once, backbone on Wi-Fi 60–90 s after the outage began, back on Ethernet 318 s after the LAN returned |
| W5 tests and docs | TC41–TC45 in `tests/hw_test.py`, UI26 in `tests/ui/ui_test.mjs`, docs, CHANGELOG | see the test results |

## Notes from the hardware

- The radio's ROM prints its boot banner at 115200 Bd on the transport pins. At 921600 Bd it is noise
  in which the host's frame decoder finds a false header and waits for bytes that never come, so the
  co-processor's INIT is never read. The host holds the radio in reset until its UART listens, then
  drops everything up to the first quiet gap of 20 ms before it opens the transport.
- The host has no reset line of its own (`pin_reset = -1`): radio B's reset is open drain, driven by
  the flasher's `c6_hold_reset()`.
- The flasher runs the same wiring at 460800 Bd only, so speeds above 921600 Bd are not proven on the
  jumper wires yet.
- One host image serves both radios: the host is built for the ESP32-C5 co-processor and refuses
  any other chip, so the gateway builds it from the pinned checkout with a patch that accepts a
  known chip with a warning (`firmware/gateway/patches/esp_hosted_any_cp.patch`).
- In access point plus station mode the radio has one channel: when the station joins a network
  on another channel, the phone on the setup access point drops and has to join again. While the
  access point is open the station retries only once a minute (each scan takes the radio off the
  access point's channel) and not at all while a phone is joined.
- The web server once stopped answering on port 80 after many setup access point runs (ping and
  port 8081 still answered; cause unknown). The gateway now asks its own web server every 60 s and
  after three misses aborts with a core dump (`/api/coredump`) and restarts.
- lwIP has one DNS list and each DHCP answer replaces it: the gateway keeps the servers of the LAN and
  of the Wi-Fi and puts back those of the interface carrying the traffic (the Wi-Fi in standby does not
  replace the LAN's; LAN gone, the Wi-Fi's apply). The IDF option for DNS per interface
  (`CONFIG_ESP_NETIF_SET_DNS_PER_DEFAULT_NETIF`) lost the LAN servers beside the Thread DNS64 entry
  with a static LAN address (TC32). Name lookups still fail for a moment while the uplink comes up
  at the start, so the radio package download tries three times.
- A radio package staged for radio B's chip (an upload) is used by radio B before radio A starts:
  radio A may fetch its own package into the same PSRAM buffer.
- ESP-Hosted is not started at boot (`CONFIG_ESP_HOSTED_AUTO_CALL_INIT_BEFORE_APP_MAIN=n`) and never
  restarts the gateway on a transport failure (`..._RESTART_ON_FAILURE=n`); its task stacks and
  buffers go to PSRAM.

## API

| | |
|---|---|
| `GET /api/net/wifi` | settings (`ssid`, `password_set`, `from_esphome`, `mode`, static fields) and status |
| `POST /api/net/wifi` | `{"ssid", "password" (left out: kept), "mode": "dhcp"\|"static", "ip", "mask", "gw", "dns1", "dns2"}`, applied at once |
| `GET /api/net/wifi/traffic` | counters per service like `/api/net/traffic`, from the first connection |
| `GET /api/net/wifi/scan` | networks in range (while the uplink runs) |
| `/api/status` `role_status` / `role_status_b` | `mode: "wifi_uplink"`, `state`, `error`, `ssid`, `rssi`, `channel`, `band`, `phy`, `ipv4`, `default_route`, … |

Without a network of its own the uplink uses the Wi-Fi set for ESPHome.

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
