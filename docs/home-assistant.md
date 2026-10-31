# Home Assistant setup

The web UI tab **Home Assistant** shows these steps with the gateway's current address.
`<ip>` is the gateway address (or `p4gw-xxxxxx.local`).

## Thread border router

1. **Settings → Devices & services → Add integration → OpenThread Border Router**, URL
   `http://<ip>:8081` (no trailing slash). Port 80 is the gateway UI, 8081 the OTBR REST API.
2. **Settings → Devices & services → Thread → Configure**: mark the network as preferred.
3. To join an existing Thread network instead of creating a second one: copy the active dataset
   TLVs of the preferred network in HA and apply them in the gateway UI (**Thread → Apply dataset**).
   With another border router in the network, SRP server and NAT64 stay idle on this gateway
   (only one is active per network); that is expected.
4. Send credentials to the phone with the Companion app (Android: Settings → Companion app →
   Troubleshooting → Sync Thread credentials; iOS: Settings → Companion App → Debugging).
5. Matter: Matter Server add-on / integration, then Add device → Matter.
6. Network: IPv6 on the HA host and LAN, router advertisements accepted (Docker or VM host:
   `net.ipv6.conf.<if>.accept_ra=2`, `accept_ra_rt_info_max_plen=64`), no multicast filtering
   between HA and the gateway (IGMP/MLD snooping).

Verified 2026-10-28: integration added, gateway joined the existing network as a router.

## Thread router

Apply the dataset TLVs of the HA preferred network (see step 3 above). Nothing to add in HA.

## Zigbee coordinator

Zigbee2MQTT (recommended):

```yaml
serial:
  port: tcp://<ip>:6638
  adapter: zboss
advanced:
  transmit_power: 20
```

One TCP client at a time; a new connection replaces the old one.

ZHA (built into Home Assistant) cannot use this coordinator today (2026-10-29). The protocol fits:
zigpy-zboss 2.0.5 (the zigpy library for ZBOSS NCPs) formed a network on the gateway, started again
from the stored network in 0.9 s and opened permit join (`tests/zha/zigpy_zboss_check.py`; pairing a
device was not tried). But ZHA does not ship zigpy-zboss (not among the radio libraries of the `zha`
package) and zigpy-zboss requires `zigpy<2`, while Home Assistant runs zigpy 2.x, so not even a
copied ZHA custom component can load it without porting zigpy-zboss first. Use Zigbee2MQTT.
The gateway keeps a copy of the coordinator NVRAM whenever it leaves the role (Zigbee tab).

## Zigbee router

Permit join in the Zigbee host, then **Zigbee → Join network (steer)** in the gateway UI.

Transmit power (-15..20 dBm) is set under Zigbee › Information or with the power button of the
gateway's row in Zigbee › Network and devices. Zigbee2MQTT shows the router with an automatically
generated definition (`identify`, `analog_output` = transmit power in dBm). For a named
`tx_power` setting with its range and unit, copy `tools/z2m/p4gw_router.mjs` to
`<zigbee2mqtt data>/external_converters/` and restart Zigbee2MQTT. Identify from Zigbee2MQTT or
Home Assistant blinks the icon in the gateway's web UI header for at least 10 s (the board has no
LED the firmware can drive).

## ESPHome BLE proxy

A role of a radio (A or B, [dual-radio.md](dual-radio.md)): the radio runs the ESPHome Bluetooth
proxy of the release (`firmware/esphome/radio_proxy`) on its own over Wi-Fi, the gateway keeps
working with the other radio.

1. Gateway UI, Home Assistant › ESPHome: enter the Wi-Fi (SSID, password) and save.
2. Role: pick *ESPHome BLE proxy* for a radio. The gateway flashes ESPHome from the release into it
   and sets the Wi-Fi over Improv on the radio's UART; the page shows the Improv state and the URL.
3. HA discovers the device (ESPHome integration, `p4gw-ble-proxy-xxxxxx`) and sets its API
   encryption key when it adopts it. The image holds no key and offers no access point.

## P4 Gateway integration (native, without MQTT)

Like SMLIGHT SLZB devices, the gateway has its own Home Assistant integration
([custom_components/p4gw](../custom_components/p4gw)). It talks to the gateway directly: no broker,
no add-on.

Install (once), one click each:

