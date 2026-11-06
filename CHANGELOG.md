# Changelog

## Unreleased

## v1.14 (2026-11-06)

Summary: Modbus TCP server for the gateway's Modbus devices (RS485, Modbus TCP, GoodWe UDP): shared port 502 by unit id or own ports, cache of polled registers and of answers to clients, rate limit, optional writes with an address allowlist.

- Modbus TCP server, System › Modbus TCP server (`GET`/`PUT /api/modbus/server`): the gateway's Modbus
  devices (RS485, Modbus TCP, GoodWe UDP) for other Modbus TCP clients in the network. The shared port
  502 routes by unit id, a device may also get its own port (any unit id). Reads of polled registers come
  from a cache of the blocks read, and for `cache_ms` (1500-5000 ms, default 2000; 0 off) so do answers to
  clients; a write drops what it touches; the same read of two clients goes to the device once. Requests
  wait between the blocks of a poll, not for the whole poll; at most `rate` per second (else exception 6);
  writes only where switched on, optionally only from listed addresses. Up to 8 ports, 2 clients per port,
  8 in all. lwIP limits raised: 40 sockets, 32 active and 24 listening TCP connections.
- Tests: TC55, UI37, unit tests of the server's request decoding; `tools/modbus/server_load.py` (load
  test, results in docs/modbus.md); the simulator logs reads with `--log-reads`, `--delay` plays a slow device.

## v1.13 (2026-11-06)

Summary: free GPIO of header JP1 (input, output, PWM, counter, UART-TCP bridge, second I2C bus, 1-Wire DS18B20, WS2812) in the web UI and Home Assistant (integration 1.7.0, MQTT group `gpio`); SNTP servers and the gateway time zone for the whole gateway.

- Time settings, System › LAN › Time (`GET`/`POST /api/time`): up to three SNTP servers (default
  pool.ntp.org) and the gateway time zone (a listed IANA zone or a POSIX rule), applied at once. The zone
  holds for the whole gateway: the web UI shows times (header clock, log, backups, events) in it instead of
  the browser's zone, Telegram quiet hours follow it, the nightly Zigbee backup runs 03:00-04:00 local
  time (was 02:00-03:00 UTC). The time zone moved there from the Telegram settings (same key, no migration;
  `tz` of `POST /api/telegram` still sets it). `/api/status` carries `tz_name`.
- Tests: TC51, UI35.
- GPIO of header JP1, System › GPIO (JP1) (`/api/gpio`): the header drawn with every pin's use, radio pins
  locked with the reason; free pins (GPIO1, 2, 3, 45; radio B's while radio B has no role and none is found)
  as input (pull, inverted, debounce), output (open drain, pulse, read back on the pad), PWM (1 Hz-1 MHz)
  or pulse counter (rate, frequency, glitch filter), state after a restart per pin (off, on, last).
  Telegram `/gpio`.
- Home Assistant: integration 1.7.0 shows GPIO set to *integration* as the device "<host> GPIO" (input
  binary sensor, output switch, PWM light with the duty as brightness, counter count and frequency);
  MQTT discovery group `gpio` with the same entities and commands `gpio/<n>/set` (ON, OFF, PULSE <ms>) and
  `gpio/<n>/duty/set`. A stored selection of all eight MQTT groups gets the new one.
- GPIO functions over the chip's peripherals: UART-TCP bridge (UART4, TCP port 6639 by default, one
  client at a time), second I2C bus for the I2C sensors (`"bus": 2`, ids `i2c2_<addr>`, System › Hardware
  shows the bus), 1-Wire DS18B20/DS1822/DS18S20 (each a temperature in Home Assistant), WS2812 LED strip
  (colour, brightness, rainbow effect; MQTT JSON light). MQTT commands may carry up to 191 bytes.
- Tests: TC52, TC53, TC54, UI36, integration tests of the GPIO device.
- Fix: an MQTT settings change that switched groups on and off at once (e.g. a new broker with other
  groups) published the newly switched-on groups to the old broker before reconnecting; now only the
  removals go there.

## v1.12 (2026-11-04)

Summary: Modbus TCP search in a network, Victron GX and GoodWe XS templates, Modbus over UDP for GoodWe Wi-Fi modules.

- Modbus TCP search in a network (System › Modbus TCP search, `/api/modbus/scan`): connects to the
  chosen ports of every address of a range (up to /22), asks each open one a few unit ids, recognises
  Victron GX (with its VE.Bus unit) and GoodWe by their serial numbers and adds a recognised device with
  one click; GoodWe Wi-Fi dongles answering only on UDP 8899 are listed too. Runs beside the polling.
- Modbus templates of a Victron GX: `victron_gx_system` (unit 100) and `victron_vebus` (MultiPlus /
  Quattro), from Victron's register list 3.81, checked on a MultiPlus-II GX.
- Modbus over UDP for GoodWe Wi-Fi modules (connection "GoodWe UDP", `transport: "udp"`, port 8899):
  Modbus RTU frames out, `AA 55` answers back, also split into several datagrams. The network search
  lists such a module with the unit that answered and adds a DT family inverter (unit 127) in one click.
  Template `goodwe_xs` (XS, MS, D-NS) from the goodwe library's DT map, checked on a GW3000-XS.
- Tests: TC49, UI33; the Modbus simulator plays text registers as serial numbers (no spaces).
- Tests: TC50, UI34 and a unit test of the AA 55 framing; the simulator plays a GoodWe module (`--udp`).
- `tools/modbus/udp_relay.py`: a UDP relay on a computer that lets the gateway reach a device in a network
  only that computer reaches (e.g. over a VPN), for testing.
