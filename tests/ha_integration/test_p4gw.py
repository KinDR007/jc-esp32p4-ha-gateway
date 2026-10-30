"""P4 gateway integration against a fake gateway (python -m pytest tests/ha_integration)."""

import asyncio
import json
from ipaddress import ip_address
from unittest.mock import patch

from homeassistant.config_entries import SOURCE_USER, SOURCE_ZEROCONF, ConfigEntryState
from homeassistant.const import CONF_API_TOKEN, CONF_HOST, CONF_PASSWORD, STATE_OFF, STATE_ON, STATE_UNAVAILABLE
from homeassistant.exceptions import HomeAssistantError
from homeassistant.core import HomeAssistant
from homeassistant.data_entry_flow import FlowResultType
from homeassistant.helpers import entity_registry as er
from homeassistant.helpers.service_info.zeroconf import ZeroconfServiceInfo
from homeassistant.components import persistent_notification
from pytest_homeassistant_custom_component.common import MockConfigEntry
import pytest

from custom_components.p4gw.const import DOMAIN

MAC = "30:ed:a0:e3:0a:08"


async def setup_entry(hass: HomeAssistant, gateway, **data) -> MockConfigEntry:
    entry = MockConfigEntry(domain=DOMAIN, unique_id=MAC, title="p4gw-e30a08", data={CONF_HOST: gateway.host, **data})
    entry.add_to_hass(hass)
    with patch("custom_components.p4gw.coordinator.RECONNECT_DELAY", 0):
        assert await hass.config_entries.async_setup(entry.entry_id)
        await hass.async_block_till_done()
        await asyncio.wait_for(gateway.connected.wait(), 5)
    return entry


def entity_id(hass: HomeAssistant, platform: str, key: str) -> str:
    return er.async_get(hass).async_get_entity_id(platform, DOMAIN, f"{MAC}_{key}")


async def push(hass: HomeAssistant, gateway, topic: str, payload) -> None:
    await gateway.push(topic, payload if isinstance(payload, str) else json.dumps(payload))
    for _ in range(50):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()


async def test_user_flow(hass: HomeAssistant, gateway) -> None:
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_USER})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_HOST: gateway.host})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["title"] == "p4gw-e30a08"
    assert result["data"] == {CONF_HOST: gateway.host}
    assert result["result"].unique_id == MAC


async def test_user_flow_unreachable(hass: HomeAssistant, gateway) -> None:
    await gateway.server.close()
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_USER})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_HOST: gateway.host})
    assert result["type"] is FlowResultType.FORM
    assert result["errors"] == {"base": "cannot_connect"}


async def test_user_flow_token(hass: HomeAssistant, gateway) -> None:
    gateway.token = "p4gw_secret"
    result = await hass.config_entries.flow.async_init(DOMAIN, context={"source": SOURCE_USER})
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_HOST: gateway.host})
    assert result["step_id"] == "auth"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_API_TOKEN: "p4gw_wrong"})
    assert result["errors"] == {"base": "invalid_auth"}
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {CONF_API_TOKEN: " p4gw_secret "})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["data"] == {CONF_HOST: gateway.host, CONF_API_TOKEN: "p4gw_secret"}


async def test_legacy_password_entry(hass: HomeAssistant, gateway) -> None:
    """Entries from before user accounts keep working with the admin password (Basic)."""
    gateway.password = "secret"
    entry = await setup_entry(hass, gateway, **{CONF_PASSWORD: "secret"})
    assert entry.state is ConfigEntryState.LOADED


def zeroconf_info(host: str, mac: str | None = MAC) -> ZeroconfServiceInfo:
    addr, port = host.split(":")
    return ZeroconfServiceInfo(
        ip_address=ip_address(addr),
        ip_addresses=[ip_address(addr)],
        hostname="p4gw-e30a08.local.",
        name="P4 gateway._p4gw._tcp.local.",
        port=int(port),
        type="_p4gw._tcp.local.",
        properties={"mac": mac, "version": "v1.5"} if mac else {},
    )


async def test_zeroconf_flow(hass: HomeAssistant, gateway) -> None:
    with patch("custom_components.p4gw.config_flow.ZeroconfServiceInfo.host", gateway.host):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": SOURCE_ZEROCONF}, data=zeroconf_info(gateway.host)
        )
    assert result["step_id"] == "confirm"
    assert result["description_placeholders"]["name"] == "p4gw-e30a08"
    result = await hass.config_entries.flow.async_configure(result["flow_id"], {})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    assert result["result"].unique_id == MAC


async def test_zeroconf_updates_host(hass: HomeAssistant, gateway) -> None:
    entry = MockConfigEntry(domain=DOMAIN, unique_id=MAC, data={CONF_HOST: "192.0.2.1"})
    entry.add_to_hass(hass)
    with patch("custom_components.p4gw.config_flow.ZeroconfServiceInfo.host", "192.0.2.7"):
        result = await hass.config_entries.flow.async_init(
            DOMAIN, context={"source": SOURCE_ZEROCONF}, data=zeroconf_info(gateway.host)
        )
    assert result["type"] is FlowResultType.ABORT
    assert result["reason"] == "already_configured"
    assert entry.data[CONF_HOST] == "192.0.2.7"