[![Open your Home Assistant instance and open this repository inside HACS](https://my.home-assistant.io/badges/hacs_repository.svg)](https://my.home-assistant.io/redirect/hacs_repository/?owner=KinDR007&repository=jc-esp32p4-ha-gateway&category=integration)
[![Open your Home Assistant instance and start setting up the P4 Gateway integration](https://my.home-assistant.io/badges/config_flow_start.svg)](https://my.home-assistant.io/redirect/config_flow_start/?domain=p4gw)

1. **Open HACS repository on My** adds this repository to HACS; *Download*, then restart Home Assistant.
   Without HACS: copy `custom_components/p4gw` into `/config/custom_components/` (e.g. over the SSH
   add-on). HACS installs only from a public repository.
2. **Add integration** starts the setup (or use the discovered gateway, below).

The web UI shows the same buttons under Home Assistant › Guide.

The gateway announces itself over mDNS (`_p4gw._tcp`, TXT `mac`, `version`),
so it appears under Settings › Devices › **Discovered**; or add it by hand (Add integration ›
P4 Gateway, address). Once the gateway has user accounts, the flow asks for an **API token**: create
one in the gateway web UI under System › User management › API tokens (role admin, or user when the
integration should not restart the gateway or install updates) and paste it. The token does not
depend on any user, so two-factor sign-in of the admins does not matter. Entries set up with the
admin password of older firmware keep it while the account has no 2FA; otherwise re-authentication
asks for a token, which then replaces the password.

How it works: state is read from the REST API every 30 s (`/api/status`, `/api/sound/status`,
`/api/sound/events`); sound detections, sound event states and update progress are pushed at once
over a WebSocket (`/api/ws`, same password; the gateway sends there everything it publishes to
MQTT, with or without a broker). The push connection reconnects by itself after 10 s.

| Entity | Notes |
|---|---|
| Active role, Thread state | sensors |
| Noise, Noise max | sound pressure in dB(A) over the last minute (LAeq, LAmax); calibration in the gateway UI, Sound › Live ([details](sound-detection.md)) |
| Role (select) | role of radio A; switching restarts the gateway |
| Radio B role (select) | with a radio B on JP1: its role, `none` switches it off; restarts the gateway |
| Sound recognition (switch) | microphone and model on/off |
| `event.<host>_<event>`, `binary_sensor.<host>_<event>_active` | one pair per enabled sound event, attributes `class`, `score`, `level_db`, `name_cs`; events added on the gateway appear without a reload |
| Firmware (update) | install with progress, from the online update manifest |
| C6 radio firmware (update) | the package of all C6 firmwares from the same manifest; versions are the first 8 hex digits of its sha256, the release summary lists the firmwares that change; install downloads it into the gateway's RAM, stores it and restarts the gateway, which then updates the C6 |
| Restart, Identify, Back up Zigbee network, Check for update (buttons) | Zigbee backup only in a Zigbee role |
| Notification (event) | every notification of the gateway: event type `info`, `warning` or `critical`, attributes `topic` (security, system, network, sound), `text`, `time`; for automations, e.g. forwarding to the phone |
| Ethernet, Zigbee host connected, Push connection | binary sensors (push connection disabled by default) |
| Chip and C6 temperature, Started, IP, C6 firmware, Maintenance reason, Sound level (dBFS), heap, CPU load | diagnostic (heap and CPU disabled by default) |

Notifications: the gateway sends them on the push connection and keeps the last 32 (`/api/notify`), so
those sent while Home Assistant was not connected (a restart of the gateway, a crash) arrive after the
reconnect. Integration options (Configure) choose per topic from which level a persistent notification
appears: off, critical only, warnings and critical, all. Defaults: security, system and network from
warnings, sounds off (they have their own event entities).

The integration and MQTT discovery can run side by side, but every entity would then exist twice.
With the integration in use, switch off **Home Assistant discovery** in the gateway MQTT settings
(MQTT then keeps only the plain topics, e.g. for Zigbee2MQTT network view).

Tests: `tests/ha_integration` (pytest-homeassistant-custom-component against a fake gateway,
also in CI), hardware test TC30 (mDNS, WebSocket push, account and token on the handshake).

## MQTT (optional)

Broker in the gateway UI (Home Assistant tab). Entities via MQTT discovery: active role, role
select, maintenance reason, uptime, chip temperature, heap, Ethernet, IP, C6 firmware, Thread state,
Zigbee host connection, restart button, noise and noise max (dB(A)) and sound level (dBFS) when the
microphone is on.
Base topic `p4gw/<hostname>/`.

Connection options (Home Assistant › MQTT):

| Option | Values | Default |
|---|---|---|
| Broker URI | `mqtt://`, `mqtts://` (TLS), `ws://`, `wss://` (MQTT over WebSocket, Mosquitto add-on port 1884) | — |
| Server certificate (TLS) | public CA bundle, or own CA / self-signed broker certificate (PEM, max 3999 characters) | public CA |
| Do not check the server name | the certificate is still verified against the CA; for a broker reached by IP (also disables SNI) | off |
| Client ID | unique per broker | hostname |
| Keepalive | 10–240 s | 30 s |
| Base topic | no `#`, `+`, spaces or outer slashes | `p4gw/<hostname>` |
| State interval | 10–3600 s | 30 s |
| Discovery prefix | as configured in the HA MQTT integration | `homeassistant` |
| Home Assistant discovery | off: plain topics only, the entities are removed from HA | on |

TLS always verifies the broker; there is no "insecure" mode (it would weaken the TLS of online
updates as well). Connection errors name the cause, e.g. `TLS: server certificate not accepted (CA or
server name)`. When Home Assistant publishes `online` on `<prefix>/status` (its start), the gateway
sends its discovery again, so entities come back even with a broker that does not keep retained
messages.


Firmware update entity (`update.<host>_firmware`): installed and offered gateway version from the
online update manifest (System › Firmware › Settings), *Install* downloads it into the other
gateway slot and restarts. Without a manifest URL it always shows the installed version.
An older release that runs on the board (attribute `available_versions`) is installed with
`update.install` and `version: v1.6` (rollback); the *C6 radio firmware* entity takes the package id
(attribute `available_versions`: id → release) or the release version.

## Sound events (MQTT)

Configured in the gateway UI, Sound tab (presets, per-event classes and timing, ignored sounds).
Every enabled event creates in Home Assistant (`<host>` = hostname with `_`):

| Entity | Use |
|---|---|
| `event.<host>_snd_<id>` | fires once per occurrence; attributes `class`, `score`, `level_db` and `name_cs` (Czech name of the event, when set) |
| `binary_sensor.<host>_snd_<id>_active` | on while the sound lasts (plus hold time); conditions, `for:` triggers |
| device trigger "sound <name>" | Automations → trigger → Device → P4 gateway |

The Sound tab copies a ready automation for each event ("HA automation"). Example:

```yaml
triggers:
  - trigger: state
    entity_id: event.p4gw_e30a08_snd_doorbell
actions:
  - action: notify.notify
    data:
      message: "Doorbell ({{ trigger.to_state.attributes.score }})"
```

Deleting or disabling an event removes its entities from Home Assistant.

## Matter attribute watcher

`tools/matter_watch.py` logs attribute updates and events of one Matter node (standard library
only), highlights one cluster and polls its attributes. Needs port 5580 exposed in the Matter
Server add-on (Configuration → Network). Reconnects after a Matter Server restart and logs what
changed in the meantime.

```sh
python3 tools/matter_watch.py --url ws://homeassistant.local:5580/ws --node 39 \
    --cluster 0x117CFC01 --interval 300 --log liljebagge.jsonl
```

## Thread network map: device names

Thread › Topology shows the mesh. To see which device is which, the map reads the Matter Server
add-on over its WebSocket (port 5580, has to be exposed in the add-on: Configuration → Network).
Without a setting the gateway uses the Home Assistant host it finds over mDNS
(`ws://<HA IP>:5580/ws`); System › Settings shows the instance found, lets you set another address
and test the connection. The browser reads the nodes from it and matches them with
the map by Thread MAC or IPv6 address; Home Assistant shows the same node ID in the device info of
the Matter integration. Devices that are not Matter (other border routers) show what they report
over Thread diagnostics, often only the vendor.

The header shows who uses the gateway: in the Zigbee coordinator role whether Zigbee2MQTT is
connected (it keeps a TCP session); in the Thread border router role when Home Assistant last
called the OTBR REST API. Home Assistant does that only at its start, in the Thread settings and on
dataset changes, so the pill tells the last contact, not a live connection.

## Zigbee network and devices

Zigbee › Network and devices reads Zigbee2MQTT over the MQTT broker the gateway is connected to
(Home Assistant › MQTT): every instance announces itself on `<base>/bridge/info`; the one whose
serial port is this gateway is picked first, others can be chosen. It works in every role, also
for a Zigbee network on another adapter.

- Devices: name, vendor and model, type, power, LQI, last seen (needs `last_seen` enabled in
  Zigbee2MQTT), availability, update state.
- Map: on request; Zigbee2MQTT asks every router for its neighbours, which takes several minutes in
  a network with dozens of routers.
- Operations go to Zigbee2MQTT (`bridge/request/...`) and show its answer: permit join for 254 s
  (whole network or via a router), rename (Home Assistant entities too), remove (optionally
  forced), interview, OTA check and update, Identify.
