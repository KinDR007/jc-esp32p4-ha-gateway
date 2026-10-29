"""Buttons: restart, identify, Zigbee network backup, update check."""

from __future__ import annotations

from collections.abc import Callable
from dataclasses import dataclass
from typing import Any

from homeassistant.components.button import ButtonDeviceClass, ButtonEntity, ButtonEntityDescription
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.exceptions import HomeAssistantError
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .api import P4Error
from .const import ZIGBEE_ROLES
from .coordinator import P4ConfigEntry, P4Data
from .entity import P4Entity


@dataclass(frozen=True, kw_only=True)
class P4ButtonDescription(ButtonEntityDescription):
    path: str
    body: dict[str, Any] | None = None
    available: Callable[[P4Data], bool] = lambda d: True


BUTTONS = (
    P4ButtonDescription(
        key="restart",
        device_class=ButtonDeviceClass.RESTART,
        entity_category=EntityCategory.CONFIG,
        path="/api/reboot",
    ),
    P4ButtonDescription(
        key="identify",
        device_class=ButtonDeviceClass.IDENTIFY,
        entity_category=EntityCategory.DIAGNOSTIC,
        path="/api/identify",
        body={"seconds": 10},
    ),
    P4ButtonDescription(
        key="zigbee_backup",
        translation_key="zigbee_backup",
        entity_category=EntityCategory.CONFIG,
        path="/api/c6/zigbee-snapshot",
        available=lambda d: d.status["role"]["active"] in ZIGBEE_ROLES,
    ),
    P4ButtonDescription(
        key="update_check",
        translation_key="update_check",
        entity_category=EntityCategory.DIAGNOSTIC,
        path="/api/update/check",
        entity_registry_enabled_default=False,
    ),
)


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities(P4Button(entry.runtime_data, d) for d in BUTTONS)


class P4Button(P4Entity, ButtonEntity):
    entity_description: P4ButtonDescription

    def __init__(self, coordinator, description: P4ButtonDescription) -> None:
        super().__init__(coordinator, description.key)
        self.entity_description = description

    @property
    def available(self) -> bool:
        return super().available and self.entity_description.available(self.coordinator.data)

    async def async_press(self) -> None:
        try:
            await self.coordinator.client.post(self.entity_description.path, self.entity_description.body)
        except P4Error as err:
            raise HomeAssistantError(f"{self.entity_description.key}: {err}") from err