- Tests: Modbus TCP simulator of a Huawei SUN2000 hybrid PV inverter with battery and meter (simulated
  day, power limit, start/stop) and its template to import (`tests/modbus_tcp/`).
- 3D printable case for the board (OpenSCAD, two shells, flexible BOOT/RST/PWR buttons, bay for the
  XIAO radio; `hardware/enclosure/`).

## v1.11 (2026-11-03)

Summary: I2C sensors on connector CN3 (24 chip families, recognised by the gateway) and Modbus devices on RS485 and Modbus TCP with JSON templates (Eastron SDM, ORNO, Hiking, Chint, Peacefair built in), each shown in Home Assistant through the integration or MQTT.

- **Needs flash layout v4**: the image is larger than 3 MB, so the online update offers v1.11 only to
  gateways on layout v4 (System › Information, `update.layout`). A gateway on layout v3 migrates once
  over USB with `scripts/migrate_layout_v4.sh <port>` (settings, networks, users and recordings stay).
- Modbus, System › RS485: devices on the board's RS485 port (J4, UART3) and over Modbus TCP, described
  by JSON templates (read and write: number, switch, select, button). Built-in templates after research
  and counter-research: Eastron SDM120, SDM230, SDM630 V2, SDM72D-M V1/V2, ORNO OR-WE-504, 514/515,
  516/517, Hiking DDS238-2 ZN/S, Chint DTSU666, DDSU666, Peacefair PZEM-016/014, PZEM-017/003. Own
  templates in the web UI (editor with checks, import/export), address search over addresses and baud
  rates, manual requests with register decoding. Each device is its own device in Home Assistant via the
  gateway, through the integration or MQTT (group `modbus`, command topics). API `/api/modbus*`,
  Telegram `/modbus`, permission `system/rs485` ([docs/modbus.md](docs/modbus.md)).
- MQTT: commands below the base topic for components; discovery entries may describe their own device;
  the discovery group mask is stored under a new key (eight groups).
- I2C sensors on connector CN3 (or JP1), System › Hardware: 24 chip families (Bosch BME/BMP, Sensirion
  SHT3x/SHT4x/SCD4x/SGP30, AHT2x, Si7021/HTU21D, HDC1080, ENS160, PMSA003I, BH1750, VEML7700, TSL25x1,
  LTR390, OPT3001, MAX44009, TMP117, MCP9808). A scan at every start and on request identifies chips by
  their ID registers and adds those recognised without doubt; for shared addresses the user picks the
  chip. Name, expose and enable per sensor, read interval. API `/api/sensors`, `/api/sensors/scan`,
  Telegram `/sensors`, page permission `system/hw` ([docs/sensors.md](docs/sensors.md)).
- Home Assistant: integration 1.6.0 adds a sensor entity per exposed I2C sensor and quantity; MQTT
  discovery group `sensors` (a stored "all groups" of older firmware includes it).
- MQTT: discovery and the state are published only by the publish task, never from the client's event
  handler; this removes a deadlock between the MQTT client and components that publish while holding
  their own lock (sound events, sensors) after a reconnect.
- The I2C bus is shared through gw_core (`gw_i2c`); docs/hardware.md describes the bus, CN3, RS485
  and the other interfaces from the full schematics.
- API reference: the response of `/api/thread/topology` (router, child and vendor fields, time limits)
  and the meaning of the Telegram levels (0 off, 1 critical, 2 warnings and critical, 3 all; quiet hours
  critical only) checked against the code.
- Documentation: Zigbee router screenshots of v1.10.

## v1.10.1 (2026-11-03)

Summary: fix of the C6 update offered again after its install; documentation with v1.10 screenshots.

- Fix: after the C6 package was installed (the radio is flashed at the next start) the update state
  kept the previous package as installed until another restart, so the web UI and Home Assistant
  offered the same C6 update again. The installed package is now read with every state. TC36 checks it.
- Documentation (GitHub Pages): screenshots of v1.10, new chapters Data flows, System › Wi-Fi,
  System › API, the setup wizard, the new overview and Radios › Roles.

## v1.10 (2026-11-03)

Summary: Data flows page, setup wizard, HTTP API reference (System › API, `/api/help`, docs/api.md),
MQTT discovery per group (no duplicates next to the P4 Gateway integration), Radios › Roles as a table,
traffic in the header tooltips, Wi-Fi uplink (and ESPHome on the C5) as radio slots (switching offline),
ESPHome proxy with 5 connections and diagnostics, Home Assistant integration 1.5.0 (C5 update entity),
64 kB more internal RAM, flash layout v4 (optional USB migration, the release also runs on v3).

- Flash layout v4: the `esphome` slot (unused since v1.8) goes to the gateway slots, 3 -> 4 MB each
  (28 % free with the current image instead of 4.5 %). Migration over USB once with
  `scripts/migrate_layout_v4.sh <port>` (settings, Thread network, users, Zigbee snapshots, model and
  sounds stay; `--rollback <stamp>` goes back). `update.layout` reports 4; releases for layout 3 are
  still offered on v4, a manifest names only layout 4 when `p4gw.bin` is larger than 3 MB.
- Sound: the mel filter bank keeps only the used weights (66 kB -> 2.4 kB of internal RAM, same
  output bit for bit).
- mbedTLS allocates from PSRAM: a TLS session (update check, OTA, Telegram, MQTT over TLS) no longer
  takes about 40 kB of internal RAM; the lowest free internal RAM since start went from 85 to 145 kB.
