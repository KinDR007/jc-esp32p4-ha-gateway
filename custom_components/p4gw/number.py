"""Numbers: writable registers of the Modbus devices (System › RS485)."""

from __future__ import annotations

from homeassistant.components.number import NumberEntity, NumberMode
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import P4ConfigEntry
from .modbus import P4ModbusEntity, add_modbus_entities


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    add_modbus_entities(entry.runtime_data, async_add_entities, "number", P4ModbusNumber)


class P4ModbusNumber(P4ModbusEntity, NumberEntity):
    """A writable register within the template's limits."""

    _attr_mode = NumberMode.BOX

    def __init__(self, coordinator, device_id: str, entity: dict) -> None:
        super().__init__(coordinator, device_id, entity)
        write = entity["write"]
        self._attr_native_min_value = write["min"]
        self._attr_native_max_value = write["max"]
        self._attr_native_step = write.get("step") or 10 ** -entity.get("precision", 0)
        self._attr_native_unit_of_measurement = entity.get("unit")

    @property
    def native_value(self) -> float | None:
        value = self.value
        return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None

    async def async_set_native_value(self, value: float) -> None:
        await self.write(value)
