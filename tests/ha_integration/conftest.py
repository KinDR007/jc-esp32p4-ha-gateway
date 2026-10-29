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
SOUND = {"enabled": True, "running": True, "level_db": -60.5}
EVENTS = [
    {"id": "dog", "name": "Dog barking", "name_cs": "Štěkání psa", "enabled": True, "active": False},
    {"id": "glass", "name": "Glass breaking", "enabled": False, "active": False},
]


@pytest.fixture(autouse=True)
def auto_enable_custom_integrations(enable_custom_integrations):
    yield


class FakeGateway:
    def __init__(self) -> None:
        self.status = copy.deepcopy(STATUS)
        self.sound = dict(SOUND)
        self.events = copy.deepcopy(EVENTS)
        self.password = ""
        self.posts: list[tuple[str, dict]] = []
        self.sockets: list[web.WebSocketResponse] = []
        self.connected = asyncio.Event()
        app = web.Application()
        app.router.add_get("/api/status", self._get(lambda: self.status))
        app.router.add_get("/api/sound/status", self._get(lambda: self.sound))
        app.router.add_get("/api/sound/events", self._get(lambda: {"events": self.events}))
        app.router.add_post("/api/{path:.*}", self._post)
        app.router.add_get("/api/ws", self._ws)
        self.server = TestServer(app, host="127.0.0.1")

    @property
    def host(self) -> str:
        return f"127.0.0.1:{self.server.port}"

    def _authorized(self, request: web.Request) -> bool:
        if not self.password:
            return True
        auth = request.headers.get("Authorization", "")
        import base64

        return auth == "Basic " + base64.b64encode(f"admin:{self.password}".encode()).decode()

    def _get(self, body):
        async def handler(request: web.Request) -> web.Response:
            if not self._authorized(request):
                return web.json_response({"error": "unauthorized"}, status=401)
            return web.json_response(body())

        return handler

    async def _post(self, request: web.Request) -> web.Response:
        if not self._authorized(request):
            return web.json_response({"error": "unauthorized"}, status=401)
        path = "/api/" + request.match_info["path"]
        body = await request.json() if request.can_read_body else {}
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