async def test_zeroconf_without_mac(hass: HomeAssistant, gateway) -> None:
    result = await hass.config_entries.flow.async_init(
        DOMAIN, context={"source": SOURCE_ZEROCONF}, data=zeroconf_info(gateway.host, mac=None)
    )
    assert result["reason"] == "not_p4gw"


async def test_entities(hass: HomeAssistant, gateway) -> None:
    await setup_entry(hass, gateway)
    assert hass.states.get(entity_id(hass, "sensor", "role")).state == "thread_br"
    assert hass.states.get(entity_id(hass, "sensor", "thread_state")).state == "router"
    assert hass.states.get(entity_id(hass, "sensor", "sound_level")).state == "-60.5"
    noise = hass.states.get(entity_id(hass, "sensor", "noise"))
    assert noise.state == "40.8"
    assert noise.attributes["unit_of_measurement"] == "dBA"
    assert noise.attributes["device_class"] == "sound_pressure"
    assert hass.states.get(entity_id(hass, "sensor", "noise_max")).state == "52.3"
    assert float(hass.states.get(entity_id(hass, "sensor", "temperature")).state) == gateway.status["temperature_c"]
    assert hass.states.get(entity_id(hass, "select", "desired_role")).state == "thread_br"
    assert hass.states.get(entity_id(hass, "binary_sensor", "ethernet")).state == STATE_ON
    assert hass.states.get(entity_id(hass, "binary_sensor", "zigbee_host")).state == STATE_UNAVAILABLE
    assert hass.states.get(entity_id(hass, "button", "zigbee_backup")).state == STATE_UNAVAILABLE
    assert hass.states.get(entity_id(hass, "switch", "sound")).state == STATE_ON
    update = hass.states.get(entity_id(hass, "update", "firmware"))
    assert update.state == STATE_OFF
    assert update.attributes["installed_version"] == gateway.status["version"]
    # only enabled sound events get entities
    assert entity_id(hass, "event", "sound_dog") is not None
    assert entity_id(hass, "event", "sound_glass") is None
    assert hass.states.get(entity_id(hass, "binary_sensor", "sound_dog_active")).name.endswith("Dog barking active")


async def test_push_sound(hass: HomeAssistant, gateway) -> None:
    await setup_entry(hass, gateway)
    event_id = entity_id(hass, "event", "sound_dog")
    await push(hass, gateway, "sound/dog", {"event_type": "detected", "class": "Bark", "score": 0.81, "level_db": -30.2})
    state = hass.states.get(event_id)
    assert state.attributes["event_type"] == "detected"
    assert state.attributes["class"] == "Bark"
    assert state.attributes["score"] == 0.81

    active = entity_id(hass, "binary_sensor", "sound_dog_active")
    await push(hass, gateway, "sound/dog/state", "ON")
    assert hass.states.get(active).state == STATE_ON
    await push(hass, gateway, "sound/dog/state", "OFF")
    assert hass.states.get(active).state == STATE_OFF


async def test_push_update_progress(hass: HomeAssistant, gateway) -> None:
    await setup_entry(hass, gateway)
    await push(
        hass, gateway, "update",
        dict(gateway.status["update"], latest_version="v9.9", available=True, in_progress=True, percent=42),
    )
    state = hass.states.get(entity_id(hass, "update", "firmware"))
    assert state.state == STATE_ON
    assert state.attributes["latest_version"] == "v9.9"
    assert state.attributes["in_progress"] is True
    assert state.attributes["update_percentage"] == 42


async def test_new_sound_event_added(hass: HomeAssistant, gateway) -> None:
    entry = await setup_entry(hass, gateway)
    gateway.events.append({"id": "knock", "name": "Knock", "enabled": True, "active": False})
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()
    assert entity_id(hass, "event", "sound_knock") is not None
    gateway.events[0]["enabled"] = False
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()
    assert hass.states.get(entity_id(hass, "event", "sound_dog")).state == STATE_UNAVAILABLE


async def test_commands(hass: HomeAssistant, gateway) -> None:
    await setup_entry(hass, gateway)
    await hass.services.async_call(
        "switch", "turn_off", {"entity_id": entity_id(hass, "switch", "sound")}, blocking=True
    )
    assert ("/api/sound/config", {"enabled": False}) in gateway.posts
    assert hass.states.get(entity_id(hass, "switch", "sound")).state == STATE_OFF

    await hass.services.async_call(
        "select", "select_option",
        {"entity_id": entity_id(hass, "select", "desired_role"), "option": "zigbee_coordinator"}, blocking=True,
    )
    assert ("/api/role", {"role": "zigbee_coordinator"}) in gateway.posts

    await hass.services.async_call("button", "press", {"entity_id": entity_id(hass, "button", "restart")}, blocking=True)
    assert ("/api/reboot", {}) in gateway.posts

    await push(hass, gateway, "update", dict(gateway.status["update"], latest_version="v9.9", available=True))
    await hass.services.async_call("update", "install", {"entity_id": entity_id(hass, "update", "firmware")}, blocking=True)
    assert ("/api/update/install", {}) in gateway.posts


