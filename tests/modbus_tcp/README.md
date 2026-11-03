# Modbus TCP simulator: Huawei SUN2000 PV inverter

A Modbus TCP slave on your computer that behaves like a Huawei SUN2000-10KTL-M1 hybrid inverter with a
LUNA2000 battery (10 kWh) and a power meter, for trying the gateway's Modbus TCP devices without
hardware. Python 3, no third party modules.

| File | |
|---|---|
| `huawei_sun2000_sim.py` | the simulator |
| `huawei_sun2000.json` | the gateway template for the inverter (import as an own template) |
| `test_sim.py` | checks of the simulator against the template (run by `scripts/ci_checks.sh`) |

## Data

A simulated summer day in central Europe: PV power follows the sun (6:00-20:00) with drifting clouds,
the household load wanders around 600 W with spikes of 2 kW, the battery charges from the surplus and
covers deficits (5-100 %), the meter shows the rest (+ export, - import). Energy counters count up, the
daily yield and peak power start again at midnight. Status: on-grid, on-grid: power limited, standby: no
irradiation, shutdown: command.

Registers are the inverter's (holding registers, function 3, big endian, scaled integers): see
`REGISTERS` in the simulator and the template's `source`. Like the inverter, it answers exception 2 on
undefined addresses and exception 3 on more than 125 registers, and only its unit id (default 1).

Writes (functions 6 and 16): 40125 active power limit (% × 10, 0-1000), 40200 startup, 40201 shutdown.

## Use

```sh
python3 tests/modbus_tcp/huawei_sun2000_sim.py -v                          # real time, port 1502
python3 tests/modbus_tcp/huawei_sun2000_sim.py --speed 60 --start 05:30 -v # a day in 24 minutes from dawn
```

Options: `--port`, `--unit`, `--speed` (simulated seconds per second), `--start HH:MM`, `--seed`
(repeatable data), `-v` (state every 10 s). The computer's firewall must let the gateway connect.

On the gateway, System › RS485:

1. Templates › Import: `huawei_sun2000.json`, then save it in the editor.
2. Add a device: template *Huawei SUN2000*, Modbus TCP, host = this computer's IP address, port 1502,
   unit 1, Home Assistant: integration or MQTT.
3. The values appear within one read interval; *Active power limit*, *Start* and *Shut down* write to
   the simulator (it prints each write).

Or over the API (`$TOKEN` an API token with edit rights on System › RS485):

```sh
curl -X PUT -H "Authorization: Bearer $TOKEN" -H 'Content-Type: application/json' \
     --data @tests/modbus_tcp/huawei_sun2000.json http://<gateway>/api/modbus/template
```

## Another device

Copy the simulator and change `REGISTERS` (address, type, gain, quantity), `WRITES` and `Model.step()`
(how the quantities change); write a template for the gateway with the same addresses and check both
with a copy of `test_sim.py`. Devices with float or low-word-first registers need their encoding in
`words()`. For playing any template with fixed values there is `tools/modbus/sim_tcp.py`.

## The template on a real inverter

The register map follows Huawei's "Solar Inverter Modbus Interface Definitions" as implemented in
[huawei-solar-lib](https://github.com/wlcrs/huawei-solar-lib) (`registers.py`); it is not yet tested on
a real inverter. Modbus TCP must be enabled in the inverter (FusionSolar / SUN2000 app); the SDongle and
the inverter's WLAN/FE answer on port 502 (some firmware 6607), unit 1. Meter and battery values need a
meter and a battery. Huawei devices are slow: the template waits 100 ms between requests and 2 s for an
answer.
