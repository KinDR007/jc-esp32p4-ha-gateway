"""Sensors of the gateway."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime
from typing import Any

from homeassistant.components.sensor import (
    SensorDeviceClass,
    SensorEntity,
    SensorEntityDescription,
    SensorStateClass,
)
from homeassistant.const import EntityCategory, UnitOfInformation, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.util import dt as dt_util

from .const import ROLES
from .coordinator import P4ConfigEntry, P4Data
from .entity import P4Entity


@dataclass(frozen=True, kw_only=True)
class P4SensorDescription(SensorEntityDescription):
    value: Callable[[P4Data], Any]


def _c6_temperature(data: P4Data) -> float | None:
    return (data.status["c6"].get("telemetry") or {}).get("temp_c")


def _boot_time(data: P4Data) -> datetime:
    # by the gateway clock (0 until it has the time): steady across polls
    now = data.status.get("time") or dt_util.utcnow().timestamp()
    boot = dt_util.utc_from_timestamp(now - data.status["uptime_s"])
    return boot.replace(second=0, microsecond=0)


SENSORS = (
    P4SensorDescription(
        key="role",
        translation_key="role",
        device_class=SensorDeviceClass.ENUM,
        options=["none", *ROLES],
        value=lambda d: d.status["role"]["active"],
    ),
    P4SensorDescription(
        key="maintenance",
        translation_key="maintenance",
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda d: d.status["role"]["maintenance"] or "none",
    ),
    P4SensorDescription(
        key="thread_state",
        translation_key="thread_state",
        value=lambda d: (d.status.get("role_status") or {}).get("role"),
    ),
    P4SensorDescription(
        key="sound_level",
        translation_key="sound_level",
        native_unit_of_measurement="dBFS",
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        value=lambda d: d.sound.get("level_db") if d.sound.get("running") else None,
    ),
    P4SensorDescription(
        key="temperature",
        translation_key="temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda d: d.status.get("temperature_c"),
    ),
    P4SensorDescription(
        key="c6_temperature",
        translation_key="c6_temperature",
        device_class=SensorDeviceClass.TEMPERATURE,
        native_unit_of_measurement=UnitOfTemperature.CELSIUS,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=_c6_temperature,
    ),
    P4SensorDescription(
        key="boot_time",
        translation_key="boot_time",
        device_class=SensorDeviceClass.TIMESTAMP,
        entity_category=EntityCategory.DIAGNOSTIC,
        value=_boot_time,
    ),
    P4SensorDescription(
        key="heap_free",
        translation_key="heap_free",
        device_class=SensorDeviceClass.DATA_SIZE,
        native_unit_of_measurement=UnitOfInformation.BYTES,
        suggested_unit_of_measurement=UnitOfInformation.KIBIBYTES,
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda d: d.status["heap"]["internal_free"],
    ),
    P4SensorDescription(
        key="cpu_load",
        translation_key="cpu_load",
        native_unit_of_measurement="%",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
        entity_registry_enabled_default=False,
        value=lambda d: max(d.status["hw"].get("cpu_load") or [0]),
    ),
    P4SensorDescription(
        key="ip",
        translation_key="ip",
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda d: d.status["eth"].get("ipv4"),
    ),
    P4SensorDescription(
        key="c6_firmware",
        translation_key="c6_firmware",
        entity_category=EntityCategory.DIAGNOSTIC,
        value=lambda d: d.status["c6"].get("bundle"),
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities(P4Sensor(coordinator, d) for d in SENSORS)


class P4Sensor(P4Entity, SensorEntity):
    entity_description: P4SensorDescription

    def __init__(self, coordinator, description: P4SensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value(self.coordinator.data)