- MQTT: Home Assistant discovery per group (roles, system, update, sound, sound events, sound event
  device triggers), `discovery_groups` of `/api/mqtt`; the web UI offers the preset "Only what the
  integration lacks" (only the device triggers of the sound events), so the P4 Gateway integration and MQTT no longer show entities twice. A group left
  out is removed from Home Assistant, the state topics stay. Test TC46, UI24.
- Home Assistant integration 1.5.0: update entity *C5 radio firmware* for an ESP32-C5 as radio B (the
  C5 package with its firmwares, the ESPHome Bluetooth proxy included; install, progress, rollback to
  a listed release), next to the C6 one.
- ESPHome Bluetooth proxy 1.1.0 (radio role): five active connection slots (default was three) and
  diagnostic entities in Home Assistant: Wi-Fi signal, uptime, chip temperature, free heap and largest
  free block, ESPHome version, IP address, Wi-Fi network, restart button. Image 1.47 MB (C6), 1.52 MB
  (C5), RAM 41 % on the C6.
- Radio slots: the Wi-Fi uplink gets slot ota_4 on both radios (C6 and C5) and the ESPHome proxy slot
  ota_5 on the C5 (8 MB, `firmware/c6/slots_c5.csv`): switching to and from them selects the slot
  (23–28 s) without the package, so a gateway without LAN or internet can still switch a radio to the
  Wi-Fi uplink. Earlier slots and the Zigbee data keep their offsets; the new layout installs once on
  the next start with the new package (95 s on the C6, 150 s on the C5). ESPHome on the C6 (4 MB)
  stays a whole-flash bundle. The C5 ESPHome image is built for 8 MB flash.
- Web UI: the overview diagram nodes (LAN, main board, radio A and B, their networks) show a details
  box on hover (chip, firmware, slot and flash layout, temperatures, RAM, radio chip data and
  telemetry, network state); the header tags add the traffic since start and the current rate of
  their service (LAN, Thread routed by the border router, Zigbee2MQTT, OTBR REST, web UI and
  integration, MQTT, HTTPS/DNS/NTP, Wi-Fi uplink), sampled every 10 s from `/api/net/traffic`.
- Web UI: Radios › Roles as a table (roles × radios, default), a list (one line per role) or the
  tiles, the choice is remembered in the browser; on a phone the switch buttons show only the icon.
  Tests UI27, UI26 updated.
- Web UI: new page Data flows: sources (Thread, Zigbee and Bluetooth devices, microphone), the
  gateway's services (border router, Zigbee coordinator or router, ESPHome proxy, sound recognition,
  state and control, Wi-Fi uplink) and the consumers (Home Assistant, Zigbee2MQTT, MQTT broker,
  Telegram, web UI and API) with drawn links coloured by state, a details box per node with every
  flow through it and its traffic, and a table of all flows with a link to their settings. Test UI28.
- Web UI: setup wizard, a panel at the bottom right that walks through every setting step by step
  (account, LAN, radios and roles, Wi-Fi uplink, Thread, Zigbee, ESPHome, Home Assistant, MQTT,
  notifications, sound, updates) on the real pages, with a check of each step's state; it opens by
  itself while the gateway has no user account, later from System › Information, and continues after
  the restart of a role switch. Test UI29.
- HTTP API reference: `firmware/gateway/web/api.json` describes all 103 endpoints (permission, query,
  body, response, errors, effects; English and Czech). The gateway serves it at `GET /api/help`
  (public, 32 kB gzip); the web UI shows it under System › API with a getting-started (token, address,
  first call) and curl, Python and Home Assistant `rest_command` examples built for that gateway (its
  address, a token typed in, its radios and current roles), GET requests can be tried at once.
  `docs/api.md` and the GitHub Pages documentation are generated from it; `tests/unit/api_spec_test.py`
  (in `scripts/ci_checks.sh`) fails when an endpoint is missing, extra or its permission differs from
  `gw_auth.c`. Test UI30.
- UI tests UI06 and UI07 use the language and theme switches of the side bar again.
- Fix: right after the manifest URL was changed or cleared, the offer of the old URL could still be
  installed until the update task checked again (TC27 installed the GitHub release). The install
  is refused until the current URL was checked.
- Wi-Fi uplink through a radio (docs/wifi-uplink.md): new radio role `wifi_uplink`. The radio runs
  the ESP-Hosted co-processor (package bundle `wifi_uplink`, C6 and C5), the gateway gets an
  ordinary Wi-Fi interface over the radio UART (own address, IPv6, mDNS), about 0.7 Mbit/s.
  Ethernet keeps the default route while its link is up; without it the traffic goes over Wi-Fi at
  once, and the Thread border router moves its backbone to Wi-Fi (restart after 60 s without LAN,
  back to Ethernet after 5 min with LAN). System › Wi-Fi: network (with a scan), password, DHCP or
  static address, state and traffic per service like the LAN page; without a network of its own the
  ESPHome Wi-Fi is used. After a start without LAN and without Wi-Fi an open setup access point named
  like the gateway opens for a few minutes; a phone joining it lands on the web UI. Radios › Roles
  can swap the roles of radio A and B to move the Wi-Fi. API: `/api/net/wifi`, `/api/net/wifi/scan`,
  `/api/net/wifi/traffic`, `POST /api/role {"swap": true}`, `POST /api/net/lan/outage` (failover test).
  Home Assistant integration and Telegram `/role` know the role. Tests TC41–TC45.
