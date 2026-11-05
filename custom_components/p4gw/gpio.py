"""GPIO of header JP1 (System › GPIO): the device "P4 GPIO", connected via the gateway.

An input is a binary sensor, an output a switch, a PWM pin a light with brightness (the duty), a counter
two sensors (count and frequency). Values come with the poll (/api/gpio) and are pushed on every change
(topic gpio: {"<gpio>": level | duty | {count, rate}})."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .api import P4Error
from .const import DOMAIN, MANUFACTURER
from .coordinator import P4Coordinator

EXPOSED = ("integration", "both")

# platform and value key of the entities each mode gets
ENTITIES = {
    "input": [("binary_sensor", "level")],
    "output": [("switch", "level")],
    "pwm": [("light", "duty")],
    "counter": [("sensor", "count"), ("sensor", "frequency")],
}


class P4GpioEntity(CoordinatorEntity[P4Coordinator]):
    """One value of a configured pin."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: P4Coordinator, gpio: int, mode: str, key: str) -> None:
        super().__init__(coordinator)
        self.gpio, self.mode, self.key = gpio, mode, key
        self._attr_unique_id = f"{coordinator.mac}_gpio_{gpio}_{mode}_{key}"

    @property
    def device_info(self) -> DeviceInfo:
        info = DeviceInfo(
            identifiers={(DOMAIN, f"{self.coordinator.mac}_gpio")},
            name=f"{self.coordinator.data.status['hostname']} GPIO",
            manufacturer=MANUFACTURER,
            model="GPIO of header JP1",
        )
        if hasattr(dr, "async_get_device_id_by_identifier"):  # Home Assistant 2026.8 and later
            info["via_device_id"] = dr.async_get_device_id_by_identifier(
                self.hass, (DOMAIN, self.coordinator.mac), config_entry_id=self.coordinator.config_entry.entry_id
            )
        else:
            info["via_device"] = (DOMAIN, self.coordinator.mac)
        return info

    @property
    def pin(self) -> dict[str, Any] | None:
        return self.coordinator.data.gpio.get(self.gpio)

    @property
    def name(self) -> str:
        pin = self.pin or {}
        base = pin.get("name") or f"GPIO{self.gpio}"
        if self.key == "frequency":
            cs = self.hass is not None and self.hass.config.language == "cs"
            return f"{base} {'frekvence' if cs else 'frequency'}"
        return base

    @property
    def available(self) -> bool:
        pin = self.pin
        return (
            super().available
            and pin is not None
            and pin.get("active", False)
            and pin["mode"] == self.mode
            and pin.get("expose") in EXPOSED
            and self.key in pin
        )

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        pin = self.pin or {}
        return {"gpio": self.gpio, "jp1_pin": pin.get("jp1")}

    async def set(self, body: dict[str, Any]) -> None:
        """POST /api/gpio/set for this pin; the value is kept until the next push or poll."""
        try:
            await self.coordinator.client.post("/api/gpio/set", {"gpio": self.gpio, **body})
        except P4Error as err:
            raise HomeAssistantError(f"GPIO{self.gpio}: {err}") from err
        pin = self.pin
        if pin is not None:
            pin.update({k: v for k, v in body.items() if k in ("level", "duty")})
            self.async_write_ha_state()


def add_gpio_entities(
    coordinator: P4Coordinator, async_add_entities: Callable, platform: str, factory: Callable[[P4Coordinator, int, str, str], Any]
) -> None:
    """Adds this platform's entities of every pin shown through the integration, now and when pins change."""
    known: set[tuple[int, str, str]] = set()

    def add_new() -> None:
        new = []
        for gpio, pin in coordinator.data.gpio.items():
            if pin.get("expose") not in EXPOSED:
                continue
            for plat, key in ENTITIES.get(pin["mode"], []):
                if plat == platform and (gpio, pin["mode"], key) not in known:
                    known.add((gpio, pin["mode"], key))
                    new.append(factory(coordinator, gpio, pin["mode"], key))
        if new:
            async_add_entities(new)

    add_new()
    coordinator.config_entry.async_on_unload(coordinator.async_add_listener(add_new))
