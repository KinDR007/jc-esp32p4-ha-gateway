# Modbus (RS485, Modbus TCP, GoodWe UDP)

The gateway reads and writes Modbus devices, on the board's RS485 port and over the network
(Modbus TCP, or Modbus RTU over UDP for GoodWe Wi-Fi modules), and shows each one in Home Assistant as its own device. What to read from a device
and how to show it is a **template** (JSON): built-in ones come with the firmware, own ones are
added in the web UI. Web UI: System › RS485 and System › Modbus TCP search. API: [api.md](api.md#modbus-rs485-and-tcp). Code:
`firmware/gateway/components/modbus`.

## Wiring

| | |
|---|---|
| Connector | **J4**: A and B of the bus (twisted pair); connect GND of the devices to the board's GND |
| Transceiver | MAX485 on the board, direction switched automatically from the TX line (74LVC1G132 and a transistor), TVS diodes and 0.1 A fuses on A and B |
| P4 | UART3, TX GPIO26, RX GPIO27 (`CONFIG_GW_RS485_*`) |
| Bus | up to 32 devices, 120 Ω termination at both ends of a long bus, one baud rate and framing for all devices |

The bus settings (baud rate, parity, stop bits, timeout) apply to all RS485 devices; a template
shows its device's defaults and the page offers to take them. Modbus TCP devices need only host,
port (502) and unit id; GoodWe UDP devices the same with port 8899.

## Devices

Per device: template, name (default the model, up to 32 characters), connection (RS485 address
1-247, TCP host, port and unit 0-255, or GoodWe UDP host, port 8899 and unit), read interval (0 = the template's), how it is shown in
Home Assistant (integration, MQTT, both, none), enabled. Up to 16 devices.

- The task reads the entities due in a poll in as few requests as the template allows (adjacent
  registers, up to `max_regs` per request, gaps of up to `max_gap` registers). When a device
  refuses a combined read (exception), its entities are read one by one from then on; an entity
  the device refuses alone (illegal address or function) is left out.
- No answer three times in a row: offline, values gone (Home Assistant: unavailable), tried again
  every 30 s.
- Entities with `"every": 0` (serial number, settings) are read once after the device answers, and
  again after a write.
- Writes come from Home Assistant (number, switch, select, button), from MQTT command topics and
  from the page; the entity is read again right after.
- **Find devices** tries every address (and chosen baud rates) with a read of the template's first
  register; an exception counts as an answer. RS485 devices are not polled meanwhile.
- **Manual request** reads or writes registers and coils of any address and shows registers as
  u16, s16, u32 and float32 in both word orders, for writing a template.
- Telegram: `/modbus` lists the devices with their measurements.

## Modbus TCP search in a network

System › Modbus TCP search (`/api/modbus/scan`, `mb_scan.c`) looks for Modbus TCP devices in an
address range (a.b.c.d/n, up to /22 = 1024 addresses; default the network of the interface carrying
the traffic):

1. Connects to the chosen ports (default 502; up to 4, e.g. 502, 503, 1502) of every address, 4 at a
   time with 300 ms each. A /24 with one port takes about 23 s on the LAN, 80 s with four ports in a
   network without answers.
2. On each open port reads holding register 0 of the chosen units (default 1, 247, 255). Data or any
   exception (2 illegal address, 10/11 of a gateway without that unit) means a Modbus server; a port that
   closes or answers something else (a web server) is not listed.
3. Recognises known devices by a serial number (at least 8 characters, so a device answering every unit
   id the same is not taken for one): Victron GX (unit 100, register 800) and its VE.Bus inverter/charger
   (units 228, 227, 242, 246, register 3), GoodWe ET/EH/BT (unit 247, 35003) and DT/XS/D-NS (unit 127 or
   247, 30004). A recognised device with a template is added with one click.
4. With "GoodWe dongles on UDP 8899" (default on) also sends the GoodWe discovery broadcast (port 48899)
   and a Modbus RTU read on UDP 8899 to every address (unit 127 register 30001, unit 247 register 35000).
   Older GoodWe Wi-Fi dongles speak Modbus only there; a module that answers is listed with its unit, and
   unit 127 (DT family: XS, MS, D-NS) is added with one click on the GoodWe UDP connection and the
   `goodwe_xs` template. A WiFi/LAN Kit-20 or the LAN+WiFi kit 2.0 offers Modbus TCP 502 instead.

The search runs in its own task beside the polling, only reads, and is started by hand. Another network
is reached only through a route of the gateway's router (broadcasts do not cross it); a PV inverter's
dongle is often powered from the panels and off at night.

## GoodWe UDP (connection `udp`)

GoodWe Wi-Fi modules (e.g. `Solar-WiFi…` of a GW3000-XS) answer Modbus only on UDP 8899: the request is a
plain Modbus RTU frame (unit, PDU, CRC), the answer is `AA 55` + an RTU frame, a long one possibly split into
several datagrams (`mb_udp_request` in `mb_bus.c` joins them up to the length the RTU header announces).
The socket is connected to the module, so only its datagrams arrive; late answers of an earlier request are
dropped before the next one. Same timeouts, offline handling and writes as Modbus TCP.

GW3000-XS checked on 2026-11-04 from a computer (the gateway has no route to that network yet): unit 127
answers 30001 (model `GW3000-XS`, serial at 30004) and 30100+ (PV 230.5 V 1.0 A, grid 233.3 V 1.7 A
50.01 Hz, about 280 W); unit 247 and all TCP ports stay silent. The module is powered from the panels and
does not answer at night. Through `tools/modbus/udp_relay.py` on that computer the gateway read all values (power 585 W, grid
234.9 V 2.4 A, energy total 15145.4 kWh, serial, model); the total input power register (30137) reads
0xFFFF on the XS and is left out of the template.

## Modbus TCP server

System › Modbus TCP server (`GET`/`PUT /api/modbus/server`, [api.md](api.md)) offers the devices the
gateway already talks to (RS485 RTU, Modbus TCP, GoodWe UDP) as Modbus TCP servers in the network, for
clients other than Home Assistant (Victron GX, SCADA, a PLC, a vendor app). Since v1.14.

- **Ports**: the shared port (default 502, can be switched off) routes by unit id, one unit id per
  device; an unknown unit id gets exception 10. A device may also have its own port where any unit id
  reaches it (clients fixed to unit 1 or 255). Not 80, 8081 or the Zigbee port.
- **Cache** (PSRAM, only the blocks actually read): registers the gateway polls anyway are fresh for the
  poll interval + 2 s; answers to clients (16 per device, an exception 2 too) serve the next clients for
  `cache_ms` (1500-5000 ms, default 2000). A request may span several blocks. A write (from a client, the
  API, MQTT or Home Assistant) drops every cached block it touches. `cache_ms` 0 turns the cache off for
  the device (registers that change when read: alarms cleared, event queues): every read goes to it.
- **Queue**: everything else goes to the device through the Modbus task, which owns the RS485 bus. It
  takes the waiting requests in order between two polls and between the blocks of a poll, so a client
  waits for one transaction at most, not for a whole poll (measured: 0.5 s instead of 1.6 s with blocks of
  300 ms). Two clients reading the same while it is on its way cost one transaction. At most `rate`
  requests per second per device go to it (1-50, default 5), else exception 6. A device off or offline
  answers exception 10 or 11 at once, without the bus; during an RS485 search RTU devices answer 6.
- **Writes** (functions 5, 6, 15, 16) only with `write`, and when `allow` lists addresses (up to 4),
  only from them; else exception 1. Modbus TCP has no sign-in.
- **Limits**: 16 devices, 8 ports (the shared one counts), 2 clients per port, 8 clients in all (more
  are closed at once and counted as `turned_away`), one request at a time per client, idle clients
  closed after 120 s. lwIP: 40 sockets, 32 active and 24 listening TCP connections (were 24/16/16).
- **State**: per port its clients, per device the requests answered from the cache, sent to the
  device, refused and failed.

The RS485 bus is the bottleneck, not the P4: reading 10 registers at 9600 Bd is about 35 ms on the wire
plus gaps and the device's answer time (20-100 ms for meters), 60-120 ms in all, so roughly 8-15
transactions per second for the gateway's own polling, writes and every TCP client together (15-25 at
19200 Bd); a silent device holds the bus for its timeout. Hence the cache and the per-device rate. One
task serves every socket with `select()`; frames are at most 260 bytes.

Measured 2026-11-06 with `tools/modbus/server_load.py` (8 clients on 502 and three own ports, three
SDM630 on the Modbus TCP simulator, poll 3 s, rate 50, cache 2000 ms, role Thread border router; every
read meant for the device asks another address, so the cache of answers does not catch it):

| Load | Answers/s | Cache p50 / p95 | Device p50 / p95 | `/api/status` p50 / max | Internal heap |
|---|---|---|---|---|---|
| idle (no client) | - | - | - | 62 / 142 ms | - |
| 8 clients, a request every 0.5 s each | 16 | 1.6 / 4.4 ms | 8 / 11 ms | 32 / 135 ms | -4 kB |
| 8 clients as fast as answered (two runs) | 1160-1220 | 1.3 / 2.7 ms | 24 / 60 ms (about 65/s per device, the rest exception 6) | 141 / 236-1400 ms | -10 kB |

No restart, the role and the device polling (every 3 s) went on in every run; at full load one sample of
`/api/status` took 1.4 s in one of the two runs and 4 of 11 500 device reads timed out (exception 11).
A device on RS485 is slower than the simulator: its requests take the bus times above.

## Emulation (virtual devices)

A virtual device shows the values of a device the gateway reads (its source, e.g. an Eastron SDM630 on
RS485) as a device of another template (e.g. `abb_b23`), for clients that accept only a certain meter (an
inverter's zero export, a Victron GX). System › RS485 › Add device, connection *Emulation*: the template it
plays and the device whose values it shows.

- Configuration: `"transport": "virtual"`, `"source": "<device id>"`, the template; no address. The source is
  a real device of the list (no chains). It is a device like the others: Modbus TCP server, MQTT, the
  integration. Read only: writes from the API, MQTT or a client are refused (the integration and MQTT
  discovery leave its writable entities out). Shown in Home Assistant next to its source, the same
  quantities appear twice; the page says so.
- Mapping, automatic, per target entity: the source entity of the same quantity (`q`, else the key; Eastron's
  `total_power`, `total_reactive_power`, `total_apparent_power`, `total_power_factor` and `serial_number` are
  `power`, `reactive_power`, `apparent_power`, `power_factor` and `serial`), units converted between W and kW,
  Wh and kWh, var and kvar, varh and kvarh; else a total as the sum of the three phases (power, reactive and
  apparent power, import and export energy, reactive energies), power as import minus export power, an L1
  value of a single phase source. Texts only from texts, options only from the same options. The API
  (`map`, `mapped`) and the opened row list where each value comes from. SDM630 as ABB B23: 29 of 30 (the
  B23's firmware version has none).
- Values follow the source: computed after each of its polls, offline with it (state and error say so).
- Modbus TCP server: every read is answered from the values, encoded as the target template says (type,
  word order, scale); no read reaches the source. An entity without a value reads the template's "no value"
  (`nan`; u64 all ones), else 0; registers no entity covers read `fill` (ABB 65535); `emulate` gives fixed
  values (ABB type "B23 112-100" at 0x8960, which a Victron GX checks). A value that does not fit the type
  (out of range) reads as no value. Writes answer exception 1, a virtual device whose source is offline
  exception 11.
- Not on RS485: the gateway is the bus master there (decided 2026-11-11).

## Home Assistant

Each device is a device of its own, connected via the gateway:

- **Integration** (P4 Gateway 1.6.0): entities from the template (sensor, binary sensor, number,
  switch, select, button), values pushed on every read (WebSocket topic `modbus/<id>`), writes
  through `/api/modbus/write`. The template is fetched once per revision.
- **MQTT** discovery group `modbus`: the same entities, state topic `<base>/modbus/<id>`,
  availability `<base>/modbus/<id>/availability` together with the gateway's, commands
  `<base>/modbus/<id>/<key>/set` (number: the value, switch: `ON`/`OFF`, select: the option text,
  button: `PRESS`).

Choose integration *or* MQTT per device, both would show every entity twice.

## Template format

```json
{
  "schema": 1,
  "id": "my_meter",
  "manufacturer": "Acme", "model": "M1", "category": "energy_meter",
  "notes": "Shown when adding a device.", "notes_cs": "Totéž česky.",
  "source": "https://... (where the register map comes from)",
  "serial": {"baud": 9600, "parity": "E", "stop": 1},
  "slave": 1, "timeout_ms": 500, "gap_ms": 50, "poll_s": 10, "max_regs": 40, "max_gap": 0,
  "entities": [
    {"key": "voltage", "name": "Voltage", "name_cs": "Napětí", "reg": "input", "addr": "0x0000",
     "type": "f32", "unit": "V", "class": "voltage", "state": "measurement", "precision": 1},
    {"key": "power", "name": "Power", "reg": "holding", "addr": 16, "type": "s32", "order": "CDAB",
     "scale": 0.1, "unit": "W", "class": "power", "state": "measurement"},
    {"key": "relay", "name": "Relay", "reg": "coil", "addr": 3, "write": {"kind": "switch"}},
    {"key": "mode", "name": "Mode", "reg": "holding", "addr": "0x0020", "map": {"0": "off", "1": "auto"},
     "write": {"kind": "select"}, "category": "config", "every": 0},
    {"key": "setpoint", "name": "Setpoint", "reg": "holding", "addr": "0x0021", "type": "s16", "scale": 0.1,
     "unit": "°C", "write": {"kind": "number", "min": 5, "max": 30, "step": 0.5}},
    {"key": "reset", "name": "Reset", "reg": "none", "write": {"kind": "button", "fc": 66}}
  ]
}
```

Device fields: `id` (a-z, 0-9, _; up to 31), `manufacturer`, `model` required; `serial` defaults
9600 8N1; `slave` the usual address; `timeout_ms` (50-5000, default 500) and `gap_ms` (pause
before each request, default 20) per request; `poll_s` (1-3600, default 10); `max_regs` registers
per request (1-125, default 40); `max_gap` unread registers a combined read may span (default 0;
only for devices that answer reads of undocumented registers); `fill` what registers no entity covers
read when another device plays this template (0-65535, default 0; ABB 65535).

Entity fields:

| Field | Values | Notes |
|---|---|---|
| `key` | a-z, 0-9, _ (up to 23) | stable: part of the Home Assistant ids |
| `q` | a-z, 0-9, _ (up to 23) | quantity for emulation when it differs from the key (Eastron `total_power`: `power`) |
| `name`, `name_cs` | up to 47 characters | `name_cs` used when Home Assistant / the page runs in Czech |
| `reg` | `coil`, `discrete`, `input`, `holding`, `none` | function 1, 2, 4, 3; `none` only for a button sending a device function |
| `addr` | 0-65535, number or `"0x..."` | protocol address (0 based): subtract 1 from 1-based tables, 30001/40001 from the "3xxxx/4xxxx" notation |
| `type` | `bool`, `u16`, `s16`, `u32`, `s32`, `u64`, `s64`, `f32`, `f64`, `str` | coils and discrete inputs are `bool`; `str` needs `count` (registers) |
| `order` | `ABCD` (default), `CDAB`, `BADC`, `DCBA` | how the bytes of a 32/64-bit value come over the wire: `CDAB` low word first (evcc `…s`, ESPHome `_R`, HA `swap: word`), `BADC` bytes swapped |
| `bit` | 0-15 | one bit of a 16-bit register (a bool) |
| `scale`, `offset` | numbers | value = raw × scale + offset |
| `nan` | raw value | means "no value" (e.g. ABB `0xffffffff`) |
| `precision` | 0-6 | decimals (default 2 for floats, else 0) |
| `unit`, `class`, `state` | Home Assistant unit, device class, state class | e.g. `kWh`, `energy`, `total_increasing` |
| `category` | `diagnostic`, `config` | entity category |
| `map` | `{"raw": "text"}`, up to 32 | shows a text (enum sensor), options of a select |
| `every` | 0-255 | read every n-th poll, 0 = once |
| `write.kind` | `number`, `switch`, `select`, `button` | only holding registers and coils can be written |
| `write.min`, `max`, `step` | numbers | number: required limits in shown units |
| `write.on`, `off` | raw values | switch on holding registers (default 1/0) |
| `write.value` | raw value | button |
| `emulate` | number, or text (`str`) | fixed value when another device plays this template (ABB type `B23 112-100`) |
| `write.fc` | 5, 6, 15, 16, or a device function | default: coil 5, one register 6, more 16; `16` for devices that take only function 16 (DDS238, Eastron); a device function (e.g. 66 = 0x42 PZEM energy reset) with `reg: none` |

A template is checked when saved; the error names the entity and field. Own templates (up to 16,
each up to 16 kB) are kept in NVS and are part of the settings backup; export and import as JSON
files on the page. A new template is easiest as a copy of a similar built-in one.

## Built-in templates

All register maps were researched from the manufacturers' documents and open source drivers, and
checked by a second, independent research (sources below). "Not confirmed" items were left out.

| Template | Device | Serial default | Notes |
|---|---|---|---|
| `eastron_sdm120` | Eastron SDM120-Modbus | 2400 8N1, address 1 | SDM120-M: 9600; no phase angle (not in Eastron's map) |
| `eastron_sdm230` | Eastron SDM230-Modbus | 9600 8N1 (manuals up to v1.6: 2400) | |
| `eastron_sdm630` | Eastron SDM630-Modbus V2 | 9600 8N1 | 80 registers per request; per-phase energies, THD left out |
| `eastron_sdm72d_v2` | Eastron SDM72D-M V2 | 9600 8N1 | 200 ms between requests (reboots on fast requests) |
| `eastron_sdm72d` | Eastron SDM72D-M (V1) | 9600 8N1 | total power and energies only (open source drivers; no Eastron map) |
| `orno_or_we_504` | ORNO OR-WE-504 | 9600 **8N1** | community code only: check the values against the display |
| `orno_or_we_514` | ORNO OR-WE-514 / 515 | 9600 **8E1** | from the ORNO table (reproduced by TapHome) |
| `orno_or_we_516` | ORNO OR-WE-516 / 517 | 9600 8E1 | power read in kW; import/export energy registers not confirmed, left out |
| `hiking_dds238_2` | Hiking DDS238-2 ZN/S | 9600 8N1 | writes only with function 16; reactive power signed. **DTS238-7: no map found, no template** |
| `chint_dtsu666` | Chint DTSU666 | 9600 8N1 | raw units of the manual (0.1 V, 0.001 A, 0.1 W) scaled; DTSU666-H differs |
| `chint_ddsu666` | Chint DDSU666 | 9600, 1 or 2 stop bits | power in kW (shown in W); export energy not documented |
| `peacefair_pzem016` | Peacefair PZEM-016 / 014 (AC) | 9600 8N1 | current, power, energy low word first; energy reset (0x42) |
| `peacefair_pzem017` | Peacefair PZEM-017 / 003 (DC) | 9600 **8N2** | shunt range select, alarm thresholds, energy reset |
| `victron_gx_system` | Victron GX (Cerbo, MultiPlus-II GX, Venus OS), system values | TCP, unit 100 | consumption, grid, generator, AC-coupled PV, battery V/A/W/SOC/state, DC PV, serial; L1 only |
| `victron_vebus` | Victron MultiPlus / Quattro via a GX | TCP, unit 228 (MultiPlus-II GX; 227 Cerbo, 242 Venus GX, 246 CCGX) | AC in/out V, A, Hz, W, battery, state, switch position, alarms; read only; L1 only |
| `abb_b21` | ABB B21 (EQ, single phase) | 19200 8E1, address 1 (third party; ABB gives no default) | voltage, current, power, reactive and apparent power, frequency, power factor, energies (u64, 0.01 kWh); serial, firmware, type |
| `abb_b23` | ABB B23 / B24 (EQ, three phase) | as B21 | per phase V, A, W, PF and import/export energy; B24 shows primary values (the meter applies the CT ratio) |
| `goodwe_xs` | GoodWe XS / MS / D-NS (single phase DT family) | UDP 8899 (Wi-Fi module) or TCP 502, unit 127 | power, PV1/PV2 V and A, grid V, A, Hz, energy today/total, temperature, work mode, error and warning codes, hours, serial, model; read only |

The templates are generated from tables in `tools/modbus/gen_sdm.py`, `tools/modbus/gen_other.py` and
`tools/modbus/gen_victron.py`, `tools/modbus/gen_goodwe.py`, `tools/modbus/gen_abb.py`.

### Findings worth knowing

- Eastron: all settings registers (address, baud, parity) are float32; reads must start at an even
  address and read pairs; undocumented registers answer 0, not an exception. Baud code 5 is 1200
  on SDM120/SDM72 but 115200 on SDM630 V2. The SDM230's default changed between manual versions
  (2400 → 9600). Meter code at 0xFC02: SDM120 and SDM120-M both 0x0020, SDM72D-M V2 0x0089.
- Word order differs between families: Eastron, Chint, ORNO, ABB big endian; Carlo Gavazzi EM340
  and Peacefair low word first.
- Victron GX (Venus OS): Modbus TCP must be enabled on the GX (Settings › Integrations › Modbus TCP
  server); since Venus 2.60 unit ids are assigned dynamically, the GX lists them under Available
  services. Unavailable values read 0xFFFF (u16) or 0x7FFF (s16), mapped to no value; reserved
  registers answer too, so reads span gaps. A unit without a device answers exception 10 or 11. The
  "Scalefactor" column divides (10 → ×0.1, 0.1 → ×10, -10 → ×-0.1). VE.Bus state of charge stays 0
  when a BMS or the GX computes it: use the system's. Checked on a MultiPlus-II GX (2026-11-04).
- ABB B21/B23/B24 share one map (both user manuals, chapter 9, identical tables): function 3 only (no
  4), up to 125 registers, any read within 0x1000-0x8EFF answers, gaps included, so the templates read
  0x5B00-0x5B3D in one request. Addresses are protocol addresses as printed. An absent quantity (B21: all
  per phase values; 3-wire: L-N voltages, L2) reads all ones (unsigned) or the largest positive value
  (signed), mapped to no value. Energies are u64 in 0.01 kWh. The factory serial settings are in no ABB
  document: 19200 Bd, even parity, address 1 are the third party claims (Victron guide: "factory default
  even"; Eniris: 19200 8E2), stop bits not confirmed. Steel variants do not measure export.
- The Adafruit/evcc/mbmd/Tasmota sources sometimes disagree on parity (ORNO 516: 8E1 by three
  sources, odd by one); the template follows the majority with a note.

## Verification

- `tests/unit/modbus_test.c` (CI): CRC and frames against the specification examples
  (`01 03 00 00 00 0A C5 CD`, Eastron's `43 66 33 34` = 230.2 V and `42 70 00 00` = 60.0), every
  word order, exceptions, the template checks, read planning; every built-in template parses and
  its reads fit `max_regs`.
- `tools/modbus/sim_tcp.py`: a Modbus TCP device simulator playing templates (`--udp`: a GoodWe
  Wi-Fi module on UDP 8899 with the discovery port 48899, `--split`: long answers in two datagrams); it encodes a known
  value per entity independently of the gateway code and answers exception 2 on undocumented
  addresses. TC48 runs it: all values of four templates match, writes of each kind are read back,
  the search finds the units, a user template works, an unreachable device goes offline, MQTT
  discovery creates the device via the gateway and a write over MQTT arrives.
- `tests/modbus_tcp/`: a Modbus TCP simulator of a Huawei SUN2000 hybrid inverter (battery, meter)
  with changing data along a simulated day, and its template `huawei_sun2000.json` to import as an
  own template; a sample for simulating other devices ([README](../tests/modbus_tcp/README.md)).
- TC49: the search finds the simulator playing a Victron GX and recognises it by its serial number with
  both units, leaves out an open port without Modbus, refuses a second search and stops; both Victron
  templates read every value as encoded. UI33: the page of the search.
- TC50: the simulator as a GoodWe module on UDP (answers split): the search lists it with unit 127, a
  manual request reads the serial number, an absent unit times out, invalid devices are refused, a device
  on `udp` with `goodwe_xs` reads every value. UI34: the search page adds it in one click. The unit test
  checks the AA 55 framing against the request bytes of the goodwe library (`7F 03 75 31 00 28 04 09`).
- TC57: an SDM630 of the simulator as a virtual ABB B23 read over the Modbus TCP server the way a Victron GX
  reads an ABB meter (type, serial, 0x5B00 x62 across the gap, energies, per phase energies, firmware without
  source) register by register as the simulator's code encodes the ABB template; mapping in the API, invalid
  sources refused, writes refused, the source offline answers exception 11. UI38: the page. The unit test
  checks mapping (units, sum of phases, q) and the register image (fill, nan, u64, fixed text).
- TC56: both ABB templates against the simulator answering gaps (`--lenient`): every value as encoded, and
  the meter's "no value" (u64 all ones, s16 and s32 largest positive) shows no value.
- TC55: the Modbus TCP server against the simulator (`--log-reads`) playing an SDM630 on Modbus TCP and a
  GoodWe on UDP: invalid settings refused, routing by unit id on 502 and any unit id on an own port,
  polled registers from the cache with no read reaching the device, other reads passed through (the
  device's exception too), the rate refusing with 6, writes refused and allowed per address, a third
  client turned away, the GoodWe answering as Modbus TCP; with a second simulator answering after 300 ms
  (`--delay`): answers to clients and an exception kept for the cache time, a write dropping them, one
  device read for two clients reading the same, a client waiting one transaction during a poll,
  `cache_ms` 0. UI37: the page. The unit test checks the
  request decoding and answers of the server and the cache's block extraction.
  `tools/modbus/server_load.py`: the load test above.
- Integration tests (`tests/ha_integration`), UI32.
- **Not yet tested on a real RS485 device**: the RS485 path ran only without a device (timeouts);
  real meters will confirm the serial timing and the maps marked "community" above.

## Sources

Eastron: SDM120-MODBUS protocol V2.4, SDM230-MODBUS protocol V1.1, SDM630 Modbus protocol V1.8
(eastroneurope.com), SDM72DM-V2 user manual V1.1, SDM630-Modbus V2 manuals V1.5/V1.6B, SDM230
manuals v1.6/v1.8, SDM120-M manual; nmakel/sdm_modbus, reaper7/SDM_Energy_Meter, ESPHome
`sdm_meter`, volkszaehler/mbmd, evcc templates, Tasmota energy drivers. Chint DTSU666/DDSU666 user
manuals, lmatula/ha_chint_pm. Peacefair PZEM-014/016 and PZEM-003/017 manuals, ESPHome `pzemac`,
`pzemdc`, mandulaj/PZEM-004T-v30. Hiking: fawno/Modbus, alphp and gabrielcolceriu gists,
nicodupont/esphome-hiking-dds238-2-zn-s-rs485. ORNO: TapHome OR-WE-515 page, evcc `orno.yaml`,
HARDWARIO and Grenton integration pages, apichlinski/orno-reader-domoticz, ioBroker forum.
ABB: B21 user manual 2CMC485004M0201 rev C, B23/B24 user manual 2CMC485003M0201 rev E, EQ B-Serie
Produkthandbuch 2CDC512084D0101; volkszaehler/mbmd `abb.go`, victronenergy/dbus-modbus-client `abb.py` and the
Victron ABB meter guide, openWB `b23.py`, evcc `abb-ab.yaml`.
Template formats compared: evcc, ESPHome `modbus_controller`, Home Assistant `modbus`, Tasmota
`modbus.json`, jollyjinx/modbus-2-mqtt-bridge. Board: schematic sheets 2, 4, 5 and 8
([unofficial_guition_esp32p4_repo](https://github.com/p1ngb4ck/unofficial_guition_esp32p4_repo/tree/main/JC-ESP32P4-M3-Dev)).

Victron: CCGX Modbus TCP register list 3.81 (victronenergy.com, technical information). GoodWe:
marcelblijleven/goodwe (`dt.py` register map of the DT family, `protocol.py` and `modbus.py`: RTU
request over UDP 8899, answer with the AA 55 head and possibly in several datagrams; discovery
`WIFIKIT-214028-READ` on port 48899), its 0.4.3 notes (Modbus TCP 502 on the LAN+WiFi kit 2.0), Home
Assistant issue 176918 and the Homey forum (unit 247 or 1, Kit-20 on 502).
