# Changelog

## Unreleased

- Web UI: releases for this gateway as expandable rows (version, date, C6 package, install buttons)
  with the release notes formatted (lists, bold, code); the list carries the whole notes, notes cut at
  1000 characters end with "…".

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
