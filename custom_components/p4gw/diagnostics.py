"""Diagnostics: what the gateway reports (it holds no secrets in these replies)."""

from __future__ import annotations

from dataclasses import asdict
from typing import Any

from homeassistant.components.diagnostics import async_redact_data
from homeassistant.const import CONF_API_TOKEN, CONF_PASSWORD
from homeassistant.core import HomeAssistant

from .coordinator import P4ConfigEntry


async def async_get_config_entry_diagnostics(hass: HomeAssistant, entry: P4ConfigEntry) -> dict[str, Any]:
    coordinator = entry.runtime_data
    return {
        "entry": async_redact_data(dict(entry.data), {CONF_API_TOKEN, CONF_PASSWORD}),
        "push_connected": coordinator.push_connected,
        "data": asdict(coordinator.data),
    }
