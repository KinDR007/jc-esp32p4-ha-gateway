"""Event entities: a sound event detected and a notification of the gateway (both pushed the moment
they happen)."""

from __future__ import annotations

from typing import Any

from homeassistant.components.event import EventEntity
from homeassistant.core import HomeAssistant, callback
from homeassistant.helpers.dispatcher import async_dispatcher_connect
from homeassistant.helpers.entity_platform import AddConfigEntryEntitiesCallback

from .coordinator import P4ConfigEntry, P4Coordinator
from .entity import P4Entity, P4SoundEntity, add_sound_entities


async def async_setup_entry(
    hass: HomeAssistant, entry: P4ConfigEntry, async_add_entities: AddConfigEntryEntitiesCallback
) -> None:
    async_add_entities([P4NotificationEvent(entry.runtime_data)])
    add_sound_entities(entry.runtime_data, async_add_entities, P4SoundEvent)


class P4NotificationEvent(P4Entity, EventEntity):
    """Notification of the gateway: event type is the level, attributes the topic and the text."""

    _attr_translation_key = "notification"
    _attr_event_types = ["info", "warning", "critical"]

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "notification")

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            async_dispatcher_connect(self.hass, self.coordinator.notify_signal, self._on_notify)
        )

    @callback
    def _on_notify(self, note: dict[str, Any]) -> None:
        level = note.get("level") if note.get("level") in self._attr_event_types else "info"
        self._trigger_event(level, {k: note[k] for k in ("topic", "text", "time") if k in note})
        self.async_write_ha_state()


class P4SoundEvent(P4SoundEntity, EventEntity):
    _attr_translation_key = "sound"
    _attr_event_types = ["detected"]

    def __init__(self, coordinator: P4Coordinator, event_id: str) -> None:
        super().__init__(coordinator, event_id, "")
        event = coordinator.data.events[event_id]
        self._attr_translation_placeholders = {"name": event["name"]}
        if event.get("device_class") in ("doorbell", "button", "motion"):
            self._attr_device_class = event["device_class"]

    async def async_added_to_hass(self) -> None:
        await super().async_added_to_hass()
        self.async_on_remove(
            async_dispatcher_connect(self.hass, self.coordinator.sound_signal, self._on_sound)
        )

    @callback
    def _on_sound(self, event_id: str, payload: dict[str, Any]) -> None:
        if event_id != self.event_id:
            return
        attributes = {k: payload[k] for k in ("class", "score", "level_db", "name_cs") if k in payload}
        self._trigger_event(payload.get("event_type", "detected"), attributes)
        self.async_write_ha_state()
