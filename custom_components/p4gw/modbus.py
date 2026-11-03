"""Modbus devices of the gateway (System › RS485): each one its own device, connected via the gateway.

The gateway describes every device by a template (/api/modbus/template); its entities become sensors,
binary sensors, numbers, switches, selects and buttons here. Values come with the poll (/api/modbus)
and are pushed on every read of the device (topic modbus/<id>)."""

from __future__ import annotations

from collections.abc import Callable
from typing import Any

from homeassistant.components.sensor import SensorDeviceClass, SensorStateClass
from homeassistant.const import EntityCategory
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers import device_registry as dr
from homeassistant.helpers.device_registry import DeviceInfo
from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .api import P4Error
from .const import DOMAIN
from .coordinator import P4Coordinator

EXPOSED = ("integration", "both")


def platform_of(entity: dict[str, Any]) -> str:
    kind = (entity.get("write") or {}).get("kind")
    if kind:
        return kind
    if "map" not in entity and (entity.get("type") == "bool" or "bit" in entity or entity.get("reg") in ("coil", "discrete")):
        return "binary_sensor"
    return "sensor"


class P4ModbusEntity(CoordinatorEntity[P4Coordinator]):
    """One entity of a Modbus device's template."""

    _attr_has_entity_name = True

    def __init__(self, coordinator: P4Coordinator, device_id: str, entity: dict[str, Any]) -> None:
        super().__init__(coordinator)
        self.device_id = device_id
        self.key = entity["key"]
        self.spec = entity
        self._attr_unique_id = f"{coordinator.mac}_mb_{device_id}_{self.key}"
        category = entity.get("category")
        self._attr_entity_category = EntityCategory(category) if category in ("config", "diagnostic") else None

    @property
    def device_info(self) -> DeviceInfo:
        """Its own device, connected via the gateway (read when the entity is added, hass is set then)."""
        device = self.coordinator.data.modbus.get(self.device_id, {})
        info = DeviceInfo(
            identifiers={(DOMAIN, f"{self.coordinator.mac}_mb_{self.device_id}")},
            name=device.get("name", self.device_id),
            manufacturer=device.get("manufacturer"),
            model=device.get("model"),
        )
        if hasattr(dr, "async_get_device_id_by_identifier"):  # Home Assistant 2026.8 and later
            info["via_device_id"] = dr.async_get_device_id_by_identifier(
                self.hass, (DOMAIN, self.coordinator.mac), config_entry_id=self.coordinator.config_entry.entry_id
            )
        else:
            info["via_device"] = (DOMAIN, self.coordinator.mac)
        return info

    @property
    def name(self) -> str:
        cs = self.hass is not None and self.hass.config.language == "cs"
        return (cs and self.spec.get("name_cs")) or self.spec["name"]

    @property
    def device(self) -> dict[str, Any] | None:
        return self.coordinator.data.modbus.get(self.device_id)

    @property
    def value(self) -> Any:
        device = self.device
        return device["values"].get(self.key) if device else None

    @property
    def available(self) -> bool:
        device = self.device
        if not super().available or not device or not device.get("enabled") or device.get("expose") not in EXPOSED:
            return False
        if device.get("state") not in ("ok", "partial"):
            return False
        return platform_of(self.spec) == "button" or self.key in device["values"]

    async def write(self, value: Any = None) -> None:
        body: dict[str, Any] = {"device": self.device_id, "key": self.key}
        if value is not None:
            body["value"] = value
        try:
            await self.coordinator.client.post("/api/modbus/write", body)
        except P4Error as err:
            raise HomeAssistantError(f"{self.spec['name']}: {err}") from err
        if value is not None and self.device:
            self.device["values"][self.key] = value
            self.async_write_ha_state()


def sensor_attrs(entity: dict[str, Any]) -> dict[str, Any]:
    """device class, unit, state class and precision of a read-only value"""
    if "map" in entity:
        return {"device_class": SensorDeviceClass.ENUM, "options": list(entity["map"].values())}
    attrs: dict[str, Any] = {"native_unit_of_measurement": entity.get("unit")}
    try:
        attrs["device_class"] = SensorDeviceClass(entity["class"])
    except (KeyError, ValueError):
        pass
    try:
        attrs["state_class"] = SensorStateClass(entity["state"])
    except (KeyError, ValueError):
        pass
    if entity.get("type") != "str":
        attrs["suggested_display_precision"] = entity.get("precision", 0)
    return attrs


def add_modbus_entities(
    coordinator: P4Coordinator, async_add_entities: Callable, platform: str, factory: Callable[[P4Coordinator, str, dict], Any]
) -> None:
    """Adds the entities of this platform of every exposed Modbus device, now and when devices are added."""
    known: set[tuple[str, str]] = set()

    def add_new() -> None:
        new = []
        for device_id, device in coordinator.data.modbus.items():
            template = coordinator.data.modbus_templates.get(device["template"])
            if device.get("expose") not in EXPOSED or not template:
                continue
            for entity in template["entities"]:
                if platform_of(entity) == platform and (device_id, entity["key"]) not in known:
                    known.add((device_id, entity["key"]))
                    new.append(factory(coordinator, device_id, entity))
        if new:
            async_add_entities(new)

    add_new()
    coordinator.config_entry.async_on_unload(coordinator.async_add_listener(add_new))