- The gateway checks its own web server every minute; three misses in a row restart it with a
  core dump.
- Fix: after the manifest URL was changed, the offer of the old URL stayed installable when the
  check of the new one failed. A changed URL now drops the old offer first; a failed check of the
  same URL still keeps it (server down).
- Online update: the C5 package of radio B is listed like the C6 one (installed and offered
  package, firmwares in it) and can be installed (`{"target": "c5"}`); a release row has a C5
  button when `releases.json` names the package of the release (from the next release on).
- Web UI: the (i) tooltips use the box of the header tags (title, text, table); the PSRAM tooltip
  lists the use by part (radio package buffer, sound model, audio buffers, log, other), from the
  new `heap.psram_heap` and `heap.psram_parts` of `/api/status`.
- Web UI: the overview starts with a diagram LAN, main board ESP32-P4, radio A (internal C6) and
  radio B (external, JP1) with the network each radio serves; the cards follow the hardware (main
  board, LAN, radio A, radio B), each radio card says what it runs. Without radio B the card says
  what it would add.
- UI tests: `--token` (or `P4GW_TOKEN`) signs in with an API token, for users with 2FA.

## v1.9 (2026-11-02)

Menu of the web UI arranged by task ([menu_redesign/](menu_redesign/README.md)), radio B with the
same details and maintenance as radio A, full scan on either radio with 5 GHz Wi-Fi from radio B.
New radio packages (scanner 1.1.0): the online update offers them. Home Assistant integration
unchanged (1.4.0).

- Fix: an upload of the gateway firmware in the web UI could end in a task watchdog restart into the
  old firmware (two radios busy): the slot was erased at once, stalling both cores for seconds. The
  slot is now erased while writing, with a 10 ms rest per block like the online update.
- Web UI: the menu is arranged by task (menu_redesign/): Radios (roles, status and maintenance,
  radio surroundings), ESPHome as its own section, System › Settings split to the pages it belongs
  to; Zigbee backups show their radio and download the right one.
- Web UI: the overview and Radios › Status and maintenance show the same details for radio B as for
  radio A (firmware version, MAC, chip revision, flash size, flash count, temperature min/max,
  memory, uptime, last reset); `/api/radios` carries the chip data of each radio as `chip_info`,
  `/api/status` that of radio B and its bundle under `role.b`. Radio B also gets its firmware list,
  tooltips, Reflash radio B now (`POST /api/c6/reflash?radio=b`) and a flash backup while it has no
  role (`GET /api/c6/backup?radio=b`); the role switch points to that backup for both radios.
- Radio surroundings: the full scan runs on radio A or B; radio B (ESP32-C5) also sees 5 GHz
  Wi-Fi. The Wi-Fi table shows band, channel width up to 160 MHz and the standards as `b/g/n/ax`.
- Radio surroundings: the Wi-Fi table groups the networks of one access point; the 2.4 GHz
  spectrum shows Wi-Fi, the Wi-Fi channels, MHz and 802.15.4 in lanes on one axis with the Thread
  and Zigbee networks found; the channel table is split by Wi-Fi 1/6/11 and the gaps between; a
  5 GHz spectrum (U-NII bands, DFS) after a radio B scan.
- Fix: the full scan showed WPA3 networks as open: the scanner firmware was built without WPA3
  SAE, so the Wi-Fi library did not recognise their key management (scanner 1.1.0, new radio
  packages). A network reported open with a cipher is shown as protected.
- Fix: a full scan that first had to install a new slots layout on the radio (over a minute) ran
  out of time before the scanner started.
- Fix: a C6 backup requested while the C6 was being flashed kept the upload lock, so later uploads
  and backups answered busy until a restart.

- Fix: the gateway could hang (web UI and API stop answering, the radios keep running) when an
  update check ended while MQTT was connecting, typically right after a role switch of radio B that
  fetches the C5 package: the update notification was published with the update lock held, while
  the MQTT task waited for that lock. Notifications are now sent after the lock is released.

## v1.8.1 (2026-10-31)

First release of v1.8 (the v1.8 tag was not released: the ESPHome build failed in CI).
Home Assistant integration 1.4.0 (role select of radio B).

- Fix (build): the ESPHome Bluetooth proxy is built with ESPHome's own ESP-IDF, not the one of the
  CI container.
**Two radios** ([docs/dual-radio.md](docs/dual-radio.md)): a XIAO ESP32-C5 on JP1 (powered over its own
USB-C) is radio B next to the C6 of the board, so Thread and Zigbee run at the same time. **The
ESPHome image on the P4 is gone**: ESPHome is now a role of a radio; a gateway with the old ESPHome
role selected starts radio A as the ESPHome proxy.

- Roles per radio (Role page, `/api/role {"radio"}`, HA *Radio B role* select, Telegram `/role b`):
  at most one Thread network, one Zigbee network and one ESPHome proxy; a failure of radio B does
  not stop the gateway. Each radio keeps its own networks: Thread settings per radio in the P4 NVS,
  Zigbee snapshots per radio (radio B in the c6fw partition); *Forget Thread network* per radio.
- No radio firmware in the P4 flash: the packages (`c6fw.bin`, new `c5fw.bin` in the release and the
  manifest) are downloaded or uploaded into a 4 MB PSRAM buffer and dropped after flashing; a local
  image per radio in NVS keeps role switches offline.
- Radio B: wiring test at start and on request (names the line to check), reset line open-drain,
  `/api/radios`, System › Radios page, header tag and overview card; the C5 builds use its 8 MB PSRAM
  (Zigbee coordinator tables for 400 devices).
