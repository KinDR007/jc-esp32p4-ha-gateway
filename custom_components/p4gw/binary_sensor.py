"""Binary sensors: links of the gateway and active sound events."""

from __future__ import annotations

from homeassistant.components.binary_sensor import BinarySensorDeviceClass, BinarySensorEntity
from homeassistant.const import EntityCategory
from homeassistant.core import HomeAssistant
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import P4ConfigEntry, P4Coordinator
from .entity import P4Entity, P4SoundEntity, add_sound_entities


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    coordinator = entry.runtime_data
    async_add_entities([P4Ethernet(coordinator), P4ZigbeeHost(coordinator), P4Push(coordinator)])
    add_sound_entities(coordinator, async_add_entities, P4SoundActive)


class P4Ethernet(P4Entity, BinarySensorEntity):
    _attr_translation_key = "ethernet"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_entity_category = EntityCategory.DIAGNOSTIC

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "ethernet")

    @property
    def is_on(self) -> bool:
        return self.coordinator.data.status["eth"]["link"]


class P4ZigbeeHost(P4Entity, BinarySensorEntity):
    """Zigbee2MQTT or ZHA connected to the coordinator (Zigbee coordinator role only)."""

    _attr_translation_key = "zigbee_host"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "zigbee_host")

    @property
    def available(self) -> bool:
        return super().available and "client_connected" in (self.coordinator.data.status.get("role_status") or {})

    @property
    def is_on(self) -> bool:
        return bool(self.coordinator.data.status["role_status"]["client_connected"])


class P4Push(P4Entity, BinarySensorEntity):
    """The push connection (sound events without delay) is open."""

    _attr_translation_key = "push"
    _attr_device_class = BinarySensorDeviceClass.CONNECTIVITY
    _attr_entity_category = EntityCategory.DIAGNOSTIC
    _attr_entity_registry_enabled_default = False

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "push")

    @property
    def is_on(self) -> bool:
        return self.coordinator.push_connected


class P4SoundActive(P4SoundEntity, BinarySensorEntity):
    _attr_translation_key = "sound_active"
    _attr_device_class = BinarySensorDeviceClass.SOUND

    def __init__(self, coordinator: P4Coordinator, event_id: str) -> None:
        super().__init__(coordinator, event_id, "_active")
        self._attr_translation_placeholders = {"name": coordinator.data.events[event_id]["name"]}

    @property
    def is_on(self) -> bool:
        return bool(self.sound_event and self.sound_event["active"])
