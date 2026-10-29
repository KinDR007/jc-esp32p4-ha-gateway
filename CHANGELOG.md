# Changelog

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