- ESPHome Bluetooth proxy on a radio (C6 or C5, ESPHome 2026.10.1, built in CI): Wi-Fi set by the
  gateway over Improv (Home Assistant › ESPHome), API key set by Home Assistant on adoption, no
  secret and no access point in the image.
- The Thread and Zigbee menus and pages follow the radio that runs them.
- Fix: task watchdog while the online update writes the gateway image (10 ms rest per step).
- Web UI: releases for this gateway as expandable rows (version, date, C6 package, install buttons)
  with the release notes formatted (lists, bold, code); the list carries the whole notes, notes cut at
  1000 characters end with "…".
- Web UI, System › Firmware has subpages: *Online update* (gateway and C6 package in one aligned
  table: installed, offered, state, install; firmwares of the C6 package expandable; releases for
  this gateway in their own tile, **Reflash** writes the installed release again), *Manual update*
  (uploads of the gateway and the radio packages in one tile) and *Settings* (manifest URL).
- Web UI, Home Assistant › Guide: the HACS and *Add integration* buttons look like the My Home
  Assistant badges.

## v1.7 (2026-10-30)

Home Assistant integration 1.3.0 (install a given version).

- Online update: older releases can be installed (rollback), the gateway firmware and the C6 package
  separately. Each release carries `releases.json`; the gateway offers only the releases whose manifest
  names its flash layout (`layouts`, from v1.6), so on layout v3 nothing older than v1.6. Web UI
  (*Releases for this gateway*), Home Assistant (`update.install` with `version`, attribute
  `available_versions`), Telegram (`/releases`, `/install v1.6`, `/install c6 v1.6`).
- Fix: the online update checked every 125 s instead of every 6 h (tick overflow).

## v1.6 (2026-10-30)

**Flash layout v3 needs one USB flash to free the space** (`scripts/migrate_layout_v3.sh <port>`, keeps
settings, Thread network, users and recordings). Without it v1.6 runs on the old layout as before.
Gateways on v1.5 cannot read GitHub releases (redirect bug): upload this `p4gw.bin` once by hand.

- Fix: the online update did not follow redirects, so the manifest and the C6 package of a GitHub
  release (github.com answers 302 to its CDN) could not be read; v1.5 gateways have to be updated once
  by hand (upload `p4gw.bin`), later ones follow up to 5 redirects. Larger HTTP buffers for the long
  redirect URL.

- Flash layout v3: the c6fw partition shrinks from 2 MB to 64 kB (directory and the small bundles,
  so Thread/Zigbee role switches stay offline) and the model partition grows to 6 MB. The large C6
  firmwares are read from the whole package in a PSRAM buffer that survives software restarts
  (`EXT_RAM_NOINIT_ATTR`, kept clear of the PSRAM timing tuning pattern at 0x80, cache written back);
  missing after a power cut, the gateway downloads it from the manifest when the C6 needs it.
  Uploads and online updates go through that buffer. Migration over USB:
  `scripts/migrate_layout_v3.sh`; v1.6 runs on layout v2 too. `scripts/c6fw_local.py` builds the
  small image for the factory image and `flash_p4.sh`.

## v1.5 (2026-10-30)

- Online update: the manifest URL defaults to the latest GitHub release of the project
  (`releases/latest/download/manifest.json`); release notes carry the commits since the previous tag.
- Online update of the C6 radio firmwares: the manifest offers the C6 package (`c6fw`: URL, sha256,
  size, format, firmware versions) and the build (commit, date, ESP-IDF, CI run); the gateway compares
  it with its c6fw partition and on request downloads it into PSRAM, checks sha256 and header, stores
  it and restarts to update the C6. Web UI (System › Firmware: both parts, firmware versions installed
  and offered, build, release notes, GitHub Releases preset), Home Assistant (*C6 radio firmware* update
  entity, integration 1.2.0), Telegram (`/update` with notes and build, `/install c6`), notification
  when a new package appears. Release workflow on tags `v*` (`release.yml`, `scripts/release_notes.py`,
  `scripts/make_update_manifest.py`). The c6fw partition stays as the cache (offline recovery);
  dropping it is step B. Test TC36. Fix: the first update check after a start waited 30 s even when
  asked earlier.

- Telegram bot (System › Notifications, My account › Telegram, docs/notifications.md): notifications
  to chats paired with user accounts (level per topic, quiet hours, /mute for sounds, Czech or English)
  and commands with the user's rights (/status /lan /thread /zigbee /noise /events /update /install
  /restart /identify /role /sound), changing ones with a confirmation button and the authenticator code
  for accounts with 2FA. Long-poll over one kept TLS connection. Time zone setting for quiet hours.
  Tests: TC35; hw_test `--token` for gateways with accounts (TC11 and TC34 need an open gateway).
- Home Assistant integration 1.1.0: API token instead of the admin password (config flow and
  re-authentication; old entries keep the password while the admin has no 2FA), Notification event
  entity and persistent notifications per topic and level (options), notifications missed during a
  restart of the gateway fetched from `/api/notify`. Firmware: notifications for system (start and its
  reason, crash, maintenance, C6 radio lost, update available), network (LAN link, internet, Zigbee2MQTT,
  LAN settings) and sounds (critical for alarms and glass), kept in RAM (last 32) with a run id.
