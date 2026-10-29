"""Role of the gateway (switching restarts it)."""

from __future__ import annotations

from homeassistant.components.select import SelectEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import P4Error
from .const import ROLES
from .coordinator import P4ConfigEntry, P4Coordinator
from .entity import P4Entity


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([P4RoleSelect(entry.runtime_data)])


class P4RoleSelect(P4Entity, SelectEntity):
    _attr_translation_key = "desired_role"
    _attr_entity_category = EntityCategory.CONFIG
    _attr_options = ROLES

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "desired_role")

    @property
    def current_option(self) -> str | None:
        desired = self.coordinator.data.status["role"]["desired"]
        return desired if desired in ROLES else None

    async def async_select_option(self, option: str) -> None:
        try:
            await self.coordinator.client.post("/api/role", {"role": option})
        except P4Error as err:
            raise HomeAssistantError(f"Role switch refused: {err}") from err
        self.coordinator.data.status["role"]["desired"] = option
        self.async_write_ha_state()
