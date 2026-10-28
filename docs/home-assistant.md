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

ZHA: only with the experimental `zha-zboss-esp` custom component (HACS), radio type ZBOSS,
path `socket://<ip>:6638`. One TCP client at a time; a new connection replaces the old one.
The gateway keeps a copy of the coordinator NVRAM whenever it leaves the role (Zigbee tab).

## Zigbee router

Permit join in the Zigbee host, then **Zigbee → Join network (steer)** in the gateway UI.

## ESPHome BLE proxy

HA discovers the node (ESPHome integration); enter the API key from
`firmware/esphome/secrets.yaml`. Return to a gateway role with *Gateway role* + *Return to gateway*.

## MQTT (optional)

Broker in the gateway UI (Home Assistant tab). Entities via MQTT discovery: active role, role
select, maintenance reason, uptime, chip temperature, heap, Ethernet, IP, C6 firmware, Thread state,
Zigbee host connection, restart button. Base topic `p4gw/<hostname>/`.
