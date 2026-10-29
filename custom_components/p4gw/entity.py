"""Base of the gateway entities."""

from __future__ import annotations

from homeassistant.helpers.update_coordinator import CoordinatorEntity

from .coordinator import P4Coordinator


class P4Entity(CoordinatorEntity[P4Coordinator]):
    _attr_has_entity_name = True

    def __init__(self, coordinator: P4Coordinator, key: str) -> None:
        super().__init__(coordinator)
        self._attr_unique_id = f"{coordinator.mac}_{key}"
        self._attr_device_info = coordinator.device_info


class P4SoundEntity(P4Entity):
    """Entity of one sound event defined on the gateway (Sound › Events)."""

    def __init__(self, coordinator: P4Coordinator, event_id: str, key: str) -> None:
        super().__init__(coordinator, f"sound_{event_id}{key}")
        self.event_id = event_id

    @property
    def sound_event(self) -> dict | None:
        return self.coordinator.data.events.get(self.event_id)

    @property
    def available(self) -> bool:
        event = self.sound_event
        return super().available and event is not None and event["enabled"]


def add_sound_entities(coordinator: P4Coordinator, async_add_entities, factory) -> None:
    """Adds entities of sound events now and of those defined later."""
    known: set[str] = set()

    def add_new() -> None:
        new = [i for i, e in coordinator.data.events.items() if e["enabled"] and i not in known]
        known.update(new)
        if new:
            async_add_entities([factory(coordinator, i) for i in new])

    add_new()
    coordinator.config_entry.async_on_unload(coordinator.async_add_listener(add_new))