- Users, roles and permissions (System › User management, docs/users.md): admin, user and viewer
  roles with a level per page (hidden, read, edit) and own levels per user, enforced by the server for
  every API URI; sign-in with a session cookie, lock after failed attempts, forced password change,
  two-factor sign-in (TOTP, QR code, 10 recovery codes, required per role), password reset requests
  to the admins, API tokens for Home Assistant and scripts, HTTP Basic for accounts without 2FA. The
  single admin password of older versions becomes the account `admin`. Notifications (`gw_notify`,
  topic `notify` on MQTT and `/api/ws`) for security events. Dependency `espressif/qrcode` 0.1.0~2.
  Tests: TC34, TC11 and TC30 updated.

- Home Assistant integration **P4 Gateway** (`custom_components/p4gw`, HACS), modelled on the
  SMLIGHT integration: found by zeroconf, password in the config flow and re-authentication, REST
  polling plus push over a WebSocket, so it needs no MQTT broker. Sensors, role select, sound
  recognition switch, event and binary sensor per sound event, firmware update with progress,
  restart/identify/Zigbee backup buttons, diagnostics. Firmware: mDNS service `_p4gw._tcp` (TXT `mac`,
  `version`) and `/api/ws`, which forwards everything the gateway publishes to MQTT, broker or not,
  through its own queue (a burst of 23 messages arrived whole, one server job per message lost all
  but 6). Tests: `tests/ha_integration` (13, CI with hassfest), TC30.
- Noise in dB(A): A-weighting filter (IEC 61672, within 0.2 dB up to 7 kHz) on the microphone,
  LAS, LAeq and LAmax over 60 s, calibration by one sound level meter reading (Sound › Live ›
  Noise level, `spl_reference`, `spl_offset`, `spl_reset` in `/api/sound/config`), kept when the
  gain changes. Sensors Noise and Noise max (`sound_pressure`, dBA) in the integration and over
  MQTT; the dBFS level is diagnostic now. Tests: `tests/unit/aweight_test.c` (CI), TC31.
- Icon of the gateway (chip with radio waves, `assets/brand/*.svg`, `scripts/gen_brand.sh`): brand
  images of the integration (`custom_components/p4gw/brand/`, read by Home Assistant 2026 from the
  integration folder) and the web UI favicon (SVG, plus `/favicon.ico` 16/32/48 px served without
  a password; 16 and 32 px drawn simplified).
- Home Assistant › Guide: card of the P4 Gateway integration with one-click My Home Assistant buttons
  (open the repository in HACS, start the setup); the same badges in README and docs.
- Web UI: header tags have icons and a box with details on hover (the box of the network maps);
  language (flags) and theme (icons) moved to the bottom of the menu, with a Restart button that, after
  a confirmation, resets the C6 and right after it the P4 (`/api/reboot {"full": true}`).
- LAN settings (System › LAN): DHCP or static IPv4 with DNS servers, link speed and duplex; a change
  must be confirmed within 120 s, else the previous settings come back. Static DNS servers also go to
  the backup entries of the lwIP list, whose first entry the OpenThread DNS64 client overwrites with
  the NAT64 server (without that, a static address with one DNS server lost name resolution).
- Ethernet traffic (System › LAN): bytes and packets since boot and live rate, in total and per service,
  counted by port in the input and output functions of the interface; graph of the last 5 minutes.
  Tests: TC32, TC33.
- Web UI header in the style of SMLIGHT: status tags (outlined when working, grey when off) for LAN,
  the active role, its client (Zigbee2MQTT or OTBR REST), the P4 Gateway integration (`/api/ws`
  clients), MQTT and internet; device uptime with the Zigbee2MQTT session or C6 uptime below it, and the
  gateway clock once SNTP has set it. Firmware: `links` in `/api/status` (`mqtt`, `ha_clients`,
  `internet`, `internet_age_s`); internet means a TCP connection to github.com:443, checked every minute.
- Fix: `auth` in `/api/status` read the password into a 2-byte buffer, so a set password showed as
  none (the UI warned about a missing password) and a removed one as set. TC11 checks it.
- C6 slots layout: the Thread RCP, Zigbee router, Zigbee coordinator and scanner firmwares sit in
  their own slots of one C6 layout (`firmware/c6/slots.csv`, installed once, ~70 s). A role switch
  writes only otadata (8 kB) and the Zigbee network data instead of a whole firmware: the C6 write
  takes 1.6–2.8 s instead of 6–15 s, a Zigbee role switch 14–19 s instead of 27 s. c6fw format 3
  (`pack_c6fw.py --slots`); update the gateway before c6fw.bin. Snapshots from the old layouts are
  restored by size. The Zigbee router now returns to its network after every switch (the old
  layouts erased the C6 NVS on each switch). Research and measurements: docs/c6-slots.md.
  Tests TC13, TC23.
- C6 temperature shows its min and max since the gateway start, like the P4. Test UI18.
- Fix: a gateway, ESPHome or c6fw upload sometimes ended in a task watchdog restart (2 of 5
  uploads with sound recognition on, 0 of 5 with it off). Every flash write stalls both cores and
  a model run slowed down that way kept CPU 1 from its idle task for 10 s. Uploads and C6 flashing
  now hold a lock (`gw_flash_lock`) and recognition skips the model meanwhile
  (`flash_skips` in `/api/sound/status`).
- Log before the last restart: the last 16 KiB of log are also kept in internal RAM that startup
  does not clear, so after a requested, watchdog or panic restart the log of the previous run is
  available (`/api/logs/previous`, System › Diagnostics). Not kept over a power cut. Tests TC24, UI19.
