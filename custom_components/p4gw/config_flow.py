"""Config flow: found by zeroconf (_p4gw._tcp) or added by address, with an API token once the gateway
has user accounts; options: which gateway notifications become persistent notifications."""

from __future__ import annotations

from collections.abc import Mapping
from typing import Any

import voluptuous as vol

from homeassistant.config_entries import ConfigEntry, ConfigFlow, ConfigFlowResult, OptionsFlow
from homeassistant.const import CONF_API_TOKEN, CONF_HOST
from homeassistant.core import callback
from homeassistant.helpers.selector import SelectSelector, SelectSelectorConfig, SelectSelectorMode
from homeassistant.helpers.aiohttp_client import async_get_clientsession
from homeassistant.helpers.device_registry import format_mac
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo

from .api import P4AuthError, P4Client, P4Error
from .const import DOMAIN, NOTIFY_DEFAULTS, NOTIFY_OPTIONS, NOTIFY_TOPICS

USER_SCHEMA = vol.Schema({vol.Required(CONF_HOST): str})
AUTH_SCHEMA = vol.Schema({vol.Required(CONF_API_TOKEN): str})


class P4ConfigFlow(ConfigFlow, domain=DOMAIN):
    VERSION = 1

    def __init__(self) -> None:
        self._host = ""
        self._name = ""

    @staticmethod
    @callback
    def async_get_options_flow(config_entry: ConfigEntry) -> OptionsFlow:
        return P4OptionsFlow()

    async def _probe(self, token: str | None) -> dict[str, Any]:
        """Status of the gateway; raises P4AuthError when the token is wrong or missing."""
        client = P4Client(self._host, async_get_clientsession(self.hass), token=token)
        return await client.status()

    async def _finish(self, status: dict[str, Any], token: str | None) -> ConfigFlowResult:
        await self.async_set_unique_id(format_mac(status["eth"]["mac"]))
        self._abort_if_unique_id_configured(updates={CONF_HOST: self._host})
        data = {CONF_HOST: self._host}
        if token:
            data[CONF_API_TOKEN] = token
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
            token = user_input[CONF_API_TOKEN].strip()
            try:
                status = await self._probe(token)
            except P4AuthError:
                errors["base"] = "invalid_auth"
            except P4Error:
                errors["base"] = "cannot_connect"
            else:
                if self.source == "reauth":
                    entry = self._get_reauth_entry()
                    await self.async_set_unique_id(format_mac(status["eth"]["mac"]))
                    self._abort_if_unique_id_mismatch(reason="wrong_device")
                    # the token replaces the admin password of entries from before user accounts
                    return self.async_update_reload_and_abort(entry, data={CONF_HOST: self._host, CONF_API_TOKEN: token})
                return await self._finish(status, token)
        return self.async_show_form(
            step_id="auth", data_schema=AUTH_SCHEMA, errors=errors, description_placeholders={"host": self._host}
        )

    async def async_step_reauth(self, entry_data: Mapping[str, Any]) -> ConfigFlowResult:
        self._host = entry_data[CONF_HOST]
        return await self.async_step_auth()


class P4OptionsFlow(OptionsFlow):
    """Per topic of the gateway notifications: from which level on a persistent notification appears.
    Every notification also fires the Notification event entity, whatever is chosen here."""

    async def async_step_init(self, user_input: dict[str, Any] | None = None) -> ConfigFlowResult:
        if user_input is not None:
            return self.async_create_entry(data=user_input)
        selector = SelectSelector(
            SelectSelectorConfig(options=NOTIFY_OPTIONS, mode=SelectSelectorMode.DROPDOWN, translation_key="notify_level")
        )
        options = self.config_entry.options
        schema = vol.Schema(
            {vol.Required(t, default=options.get(t, NOTIFY_DEFAULTS[t])): selector for t in NOTIFY_TOPICS}
        )
        return self.async_show_form(step_id="init", data_schema=schema)