async def test_push_reconnects(hass: HomeAssistant, gateway) -> None:
    entry = await setup_entry(hass, gateway)
    with patch("custom_components.p4gw.coordinator.RECONNECT_DELAY", 0):
        await gateway.drop_push()
        await asyncio.wait_for(gateway.connected.wait(), 5)
    await push(hass, gateway, "sound/dog/state", "ON")
    assert hass.states.get(entity_id(hass, "binary_sensor", "sound_dog_active")).state == STATE_ON
    assert entry.runtime_data.push_connected


async def test_accounts_created_later_start_reauth(hass: HomeAssistant, gateway) -> None:
    """The gateway gets user accounts: reauth asks for a token, which replaces the old password."""
    gateway.password = "old"
    entry = await setup_entry(hass, gateway, **{CONF_PASSWORD: "old"})
    gateway.token = "p4gw_new"
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()
    flows = hass.config_entries.flow.async_progress()
    flow = next(f for f in flows if f["context"]["source"] == "reauth")
    result = await hass.config_entries.flow.async_configure(flow["flow_id"], {CONF_API_TOKEN: "p4gw_new"})
    assert result["reason"] == "reauth_successful"
    assert entry.data == {CONF_HOST: gateway.host, CONF_API_TOKEN: "p4gw_new"}
    await hass.async_block_till_done()
    assert entry.state is ConfigEntryState.LOADED


async def test_token_without_right(hass: HomeAssistant, gateway) -> None:
    """A viewer token: commands fail with a clear error instead of a broken entity."""
    gateway.token = "p4gw_viewer"
    gateway.forbidden = {"/api/reboot"}
    await setup_entry(hass, gateway, **{CONF_API_TOKEN: "p4gw_viewer"})
    with pytest.raises(HomeAssistantError, match="no right"):
        await hass.services.async_call("button", "press", {"entity_id": entity_id(hass, "button", "restart")}, blocking=True)


async def test_notifications(hass: HomeAssistant, gateway) -> None:
    entry = await setup_entry(hass, gateway)
    await push(hass, gateway, "notify", "null")  # ignored
    note_id = entity_id(hass, "event", "notification")
    await gateway.notify("security", "warning", "Password reset requested for jan")
    for _ in range(20):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()
    state = hass.states.get(note_id)
    assert state.attributes["event_type"] == "warning"
    assert state.attributes["topic"] == "security"
    assert state.attributes["text"] == "Password reset requested for jan"
    shown = [n["message"] for n in persistent_notification._async_get_or_create_notifications(hass).values()]
    assert "Password reset requested for jan" in shown
    # info below the default level (warning), sound off by default: event only
    await gateway.notify("system", "info", "Gateway restarted")
    await gateway.notify("sound", "critical", "Smoke alarm")
    for _ in range(20):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()
    shown = [n["message"] for n in persistent_notification._async_get_or_create_notifications(hass).values()]
    assert "Gateway restarted" not in shown and "Smoke alarm" not in shown
    assert hass.states.get(note_id).attributes["text"] == "Smoke alarm"
    # options: all sound notifications
    result = await hass.config_entries.options.async_init(entry.entry_id)
    result = await hass.config_entries.options.async_configure(
        result["flow_id"], {"security": "warning", "system": "all", "network": "off", "sound": "all"})
    assert result["type"] is FlowResultType.CREATE_ENTRY
    await gateway.notify("sound", "info", "Dog barking")
    for _ in range(20):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()
    shown = [n["message"] for n in persistent_notification._async_get_or_create_notifications(hass).values()]
    assert "Dog barking" in shown


async def test_missed_notifications_after_gateway_restart(hass: HomeAssistant, gateway) -> None:
    """Notifications sent while the push connection was down come from /api/notify on reconnect."""
    entry = await setup_entry(hass, gateway)
    for _ in range(20):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()
    await gateway.notify("security", "warning", "before")
    await gateway.drop_push()
    gateway.notes, gateway.boot = [], 2  # the gateway restarted
    await gateway.notify("system", "critical", "Gateway restarted after a crash", live=False)
    with patch("custom_components.p4gw.coordinator.RECONNECT_DELAY", 0):
        await asyncio.wait_for(gateway.connected.wait(), 5)
    for _ in range(50):
        await asyncio.sleep(0.01)
        await hass.async_block_till_done()
    assert hass.states.get(entity_id(hass, "event", "notification")).attributes["text"] == "Gateway restarted after a crash"
    assert entry.runtime_data.notify_boot == 2