- Settings backup (System › Settings): export of the role, Thread datasets, MQTT, Home Assistant,
  transmit power and sound settings as JSON, passwords only on request; import checks every entry
  before writing, ignores per-board state (C6 firmware, counters) and restarts the gateway
  (`/api/settings/export[?secrets=1]`, `/api/settings/import`). Tests TC25, UI20.
- Zigbee network backup on request and automatically (Zigbee › Network backups): the gateway
  restarts and saves the C6 network data before the role starts again (~25 s until the router is
  back in its network). Automatic daily or weekly (default) between 02:00 and 03:00 UTC; as
  coordinator only while no host is connected, so Zigbee2MQTT never loses its adapter unasked.
  Backups show when they were taken (`saved` in `/api/status` c6.snapshots; `POST
  /api/c6/zigbee-snapshot`, `zigbee_backup_days` in `/api/settings`). Tests TC26, UI21.
- ZHA checked: zigpy-zboss 2.0.5 forms a network on the coordinator role, restarts on it and opens
  permit join, but ZHA does not include zigpy-zboss and it needs zigpy < 2 (Home Assistant runs 2.x),
  so Zigbee2MQTT stays the only host (docs/home-assistant.md, `tests/zha/zigpy_zboss_check.py`).
- Online update of the gateway image from a manifest URL (System › Firmware, HTTP or HTTPS),
  checked every 6 hours and offered to Home Assistant as an MQTT update entity that can install it.
  The image goes to the other gateway slot (never the ESPHome slot) and is checked for the p4gw
  project before that slot is erased. `scripts/make_update_manifest.sh` writes the manifest
  (`/api/update/{check,install}`, `update_url` in `/api/settings`). Tests TC27, UI22.
- CI: GitHub Actions run `scripts/ci_checks.sh` (unit test, Python and web UI syntax, icon block) and
  `scripts/ci_build.sh` (C6 firmwares, gateway, c6fw.bin in `espressif/idf:v5.5.5`). The gateway
  build regenerates its sdkconfig when `sdkconfig.defaults` changed.
- Learned sounds (sound phase 4, Sound › Learned sounds): samples recorded with the microphone or
  uploaded as WAV are kept in the `sounds` partition (0.96 s audio and the YAMNet embedding each,
  44 slots). Profiles of type "any sound" (cosine similarity to the mean embedding) or "beep or
  tone" (energy at the dominant frequency), background recordings calibrate a suggested threshold,
  a test recording shows the scores. Sound events take a `profile` instead of classes and reach
  Home Assistant like the others (`/api/sound/learn*`). Tests TC28, UI23.
- MQTT connection options (Home Assistant › MQTT): `mqtts://` now works (it had no certificate
  verification set up): public CA bundle or an own CA / self-signed certificate, optional server name
  check; MQTT over WebSocket (`ws://`, `wss://`); client ID, keepalive, base topic, state interval and
  Home Assistant discovery on/off (off removes the entities). Discovery is sent again when Home
  Assistant comes online (`<prefix>/status`). TLS errors name the cause. Tests TC29, UI24.
- Log viewer (System › Diagnostics): every line carries the time it was logged (the gateway stamps
  each line in `/api/logs` with unix ms, or `+<ms since boot>` before the clock is set), shown in the
  browser's local time; OpenThread and continuation lines now have a time too. Filters by source
  (system, network/MQTT/web, Thread, Zigbee, sound, C6 flashing), level and tag (a click on a tag
  shows only that tag), search with highlighting or a `/regular expression/`. Downloads (also the log
  before the last restart) have the full local date and time. Test UI25.
- Menu: Zigbee › Network backups is closed outside a Zigbee role like Information and Diagnostics
  (backing up and the automatic backup work only there). Network and devices stays open: it shows
  Zigbee2MQTT over MQTT whatever the gateway's role. Tests UI16, UI21.

## v1.4 (2026-10-29)

- C6 flashing: a dialog locks the web UI from the restart until the role runs, with the stage,
  a smooth progress bar and the remaining time (`elapsed_s` in `/api/status` c6.flash). The
  gateway waits for its IP address before flashing and the page polls with a short timeout while
  the board restarts, so the progress shows from the first percent (it used to appear around 50 %:
  the flash ran before the network was up). Sound detection stays on: the write is limited by the
  460800 Bd UART (~42 kB/s), not by the CPU. Test UI11.
- Menu: Thread pages and Zigbee Information/Diagnostics open only in their role; Zigbee network
  and backups keep working in any role. A link to a closed page opens the first one that works.
  Test UI16.
- Thread and Matter logos (selfh.st/icons, CC BY 4.0) for the Thread section, the Thread roles and
  the Matter Server.
- Network maps: zoom with the wheel or +/−, drag to move, home for the whole map; L (or the
  button) arranges the nodes by signal strength (stress majorization over the link quality, a
  stronger link is a shorter edge) and back to the ring. Test UI17.
- Thread and Zigbee maps: a static legend (line colours, node kinds) in the top right corner of the
  map, styled like the details box; beside the map when the window is narrow, below it on a phone.
- Info tips fixed where columns share a name but not a meaning: Zigbee device table (model, type,
  state), Thread device table (role, firmware), Zigbee2MQTT instance (base topic, devices), sound
  event and log tables, Wi-Fi channel and network type in the scan, router link in Thread topology;
  the Zigbee router Ext PAN ID got its tip. Test UI15.

## v1.3 (2026-10-28)

- Thread network map (Thread › Topology): routers with link quality between them, end devices,
  leader and border routers, vendor/model where devices report it (OpenThread mesh diagnostics,
  `/api/thread/topology`). Test TC21.
