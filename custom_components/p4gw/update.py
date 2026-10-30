"""Updates from the manifest URL set on the gateway (System › Firmware › Online update): the gateway
firmware and the package of the C6 radio firmwares."""

from __future__ import annotations

from typing import Any

from homeassistant.components.update import UpdateDeviceClass, UpdateEntity, UpdateEntityFeature
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
    async_add_entities([P4Update(entry.runtime_data), P4C6Update(entry.runtime_data)])


class P4Update(P4Entity, UpdateEntity):
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_entity_category = EntityCategory.CONFIG
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "firmware")

    @property
    def _update(self) -> dict[str, Any]:
        return self.coordinator.data.status.get("update") or {}

    @property
    def installed_version(self) -> str | None:
        return self.coordinator.data.status["version"]

    @property
    def latest_version(self) -> str | None:
        # without a check (or an update URL) the installed version is the latest known
        return self._update.get("latest_version") or self.installed_version

    @property
    def title(self) -> str | None:
        return self._update.get("title") or None

    @property
    def release_summary(self) -> str | None:
        return self._update.get("release_summary") or None

    @property
    def in_progress(self) -> bool:
        return bool(self._update.get("in_progress"))

    @property
    def update_percentage(self) -> int | None:
        return self._update.get("percent") if self.in_progress else None

    async def async_install(self, version: str | None, backup: bool, **kwargs: Any) -> None:
        try:
            self.coordinator.data.status["update"] = await self.coordinator.client.post("/api/update/install")
        except P4Error as err:
            raise HomeAssistantError(f"Update: {err}") from err
        self.async_write_ha_state()


class P4C6Update(P4Entity, UpdateEntity):
    """The C6 package (all radio firmwares); versions are the first bytes of its sha256."""

    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_entity_category = EntityCategory.CONFIG
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS
    _attr_translation_key = "c6_firmware"

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "c6_firmware")

    @property
    def _c6(self) -> dict[str, Any]:
        return (self.coordinator.data.status.get("update") or {}).get("c6") or {}

    @property
    def installed_version(self) -> str | None:
        return self._c6.get("installed_id") or None

    @property
    def latest_version(self) -> str | None:
        c6 = self._c6
        return (c6.get("offered_id") if c6.get("offered") else None) or self.installed_version

    @property
    def release_summary(self) -> str | None:
        """Firmwares that change: name installed → offered."""
        c6 = self._c6
        have, offer = c6.get("installed_versions") or {}, c6.get("offered_versions") or {}
        changes = [f"{name}: {have.get(name, '–')} → {version}" for name, version in offer.items() if have.get(name) != version]
        return "\n".join(changes) or None

    @property
    def in_progress(self) -> bool:
        return bool(self._c6.get("in_progress"))

    @property
    def update_percentage(self) -> int | None:
        return self._c6.get("percent") if self.in_progress else None

    async def async_install(self, version: str | None, backup: bool, **kwargs: Any) -> None:
        try:
            self.coordinator.data.status["update"] = await self.coordinator.client.post("/api/update/install", {"target": "c6"})
        except P4Error as err:
            raise HomeAssistantError(f"C6 update: {err}") from err
        self.async_write_ha_state()
