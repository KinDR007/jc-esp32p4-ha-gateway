"""P4 gateway (JC-ESP32P4-M3-DEV): Thread border router, Zigbee coordinator, BLE proxy and sound detection."""

from __future__ import annotations

from homeassistant.const import CONF_HOST, CONF_PASSWORD, Platform
from homeassistant.core import HomeAssistant
from homeassistant.helpers.aiohttp_client import async_get_clientsession

from .api import P4Client
from .coordinator import P4ConfigEntry, P4Coordinator

PLATFORMS = [
    Platform.BINARY_SENSOR,
    Platform.BUTTON,
    Platform.EVENT,
    Platform.SELECT,
    Platform.SENSOR,
    Platform.SWITCH,
    Platform.UPDATE,
]


async def async_setup_entry(hass: HomeAssistant, entry: P4ConfigEntry) -> bool:
    client = P4Client(entry.data[CONF_HOST], async_get_clientsession(hass), entry.data.get(CONF_PASSWORD))
    coordinator = P4Coordinator(hass, entry, client)
    await coordinator.async_config_entry_first_refresh()
    entry.runtime_data = coordinator
    entry.async_create_background_task(hass, coordinator.async_listen(), "p4gw-push")
    await hass.config_entries.async_forward_entry_setups(entry, PLATFORMS)
    return True


async def async_unload_entry(hass: HomeAssistant, entry: P4ConfigEntry) -> bool:
    return await hass.config_entries.async_unload_platforms(entry, PLATFORMS)
