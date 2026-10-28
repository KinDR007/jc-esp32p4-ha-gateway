# Changelog

## Unreleased

- Web UI in Czech and English (picked from the browser language, switch in the header) and a
  light / dark / system theme switch. Browser tests UI06–UI07.

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
