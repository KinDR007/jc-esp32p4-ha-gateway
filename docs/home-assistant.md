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

HA discovers the node (ESPHome integration); enter the API key from
`firmware/esphome/secrets.yaml`. Return to a gateway role with *Gateway role* + *Return to gateway*.

## MQTT (optional)

Broker in the gateway UI (Home Assistant tab). Entities via MQTT discovery: active role, role
select, maintenance reason, uptime, chip temperature, heap, Ethernet, IP, C6 firmware, Thread state,
Zigbee host connection, restart button, sound level (dBFS, when the microphone is on).
Base topic `p4gw/<hostname>/`.


Firmware update entity (`update.<host>_firmware`): installed and offered gateway version from the
online update manifest (System › Firmware › Online update), *Install* downloads it into the other
gateway slot and restarts. Without a manifest URL it always shows the installed version.

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
