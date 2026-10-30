"""State of the gateway: polled REST, pushed sound events and update progress."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass
import json
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.components import persistent_notification
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import P4AuthError, P4Client, P4Error
from .const import (
    DOMAIN,
    LOGGER,
    MANUFACTURER,
    MODEL,
    NOTIFY_DEFAULTS,
    NOTIFY_LEVELS,
    NOTIFY_THRESHOLD,
    RECONNECT_DELAY,
    SCAN_INTERVAL,
)

type P4ConfigEntry = ConfigEntry[P4Coordinator]


@dataclass
class P4Data:
    status: dict[str, Any]
    sound: dict[str, Any]
    events: dict[str, dict[str, Any]]  # sound events by id


class P4Coordinator(DataUpdateCoordinator[P4Data]):
    """Polls the gateway and applies what it pushes in between."""

    config_entry: P4ConfigEntry

    def __init__(self, hass: HomeAssistant, entry: P4ConfigEntry, client: P4Client) -> None:
        super().__init__(hass, LOGGER, config_entry=entry, name=DOMAIN, update_interval=SCAN_INTERVAL)
        self.client = client
        self.mac: str = entry.unique_id or ""
        self.push_connected = False
        self.notify_seq: int | None = None  # last notification seen; None until the first connect
        self.notify_boot: int | None = None  # run of the gateway it belongs to

    @property
    def sound_signal(self) -> str:
        """Dispatcher signal of a detected sound: (event id, payload)."""
        return f"{DOMAIN}_{self.config_entry.entry_id}_sound"

    @property
    def notify_signal(self) -> str:
        """Dispatcher signal of a gateway notification (dict with topic, level, text, time)."""
        return f"{DOMAIN}_{self.config_entry.entry_id}_notify"

    @property
    def device_info(self) -> DeviceInfo:
        status = self.data.status
        return DeviceInfo(
            identifiers={(DOMAIN, self.mac)},
            connections={(CONNECTION_NETWORK_MAC, self.mac)},
            name=status["hostname"],
            manufacturer=MANUFACTURER,
            model=MODEL,
            sw_version=status["version"],
            configuration_url=f"http://{self.client.host}/",
        )

    async def _async_update_data(self) -> P4Data:
        try:
            status = await self.client.status()
            sound = await self.client.sound_status()
            events = await self.client.sound_events()
        except P4AuthError as err:
            raise ConfigEntryAuthFailed from err
        except P4Error as err:
            raise UpdateFailed(f"Gateway {self.client.host} unreachable: {err}") from err
        return P4Data(status=status, sound=sound, events={e["id"]: e for e in events})

    @callback
    def _on_push(self, topic: str, payload: str) -> None:
        if not self.data:
            return
        if topic == "update":
            self.data.status["update"] = json.loads(payload)
            self.async_set_updated_data(self.data)
        elif topic.startswith("sound/") and topic.endswith("/state"):
            event = self.data.events.get(topic[6:-6])
            if event is not None:
                event["active"] = payload == "ON"
                self.async_set_updated_data(self.data)
        elif topic.startswith("sound/"):
            async_dispatcher_send(self.hass, self.sound_signal, topic[6:], json.loads(payload))
        elif topic == "notify":
            note = json.loads(payload)
            if isinstance(note, dict):
                self._notify(note)

    @callback
    def _notify(self, note: dict[str, Any]) -> None:
        """One notification: the event entity, and a persistent notification from the chosen level on."""
        seq = note.get("seq", 0)
        if note.get("boot") != self.notify_boot and self.notify_boot is not None:
            self.notify_seq = 0  # the gateway restarted and counts from 1 again
        self.notify_boot = note.get("boot", self.notify_boot)
        if self.notify_seq is not None and seq <= self.notify_seq:
            return
        self.notify_seq = seq
        async_dispatcher_send(self.hass, self.notify_signal, note)
        topic = note.get("topic", "")
        option = self.config_entry.options.get(topic, NOTIFY_DEFAULTS.get(topic, "off"))
        if option == "off" or NOTIFY_LEVELS.get(note.get("level", ""), 0) < NOTIFY_THRESHOLD[option]:
            return
        persistent_notification.async_create(
            self.hass,
            note.get("text", ""),
            title=f"{self.data.status['hostname'] if self.data else 'P4 Gateway'}: {topic}",
            notification_id=f"{DOMAIN}_{self.mac}_{seq}",
        )

    async def _fetch_missed(self) -> None:
        """Notifications sent while the push connection was down (a restart of the gateway)."""
        try:
            data = await self.client.notifications(0)
        except P4Error:
            return
        if self.notify_seq is None:
            # first connect after Home Assistant started: only remember where the gateway is
            self.notify_seq, self.notify_boot = data.get("seq", 0), data.get("boot")
            return
        for note in data.get("notifications", []):
            self._notify(note)

    @callback
    def _on_connect(self) -> None:
        self.push_connected = True
        LOGGER.debug("Push connection to %s open", self.client.host)
        # what changed while the connection was down
        self.hass.async_create_task(self.async_request_refresh())
        self.hass.async_create_task(self._fetch_missed())

    async def async_listen(self) -> None:
        """Keeps the push connection open for the life of the entry."""
        while True:
            try:
                await self.client.listen(self._on_push, self._on_connect)
            except P4AuthError:
                self.config_entry.async_start_reauth(self.hass)
                return
            except P4Error as err:
                LOGGER.debug("Push connection to %s: %s", self.client.host, err)
            self.push_connected = False
            await asyncio.sleep(RECONNECT_DELAY)
