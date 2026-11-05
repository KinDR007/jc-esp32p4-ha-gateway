"""State of the gateway: polled REST, pushed sound events and update progress."""

from __future__ import annotations

import asyncio
from dataclasses import dataclass, field
import json
from typing import Any

from homeassistant.config_entries import ConfigEntry
from homeassistant.components import persistent_notification
from homeassistant.core import HomeAssistant, callback
from homeassistant.exceptions import ConfigEntryAuthFailed
from homeassistant.helpers.device_registry import CONNECTION_NETWORK_MAC, DeviceInfo
from homeassistant.helpers.dispatcher import async_dispatcher_send
from homeassistant.helpers.update_coordinator import DataUpdateCoordinator, UpdateFailed

from .api import P4AuthError, P4Client, P4Error, P4RequestError
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
    sensors: dict[str, dict[str, Any]]  # I2C sensors by id ("i2c_76")
    quantities: dict[str, dict[str, Any]]  # what the sensors measure, by key
    modbus: dict[str, dict[str, Any]]  # Modbus devices by id ("d1"), with their values
    modbus_templates: dict[str, dict[str, Any]]  # templates of the exposed Modbus devices, by id
    gpio: dict[int, dict[str, Any]] = field(default_factory=dict)  # configured GPIO of JP1 by number, with values


def apply_gpio_push(pins: dict[int, dict[str, Any]], values: dict[str, Any]) -> None:
    """Values of the push message into the pins of the last poll."""
    for key, value in values.items():
        pin = pins.get(int(key))
        if pin is None:
            continue
        if pin["mode"] == "pwm":
            pin["duty"] = value
        elif pin["mode"] == "ws2812" and isinstance(value, dict):
            pin["light"] = value
        elif pin["mode"] == "onewire" and isinstance(value, dict):
            pin["temperatures"] = value
        elif pin["mode"] == "counter" and isinstance(value, dict):
            pin["count"], pin["rate"] = value.get("count", 0), value.get("rate", 0)
            pin["frequency"] = value.get("frequency", pin["rate"] / 2 if pin.get("edge") == "both" else pin["rate"])
        else:
            pin["level"] = value


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
            try:
                sensors = await self.client.sensors()
            except P4RequestError:
                sensors = {}  # firmware without I2C sensors, or a token without the right to System › Hardware
            try:
                modbus = (await self.client.modbus())["devices"]
            except P4RequestError:
                modbus = []  # firmware without Modbus, or no right to System › RS485
            templates = await self._modbus_templates(modbus)
            try:
                gpio = (await self.client.gpio())["pins"]
            except P4RequestError:
                gpio = []  # firmware without GPIO, or no right to System › Hardware
        except P4AuthError as err:
            raise ConfigEntryAuthFailed from err
        except P4Error as err:
            raise UpdateFailed(f"Gateway {self.client.host} unreachable: {err}") from err
        return P4Data(
            status=status,
            sound=sound,
            events={e["id"]: e for e in events},
            sensors={s["id"]: s for s in sensors.get("sensors", [])},
            quantities={q["key"]: q for q in sensors.get("quantities", [])},
            modbus={d["id"]: d for d in modbus},
            modbus_templates=templates,
            gpio={p["gpio"]: p for p in gpio},
        )

    async def _modbus_templates(self, devices: list[dict[str, Any]]) -> dict[str, dict[str, Any]]:
        """Templates of the devices shown through the integration; fetched again when their revision changes."""
        known = self.data.modbus_templates if self.data else {}
        templates: dict[str, dict[str, Any]] = {}
        for device in devices:
            tid = device["template"]
            if device.get("expose") not in ("integration", "both") or tid in templates:
                continue
            cached = known.get(tid)
            if cached and cached.get("rev") == device.get("template_rev"):
                templates[tid] = cached
            else:
                try:
                    templates[tid] = await self.client.modbus_template(tid)
                except P4RequestError as err:
                    LOGGER.warning("Modbus template %s: %s", tid, err)
        return templates

    @callback
    def _on_push(self, topic: str, payload: str) -> None:
        if not self.data:
            return
        if topic == "update":
            # the push carries the state, the poll also the versions of the C6 package: keep those
            old, new = self.data.status.get("update") or {}, json.loads(payload)
            self.data.status["update"] = {**old, **new, "c6": {**(old.get("c6") or {}), **(new.get("c6") or {})}}
            self.async_set_updated_data(self.data)
        elif topic.startswith("sound/") and topic.endswith("/state"):
            event = self.data.events.get(topic[6:-6])
            if event is not None:
                event["active"] = payload == "ON"
                self.async_set_updated_data(self.data)
        elif topic.startswith("sound/"):
            async_dispatcher_send(self.hass, self.sound_signal, topic[6:], json.loads(payload))
        elif topic.startswith("modbus/"):
            # a read of a Modbus device: its values, or its availability
            parts = topic.split("/")
            device = self.data.modbus.get(parts[1])
            if device is None:
                return
            if len(parts) == 3 and parts[2] == "availability":
                device["state"] = "ok" if payload == "online" else "offline"
            elif len(parts) == 2:
                device["values"] = json.loads(payload)
                if device.get("state") not in ("ok", "partial"):
                    device["state"] = "ok"
            self.async_set_updated_data(self.data)
        elif topic == "gpio":
            apply_gpio_push(self.data.gpio, json.loads(payload))
            self.async_set_updated_data(self.data)
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
