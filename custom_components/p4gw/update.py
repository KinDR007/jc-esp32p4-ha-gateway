"""Updates from the manifest URL set on the gateway (System › Firmware › Online update): the gateway
firmware, the package of the C6 radio firmwares and, with an ESP32-C5 as radio B, the C5 package. The
packages hold every radio firmware (Thread, Zigbee, scanner, Wi-Fi uplink, ESPHome Bluetooth proxy).
An older listed release (the releases that run on the flash layout of the board) is installed with
update.install and its version (rollback)."""

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
    coordinator = entry.runtime_data
    status = coordinator.data.status
    entities: list[UpdateEntity] = [P4Update(coordinator), P4RadioUpdate(coordinator, "c6")]
    if "c5" in (status.get("update") or {}) and (status.get("role") or {}).get("b", {}).get("chip") == "ESP32-C5":
        entities.append(P4RadioUpdate(coordinator, "c5"))
    async_add_entities(entities)


class P4Update(P4Entity, UpdateEntity):
    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_entity_category = EntityCategory.CONFIG
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS | UpdateEntityFeature.SPECIFIC_VERSION

    def __init__(self, coordinator: P4Coordinator) -> None:
        super().__init__(coordinator, "firmware")

    @property
    def _update(self) -> dict[str, Any]:
        return self.coordinator.data.status.get("update") or {}

    @property
    def installed_version(self) -> str | None:
        return self.coordinator.data.status["version"]

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        return {"available_versions": [r["version"] for r in self._update.get("releases") or []]}

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
        body = {"version": version} if version and version != self._update.get("latest_version") else None
        try:
            self.coordinator.data.status["update"] = await self.coordinator.client.post("/api/update/install", body)
        except P4Error as err:
            raise HomeAssistantError(f"Update: {err}") from err
        self.async_write_ha_state()


class P4RadioUpdate(P4Entity, UpdateEntity):
    """A radio package, c6 (radio A) or c5 (radio B); versions are the first bytes of its sha256."""

    _attr_device_class = UpdateDeviceClass.FIRMWARE
    _attr_entity_category = EntityCategory.CONFIG
    _attr_supported_features = UpdateEntityFeature.INSTALL | UpdateEntityFeature.PROGRESS | UpdateEntityFeature.SPECIFIC_VERSION

    def __init__(self, coordinator: P4Coordinator, chip: str) -> None:
        super().__init__(coordinator, f"{chip}_firmware")
        self._chip = chip
        self._attr_translation_key = f"{chip}_firmware"

    @property
    def _pkg(self) -> dict[str, Any]:
        return (self.coordinator.data.status.get("update") or {}).get(self._chip) or {}

    @property
    def installed_version(self) -> str | None:
        return self._pkg.get("installed_id") or None

    @property
    def _releases(self) -> list[dict[str, Any]]:
        return [r for r in (self.coordinator.data.status.get("update") or {}).get("releases") or [] if r.get(self._chip)]

    @property
    def extra_state_attributes(self) -> dict[str, Any]:
        """Packages of the listed releases: install one with update.install, version = its id."""
        return {"available_versions": {r[self._chip]: r["version"] for r in self._releases}}

    @property
    def latest_version(self) -> str | None:
        pkg = self._pkg
        return (pkg.get("offered_id") if pkg.get("offered") else None) or self.installed_version

    @property
    def release_summary(self) -> str | None:
        """Firmwares that change: name installed → offered."""
        pkg = self._pkg
        have, offer = pkg.get("installed_versions") or {}, pkg.get("offered_versions") or {}
        changes = [f"{name}: {have.get(name, '–')} → {version}" for name, version in offer.items() if have.get(name) != version]
        return "\n".join(changes) or None

    @property
    def in_progress(self) -> bool:
        return bool(self._pkg.get("in_progress"))

    @property
    def update_percentage(self) -> int | None:
        return self._pkg.get("percent") if self.in_progress else None

    async def async_install(self, version: str | None, backup: bool, **kwargs: Any) -> None:
        name = self._chip.upper()
        body: dict[str, Any] = {"target": self._chip}
        if version and version != self.latest_version:
            release = next((r["version"] for r in self._releases if version in (r[self._chip], r["version"])), None)
            if not release:
                raise HomeAssistantError(f"{name} update: no listed release has the package {version}")
            body["version"] = release
        try:
            self.coordinator.data.status["update"] = await self.coordinator.client.post("/api/update/install", body)
        except P4Error as err:
            raise HomeAssistantError(f"{name} update: {err}") from err
        self.async_write_ha_state()