- Map devices are identified through the Matter Server of Home Assistant (WebSocket, port 5580,
  set under Thread › Topology): name, vendor, product, device type, Matter node, firmware, serial
  number, matched by Thread MAC or IPv6 address. Formatted details on hover and a device table.
- Home Assistant is found over mDNS (`/api/ha/discover`); the Matter Server setting moved to
  System › Settings and defaults to the add-on on that host. The header shows who uses the
  gateway: Zigbee2MQTT connected or not, for Thread the last OTBR REST contact (kept across
  restarts). Test UI10.
- C6 flashing progress in the header on every page (pill with percentage, strip along the
  header) instead of a banner on some pages. Test UI11.
- Zigbee › Network and devices through Zigbee2MQTT on the gateway's MQTT broker (found
  automatically, the instance using this gateway preferred): network map with LQI, all devices with
  model, power, LQI, last seen and state, permit join (whole network or via a router), rename,
  remove, interview, OTA check and update. Works in every role. MQTT messages of any size; JSON in
  PSRAM. Tests TC22, UI12.
- C6 transmit power (-15..20 dBm, kept across restarts and role switches): Thread › Settings for
  both Thread roles (`/api/thread/txpower`), Zigbee › Information and the gateway's row in
  Zigbee › Network and devices for the Zigbee router. The router firmware (1.2.0) exposes it as
  Analog Output PresentValue, so Zigbee2MQTT can set it too (`tools/z2m/p4gw_router.mjs` names it
  `tx_power`). Test UI13.
- Identify: a blinking icon in the header for at least 10 s, from the Identify button
  (`/api/identify`), the Identify button of any device in Zigbee › Network and devices, or the
  Zigbee Identify cluster of the router (Zigbee2MQTT, Home Assistant). The board has no controllable
  LED.
- Map details box in the top left corner of the map for every device (Thread and Zigbee map),
  no longer next to the pointer over the links.
- Thread map: hovering a device highlights its links, as in the Zigbee map. The device lists under
  the maps no longer open the details box (map only). Test UI14.
- Fix: the Zigbee2MQTT device list sometimes did not arrive after a restart (retained message
  missed); the gateway subscribes again when it is still missing after 30 s.
- Fix: the Thread router showed the phase "joining" for good; it shows "running" once attached.
- Fix: a page redrawn on every status poll no longer drops the value being typed into its field.
- Fix: Czech labels of the Topology, Live and Events menu items and of the C6 tile were lost in
  v1.2; UI06 now checks that every menu item is translated.

## v1.2 (2026-10-28)

- Web UI redesign: left menu with sub pages (Information / Diagnostics / Settings pattern, Tools
  merged into System), two tiles per row with one label column width, roles grouped per function
  (Thread, Zigbee, ESPHome), deep links `#section/page`, drawer menu on phones, wider tiles, Material
  Design Icons embedded as SVG (`tools/web/gen_icons.mjs`), explanation tooltips (ⓘ) for values,
  network devices, roles and actions in Czech and English. Browser test UI08.
- `tools/matter_watch.py`: watches Matter attribute changes through Home Assistant.
- Network surroundings (System): quick 802.15.4 scan in the Thread roles and a full scan with
  Wi-Fi through a new C6 scanner image; 2.4 GHz spectrum, channel ratings and recommendations for
  new Thread, Zigbee and Wi-Fi networks. SNTP for scan times. Tests TC20, UI09.
- C6 telemetry (temperature, free RAM, uptime, reset reason) from the Thread RCP, the Zigbee
  coordinator and the Zigbee router, each over its own link; Zigbee2MQTT never sees it. Patched
  ESP-IDF openthread component, ZBOSS frame filter with host unit test, test TC19.
- Fixes: a recovery restart after an RCP failure no longer resets the boot loop guard (a C6 that
  keeps failing reflashed endlessly), the web server stays on core 0 (an upload with sound
  recognition on could trip the task watchdog), patch changes now reconfigure the build.
- Detailed hardware info: P4 CPU load per core, temperature min/max, flash/PSRAM/NVS usage, MAC,
  build fingerprint; C6 MAC, chip revision, flash size and flash count (read from its bootloader
  at the next flashing).
- Web UI in Czech and English (picked from the browser language, switch in the header) and a
  light / dark / system theme switch. Browser tests UI06–UI07.
- Household preset: clap, repeated clapping and finger snap events.
- Sound events have an optional Czech name (`name_cs`): shown by the Czech web UI, sent to Home
  Assistant as an event attribute; entity names and ids stay English.

## v1.1 (2026-10-28)

- Sound recognition with the onboard microphone: YAMNet int8 on the P4 (ESP-DL, about 60 ms per
  0.96 s of audio), off by default, Sound tab in the web UI.
- Personalised sound events (presets, editor, ignored sounds, detection log) sent to Home Assistant
  as MQTT event entity, binary sensor and device trigger.
- Flash layout v2 (model and sounds partitions, compressed C6 bundles); `flash_p4.sh` keeps NVS.
- Fixes: border router keeps its own Thread network, restarts confirm a new image, uploads keep the
  watchdog fed, copy buttons work on plain HTTP.
- Browser UI tests (UI01–UI05) and hardware tests TC14–TC18.

## v1.0

- Multi-role gateway: Thread border router/router, Zigbee coordinator/router, ESPHome BLE proxy.
- C6 flashing over JP1 with Zigbee network snapshots, web UI, MQTT discovery, A/B OTA with rollback.
- Hardware tests TC01–TC13.
