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
from homeassistant.const import EntityCategory, UnitOfFrequency, UnitOfInformation, UnitOfSoundPressure, UnitOfTemperature
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback
from homeassistant.util import dt as dt_util

from .const import ROLES
from .coordinator import P4ConfigEntry, P4Data
from .entity import P4Entity
from .gpio import P4GpioEntity, add_gpio_entities
from .modbus import P4ModbusEntity, add_modbus_entities, sensor_attrs


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
        key="noise",
        translation_key="noise",
        device_class=SensorDeviceClass.SOUND_PRESSURE,
        native_unit_of_measurement=UnitOfSoundPressure.WEIGHTED_DECIBEL_A,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        value=lambda d: (d.sound.get("spl") or {}).get("laeq"),
    ),
    P4SensorDescription(
        key="noise_max",
        translation_key="noise_max",
        device_class=SensorDeviceClass.SOUND_PRESSURE,
        native_unit_of_measurement=UnitOfSoundPressure.WEIGHTED_DECIBEL_A,
        state_class=SensorStateClass.MEASUREMENT,
        suggested_display_precision=1,
        value=lambda d: (d.sound.get("spl") or {}).get("lamax"),
    ),
    P4SensorDescription(
        key="sound_level",
        translation_key="sound_level",
        native_unit_of_measurement="dBFS",
        state_class=SensorStateClass.MEASUREMENT,
        entity_category=EntityCategory.DIAGNOSTIC,
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

    # I2C sensors (System › Hardware): one entity per exposed sensor and quantity, also those added later
    known: set[tuple[str, str]] = set()

    def add_new() -> None:
        new = [
            (sid, key)
            for sid, s in coordinator.data.sensors.items()
            if s["expose"] and s["enabled"]
            for key in s["quantities"]
            if (sid, key) not in known
        ]
        known.update(new)
        if new:
            async_add_entities(P4I2CSensor(coordinator, sid, key) for sid, key in new)

    add_new()
    entry.async_on_unload(coordinator.async_add_listener(add_new))
    add_modbus_entities(coordinator, async_add_entities, "sensor", P4ModbusSensor)
    add_gpio_entities(coordinator, async_add_entities, "sensor", P4GpioCounter)


class P4GpioCounter(P4GpioEntity, SensorEntity):
    """A pulse counter of JP1: the count since the last reset or start, or its frequency."""

    def __init__(self, coordinator, gpio: int, mode: str, key: str) -> None:
        super().__init__(coordinator, gpio, mode, key)
        if key == "frequency":
            self._attr_device_class = SensorDeviceClass.FREQUENCY
            self._attr_native_unit_of_measurement = UnitOfFrequency.HERTZ
            self._attr_state_class = SensorStateClass.MEASUREMENT
            self._attr_suggested_display_precision = 1
        else:
            self._attr_state_class = SensorStateClass.TOTAL_INCREASING

    @property
    def native_value(self) -> float | None:
        pin = self.pin
        return pin.get(self.key) if pin else None


class P4ModbusSensor(P4ModbusEntity, SensorEntity):
    """A read-only value of a Modbus device: a number, a named state (enum) or a text."""

    def __init__(self, coordinator, device_id: str, entity: dict[str, Any]) -> None:
        super().__init__(coordinator, device_id, entity)
        for attr, value in sensor_attrs(entity).items():
            setattr(self, f"_attr_{attr}", value)

    @property
    def native_value(self) -> Any:
        return self.value


class P4I2CSensor(P4Entity, SensorEntity):
    """One quantity of an I2C sensor; named after the sensor's name on the gateway."""

    _attr_state_class = SensorStateClass.MEASUREMENT

    def __init__(self, coordinator, sensor_id: str, key: str) -> None:
        super().__init__(coordinator, f"{sensor_id}_{key}")
        self.sensor_id, self.key = sensor_id, key
        quantity = coordinator.data.quantities.get(key, {})
        try:
            self._attr_device_class = SensorDeviceClass(quantity["device_class"])
        except (KeyError, ValueError):
            self._attr_device_class = None
        self._attr_native_unit_of_measurement = quantity.get("unit")
        self._attr_suggested_display_precision = quantity.get("precision")
        self._quantity_name = quantity.get("name", key)

    @property
    def _sensor(self) -> dict[str, Any] | None:
        return self.coordinator.data.sensors.get(self.sensor_id)

    @property
    def name(self) -> str:
        sensor = self._sensor
        return f"{sensor['name'] if sensor else self.sensor_id} {self._quantity_name}"

    @property
    def available(self) -> bool:
        sensor = self._sensor
        return (
            super().available
            and sensor is not None
            and sensor["expose"]
            and sensor["enabled"]
            and self.key in sensor["values"]
        )

    @property
    def native_value(self) -> float | None:
        sensor = self._sensor
        return sensor["values"].get(self.key) if sensor else None

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        sensor = self._sensor or {}
        return {"chip": sensor.get("chip"), "address": f"0x{sensor.get('addr', 0):02x}"}


class P4Sensor(P4Entity, SensorEntity):
    entity_description: P4SensorDescription

    def __init__(self, coordinator, description: P4SensorDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def native_value(self) -> Any:
        return self.entity_description.value(self.coordinator.data)
