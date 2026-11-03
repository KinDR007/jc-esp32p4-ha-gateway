"""Fake gateway for the integration tests: REST and the push WebSocket on 127.0.0.1."""

import asyncio
import copy
import json
from pathlib import Path
import sys

from aiohttp import WSMsgType, web
from aiohttp.test_utils import TestServer
import pytest

# custom_components of the repository
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

pytest_plugins = ["pytest_homeassistant_custom_component"]

STATUS = json.loads((Path(__file__).parent / "status.json").read_text())
SOUND = {"enabled": True, "running": True, "level_db": -60.5,
         "spl": {"las": 41.2, "laeq": 40.8, "lamax": 52.3, "overload": False, "window_s": 60, "partial": False,
                 "offset": 135.0, "calibrated": False}}
EVENTS = [
    {"id": "dog", "name": "Dog barking", "name_cs": "Štěkání psa", "enabled": True, "active": False},
    {"id": "glass", "name": "Glass breaking", "enabled": False, "active": False},
]


SENSORS = {
    "sensors": [
        {"id": "i2c_76", "addr": 0x76, "type": "bme280", "chip": "BME280", "name": "Obývák", "expose": True, "enabled": True,
         "state": "ok", "quantities": ["temperature", "humidity", "pressure"],
         "values": {"temperature": 21.4, "humidity": 45.2, "pressure": 1004.1}},
        {"id": "i2c_23", "addr": 0x23, "type": "bh1750", "chip": "BH1750", "name": "Light", "expose": False, "enabled": True,
         "state": "ok", "quantities": ["illuminance"], "values": {"illuminance": 120.0}},
    ],
    "quantities": [
        {"key": "temperature", "name": "Temperature", "unit": "°C", "device_class": "temperature", "precision": 1},
        {"key": "humidity", "name": "Humidity", "unit": "%", "device_class": "humidity", "precision": 1},
        {"key": "pressure", "name": "Pressure", "unit": "hPa", "device_class": "atmospheric_pressure", "precision": 1},
        {"key": "illuminance", "name": "Illuminance", "unit": "lx", "device_class": "illuminance", "precision": 1},
        {"key": "pm25", "name": "PM2.5", "unit": "\u03bcg/m³", "device_class": "pm25", "precision": 0},
    ],
}


# a Modbus device: the built-in PZEM-017 template (number, select, button, enum sensors) plus a coil switch
# and a register bit, as the gateway serves them
TEMPLATE = json.loads((Path(__file__).resolve().parents[2] / "firmware/gateway/components/modbus/templates/peacefair_pzem017.json").read_text())
TEMPLATE["entities"] += [
    {"key": "relay", "name": "Relay", "reg": "coil", "addr": "0x0000", "write": {"kind": "switch", "on": 1, "off": 0}},
    {"key": "fault", "name": "Fault", "reg": "holding", "addr": "0x0010", "bit": 2},
]
TEMPLATE.update(builtin=True, rev=1)
MODBUS = {
    "devices": [
        {"id": "d1", "name": "Battery meter", "template": "peacefair_pzem017", "template_rev": 1, "manufacturer": "Peacefair",
         "model": "PZEM-017 / PZEM-003", "transport": "rtu", "slave": 1, "expose": "integration", "enabled": True, "state": "ok",
         "values": {"voltage": 52.31, "current": 3.5, "power": 183.1, "energy": 12.345, "high_voltage_alarm": "off",
                    "low_voltage_alarm": "off", "high_voltage_threshold": 58.4, "low_voltage_threshold": 44.0, "shunt": "100 A",
                    "relay": True, "fault": False}},
        {"id": "d2", "name": "MQTT only", "template": "peacefair_pzem017", "template_rev": 1, "manufacturer": "Peacefair",
         "model": "PZEM-017", "transport": "rtu", "slave": 2, "expose": "mqtt", "enabled": True, "state": "ok", "values": {}},
    ]
}


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield


class FakeGateway:
    def __init__(self) -> None:
        self.status = copy.deepcopy(STATUS)
        self.sound = dict(SOUND)
        self.events = copy.deepcopy(EVENTS)
        self.sensors = copy.deepcopy(SENSORS)  # None: firmware without /api/sensors
        self.modbus = copy.deepcopy(MODBUS)  # None: firmware without /api/modbus
        self.template_gets = 0
        self.password = ""  # admin password of firmware before user accounts (Basic)
        self.token = ""  # API token of firmware with user accounts (Bearer)
        self.forbidden: set[str] = set()  # POST paths the token's role may not use (403)
        self.notes: list[dict] = []  # notifications kept by the gateway
        self.boot = 1
        self.posts: list[tuple[str, dict]] = []
        self.sockets: list[web.WebSocketResponse] = []
        self.connected = asyncio.Event()
        app = web.Application()
        app.router.add_get("/api/status", self._get(lambda: self.status))
        app.router.add_get("/api/sound/status", self._get(lambda: self.sound))
        app.router.add_get("/api/sound/events", self._get(lambda: {"events": self.events}))
        app.router.add_get("/api/sensors", self._sensors)
        app.router.add_get("/api/modbus", self._modbus)
        app.router.add_get("/api/modbus/template", self._modbus_template)
        app.router.add_get("/api/notify", self._get(lambda: {"notifications": self.notes, "seq": len(self.notes), "boot": self.boot}))
        app.router.add_post("/api/{path:.*}", self._post)
        app.router.add_get("/api/ws", self._ws)
        self.server = TestServer(app, host="127.0.0.1")

    @property
    def host(self) -> str:
        return f"127.0.0.1:{self.server.port}"

    def _authorized(self, request: web.Request) -> bool:
        auth = request.headers.get("Authorization", "")
        if self.token:
            return auth == f"Bearer {self.token}"
        if not self.password:
            return True
        import base64

        return auth == "Basic " + base64.b64encode(f"admin:{self.password}".encode()).decode()

    def _get(self, body):
        async def handler(request: web.Request) -> web.Response:
            if not self._authorized(request):
                return web.json_response({"error": "unauthorized"}, status=401)
            return web.json_response(body())

        return handler

    async def _sensors(self, request: web.Request) -> web.Response:
        if self.sensors is None:
            return web.Response(status=404, text="Nothing matches the given URI")
        return await self._get(lambda: self.sensors)(request)

    async def _modbus(self, request: web.Request) -> web.Response:
        if self.modbus is None:
            return web.Response(status=404, text="Nothing matches the given URI")
        return await self._get(lambda: self.modbus)(request)

    async def _modbus_template(self, request: web.Request) -> web.Response:
        self.template_gets += 1
        if request.query.get("id") != TEMPLATE["id"]:
            return web.json_response({"error": "no such template"}, status=404)
        return await self._get(lambda: TEMPLATE)(request)

    async def _post(self, request: web.Request) -> web.Response:
        if not self._authorized(request):
            return web.json_response({"error": "unauthorized"}, status=401)
        path = "/api/" + request.match_info["path"]
        body = await request.json() if request.can_read_body else {}
        if path in self.forbidden:
            return web.json_response({"error": "forbidden"}, status=403)
        self.posts.append((path, body))
        if path == "/api/c6/zigbee-snapshot" and self.status["role"]["active"] not in ("zigbee_coordinator", "zigbee_router"):
            return web.json_response({"error": "only in a Zigbee role"}, status=409)
        if path == "/api/sound/config":
            self.sound["enabled"] = body["enabled"]
        if path == "/api/update/install":
            return web.json_response(dict(self.status["update"], in_progress=True, percent=0))
        return web.json_response({})

    async def _ws(self, request: web.Request) -> web.StreamResponse:
        if not self._authorized(request):
            return web.json_response({"error": "unauthorized"}, status=401)
        ws = web.WebSocketResponse()
        await ws.prepare(request)
        self.sockets.append(ws)
        self.connected.set()
        async for msg in ws:
            if msg.type != WSMsgType.TEXT:
                break
        return ws

    async def push(self, topic: str, payload: str) -> None:
        for ws in self.sockets:
            if not ws.closed:
                await ws.send_str(json.dumps({"topic": topic, "payload": payload}))

    async def notify(self, topic: str, level: str, text: str, live: bool = True) -> dict:
        """Keeps a notification and, when live, pushes it."""
        note = {"seq": len(self.notes) + 1, "boot": self.boot, "topic": topic, "level": level, "text": text, "time": 0}
        self.notes.append(note)
        if live:
            await self.push("notify", json.dumps(note))
        return note

    async def drop_push(self) -> None:
        for ws in self.sockets:
            await ws.close()
        self.sockets.clear()
        self.connected.clear()


@pytest.fixture
async def gateway(hass, socket_enabled):
    gw = FakeGateway()
    await gw.server.start_server()
    yield gw
    # the integration first: it would connect again to a closing server
    for entry in hass.config_entries.async_entries("p4gw"):
        await hass.config_entries.async_unload(entry.entry_id)
    await gw.drop_push()
    await gw.server.close()
