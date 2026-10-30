"""Client of the gateway REST API and its push WebSocket (/api/ws)."""

from __future__ import annotations

import base64
from collections.abc import Callable
import json
from typing import Any

import aiohttp

TIMEOUT = aiohttp.ClientTimeout(total=10)
# The gateway answers WebSocket pings; one every 20 s also keeps the socket from being purged as idle.
HEARTBEAT = 20


class P4Error(Exception):
    """Gateway error."""


class P4ConnectionError(P4Error):
    """The gateway cannot be reached."""


class P4AuthError(P4Error):
    """Wrong or missing password."""


class P4RequestError(P4Error):
    """The gateway refused a request (the message says why)."""


class P4Client:
    """REST and push client of one gateway."""

    def __init__(
        self, host: str, session: aiohttp.ClientSession, password: str | None = None, token: str | None = None
    ) -> None:
        self.host = host
        self._session = session
        # an API token (firmware with user accounts); the admin password of older entries as Basic
        if token:
            self._headers = {"Authorization": f"Bearer {token}"}
        elif password:
            basic = base64.b64encode(f"admin:{password}".encode()).decode()
            self._headers = {"Authorization": f"Basic {basic}"}
        else:
            self._headers = None

    async def _request(self, method: str, path: str, body: dict[str, Any] | None = None) -> Any:
        try:
            async with self._session.request(
                method, f"http://{self.host}{path}", json=body, headers=self._headers, timeout=TIMEOUT
            ) as resp:
                if resp.status == 401:
                    raise P4AuthError
                data = await resp.json(content_type=None)
                if resp.status == 403:
                    raise P4RequestError("the API token has no right to do this (role of the token)")
                if resp.status >= 400:
                    raise P4RequestError(data.get("error", resp.reason) if isinstance(data, dict) else resp.reason)
                return data
        except (aiohttp.ClientError, TimeoutError, ValueError) as err:
            raise P4ConnectionError(str(err) or type(err).__name__) from err

    async def status(self) -> dict[str, Any]:
        return await self._request("GET", "/api/status")

    async def sound_status(self) -> dict[str, Any]:
        return await self._request("GET", "/api/sound/status")

    async def sound_events(self) -> list[dict[str, Any]]:
        return (await self._request("GET", "/api/sound/events"))["events"]

    async def notifications(self, since: int) -> dict[str, Any]:
        """Notifications the gateway keeps, newer than seq since: {"notifications": [...], "seq"}."""
        return await self._request("GET", f"/api/notify?since={since}")

    async def post(self, path: str, body: dict[str, Any] | None = None) -> Any:
        return await self._request("POST", path, body if body is not None else {})

    async def listen(self, on_message: Callable[[str, str], None], on_connect: Callable[[], None]) -> None:
        """Delivers (topic, payload) of every push message until the connection drops."""
        try:
            async with self._session.ws_connect(
                f"http://{self.host}/api/ws", headers=self._headers, heartbeat=HEARTBEAT, timeout=aiohttp.ClientWSTimeout(ws_close=10)
            ) as ws:
                on_connect()
                async for msg in ws:
                    if msg.type != aiohttp.WSMsgType.TEXT:
                        break
                    data = json.loads(msg.data)
                    on_message(data["topic"], data["payload"])
        except aiohttp.WSServerHandshakeError as err:
            if err.status == 401:
                raise P4AuthError from err
            raise P4ConnectionError(str(err)) from err
        except (aiohttp.ClientError, TimeoutError, ValueError, KeyError) as err:
            raise P4ConnectionError(str(err) or type(err).__name__) from err
