"""Sound recognition on/off."""

from __future__ import annotations

from typing import Any

from homeassistant.components.switch import SwitchEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import P4Error
from .coordinator import P4ConfigEntry, P4Coordinator
from .entity import P4Entity


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([P4SoundSwitch(entry.runtime_data)])


class P4SoundSwitch(P4Entity, SwitchEntity):
    """The microphone and the model (events stop when off)."""

    _attr_translation_key = "sound"
    _attr_entity_category = EntityCategory.CONFIG

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "sound")

    @property
    def is_on(self) -> bool:
        return bool(self.coordinator.data.sound.get("enabled"))

    async def _set(self, on: bool) -> None:
        try:
            await self.coordinator.client.post("/api/sound/config", {"enabled": on})
        except P4Error as err:
            raise HomeAssistantError(f"Sound recognition: {err}") from err
        self.coordinator.data.sound["enabled"] = on
        self.coordinator.async_set_updated_data(self.coordinator.data)

    async def async_turn_on(self, **kwargs: Any) -> None:
        await self._set(True)

    async def async_turn_off(self, **kwargs: Any) -> None:
        await self._set(False)
