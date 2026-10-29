"""Config flow: found by zeroconf (_p4gw._tcp) or added by address, with the web UI password if set."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigFlow, ConfigFlowResult
from homeassistant.const import CONF_HOST, CONF_PASSWORD
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import P4AuthError, P4Client, P4Error
from .const import DOMAIN

USER_SCHEMA = vol.Schema({vol.Required(CONF_HOST): str})
AUTH_SCHEMA = vol.Schema({vol.Required(CONF_PASSWORD): str})


class P4ConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host = ""
        self._name = ""

    async def _probe(self, password: str | None) -> dict[str, Any]:
        """Status of the gateway; raises P4AuthError when the password is wrong or missing."""
        client = P4Client(self._host, async_get_clientsession(self.hass), password)
        return await client.status()

    async def _finish(self, status: dict[str, Any], password: str | None) -> ConfigFlowResult:
        await self.async_set_unique_id(format_mac(status["eth"]["mac"]))
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})
        data = {CONF_HOST: self._host}
        if password:
            data[CONF_PASSWORD] = password
        return self.async_create_entry(title=status["hostname"], data=data)

    async def async_step_user(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            self._host = user_input[CONF_HOST].strip()
            try:
                return await self._finish(await self._probe(None), None)
            except P4AuthError:
                return await self.async_step_auth()
            except P4Error:
                errors["base"] = "cannot_connect"
        return self.async_show_form(step_id="user", data_schema=USER_SCHEMA, errors=errors)

    async def async_step_zeroconf(self, discovery_info: ZeroconfServiceInfo) -> ConfigFlowResult:
        mac = discovery_info.properties.get("mac")
        if not mac:
            return self.async_abort(reason="not_p4gw")
        self._host = discovery_info.host
        await self.async_set_unique_id(format_mac(mac))
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})
        self._name = discovery_info.hostname.removesuffix(".local.").removesuffix(".")
        self.context["title_placeholders"] = {"name": self._name}
        return await self.async_step_confirm()

    async def async_step_confirm(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            try:
                return await self._finish(await self._probe(None), None)
            except P4AuthError:
                return await self.async_step_auth()
            except P4Error:
                return self.async_abort(reason="cannot_connect")
        self._set_confirm_only()
        return self.async_show_form(
            step_id="confirm", description_placeholders={"name": self._name, "host": self._host}
        )

    async def async_step_auth(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        errors: dict[str, str] = {}
        if user_input is not None:
            try:
                status = await self._probe(user_input[CONF_PASSWORD])
            except P4AuthError:
                errors["base"] = "invalid_auth"
            except P4Error:
                errors["base"] = "cannot_connect"
            else:
                if self.source == "reauth":
                    entry = self._get_reauth_entry()
                    await self.async_set_unique_id(format_mac(status["eth"]["mac"]))
                    self._abort_if_unique_id_mismatch(reason="wrong_device")
                    return self.async_update_reload_and_abort(entry, data_updates=user_input)
                return await self._finish(status, user_input[CONF_PASSWORD])
        return self.async_show_form(
            step_id="auth", data_schema=AUTH_SCHEMA, errors=errors, description_placeholders={"host": self._host}
        )

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        self._host = entry_data[CONF_HOST]
        return await self.async_step_auth()
