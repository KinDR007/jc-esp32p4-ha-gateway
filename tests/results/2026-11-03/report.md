# Regression 2026-11-03 (v1.9-30, layout v4, radio slots with Wi-Fi uplink and ESPHome)

Board: radio A C6 Thread border router (MyHome284238473), radio B XIAO C5 ESPHome proxy 1.1.0,
user accounts on, MQTT to the HA broker with discovery groups `sound_triggers`.

`hw_test.py --token … --only` 38 cases (regression.json, regression.log), reruns after test fixes
(regression_rerun.log):

| Result | Cases |
|---|---|
| PASS | TC04–08, TC10–12, TC14, TC16, TC17, TC19–22, TC24, TC25, TC27, TC28, TC31–37, TC40–43, TC45; after the test fixes TC09, TC30, TC39 |
| expected FAIL | TC01–03: first start of a fresh board (this one is set up) |
| known FAIL | TC26: the Zigbee router of radio A has no network (TODO) |
| not run | TC13 (needs the radio package of a release), TC23 (router network, as TC26), TC15/TC18 (--play), TC29 (Docker broker), TC44 (Wi-Fi interface of the host); TC46 ran separately against a local mosquitto |

Test fixes (no firmware change):

- TC09: the proxy has no web server, so Improv names no URL; `provisioned` is the join.
- TC30: the WebSocket check without credentials no longer sends the run's token; on a gateway with
  accounts the test admin sets its own password first and the run's token deletes it (it was left
  behind once, deleted by hand).
- TC39: waits for the border router to attach again after the restart (detached for a few seconds).
- UI22 puts the update URL back (it switched online updates off).
