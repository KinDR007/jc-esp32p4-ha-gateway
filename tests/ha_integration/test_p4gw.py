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
        # the push connection refreshes on connect: let that poll land before a test pushes, or it
        # overwrites the pushed state (a slow runner connects after the setup settled)
        for _ in range(20):
            await asyncio.sleep(0.01)
            await hass.async_block_till_done()
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


async def test_c6_update(hass: HomeAssistant, gateway) -> None:
    """The C6 package: offered by a pushed update state, versions kept from the poll, install posts target c6."""
    await setup_entry(hass, gateway)
    c6_id = entity_id(hass, "update", "c6_firmware")
    assert hass.states.get(c6_id).state == STATE_OFF
    await push(hass, gateway, "update", dict(gateway.status["update"], c6={"offered": True, "available": True, "in_progress": False,
                                                                         "percent": 0, "installed_id": "9ae0d4cd", "offered_id": "1b2c3d4e"}))
    state = hass.states.get(c6_id)
    assert state.state == STATE_ON
    assert state.attributes["installed_version"] == "9ae0d4cd"
    assert state.attributes["latest_version"] == "1b2c3d4e"
    await hass.services.async_call("update", "install", {"entity_id": c6_id}, blocking=True)
    assert ("/api/update/install", {"target": "c6"}) in gateway.posts


async def test_c5_update(hass: HomeAssistant, gateway) -> None:
    """Radio B with an ESP32-C5 gets the C5 package entity; install posts target c5, a listed release by its id."""
    gateway.status["update"]["c5"] = {"offered": True, "available": True, "in_progress": False, "percent": 0,
                                      "installed_id": "b6c56e01", "offered_id": "7f00aa11",
                                      "installed_versions": {"esphome": "p4gw-ble-proxy 2026.10.1"},
                                      "offered_versions": {"esphome": "p4gw-ble-proxy 2026.11.1"}}
    gateway.status["update"]["releases"] = [{"version": "v1.9", "c5": "b6c56e01"}]
    await setup_entry(hass, gateway)
    c5 = entity_id(hass, "update", "c5_firmware")
    state = hass.states.get(c5)
    assert state.state == STATE_ON
    assert state.attributes["installed_version"] == "b6c56e01" and state.attributes["latest_version"] == "7f00aa11"
    assert "esphome: p4gw-ble-proxy 2026.10.1 → p4gw-ble-proxy 2026.11.1" in state.attributes["release_summary"]
    await hass.services.async_call("update", "install", {"entity_id": c5}, blocking=True)
    assert ("/api/update/install", {"target": "c5"}) in gateway.posts
    await hass.services.async_call("update", "install", {"entity_id": c5, "version": "b6c56e01"}, blocking=True)
    assert ("/api/update/install", {"target": "c5", "version": "v1.9"}) in gateway.posts


async def test_no_c5_update_without_c5(hass: HomeAssistant, gateway) -> None:
    """Radio B without an ESP32-C5 (or none): no C5 package entity."""
    gateway.status["update"]["c5"] = {"offered": False, "installed_id": ""}
    gateway.status["role"]["b"]["chip"] = "ESP32-C6"
    await setup_entry(hass, gateway)
    assert entity_id(hass, "update", "c5_firmware") is None


async def test_rollback(hass: HomeAssistant, gateway) -> None:
    """A listed release is installed with update.install and its version (the C6 package by its id)."""
    gateway.status["update"]["releases"] = [{"version": "v1.7", "c6": "1b2c3d4e"}, {"version": "v1.6", "c6": "0cc0a7ad"}]
    await setup_entry(hass, gateway)
    fw, c6 = entity_id(hass, "update", "firmware"), entity_id(hass, "update", "c6_firmware")
    assert hass.states.get(fw).attributes["available_versions"] == ["v1.7", "v1.6"]
    assert hass.states.get(c6).attributes["available_versions"] == {"1b2c3d4e": "v1.7", "0cc0a7ad": "v1.6"}
    await hass.services.async_call("update", "install", {"entity_id": fw, "version": "v1.6"}, blocking=True)
    assert ("/api/update/install", {"version": "v1.6"}) in gateway.posts
    await hass.services.async_call("update", "install", {"entity_id": c6, "version": "0cc0a7ad"}, blocking=True)
    assert ("/api/update/install", {"target": "c6", "version": "v1.6"}) in gateway.posts
    with pytest.raises(HomeAssistantError):
        await hass.services.async_call("update", "install", {"entity_id": c6, "version": "deadbeef"}, blocking=True)


async def test_radio_b_role(hass: HomeAssistant, gateway) -> None:
    """A gateway with radio B gets its role select; picking a role posts it for radio B."""
    await setup_entry(hass, gateway)
    select = entity_id(hass, "select", "radio_b_role")
    assert hass.states.get(select).state == "zigbee_coordinator"
    await hass.services.async_call("select", "select_option", {"entity_id": select, "option": "none"}, blocking=True)
    assert ("/api/role", {"role": "none", "radio": "b"}) in gateway.posts


async def test_i2c_sensors(hass: HomeAssistant, gateway) -> None:
    """Exposed I2C sensors become sensor entities named after the gateway's name; later ones are added,
    renaming follows, a sensor without a value or no longer exposed is unavailable."""
    entry = await setup_entry(hass, gateway)
    temp = entity_id(hass, "sensor", "i2c_76_temperature")
    state = hass.states.get(temp)
    assert state.state == "21.4" and state.attributes["unit_of_measurement"] == "°C"
    assert state.attributes["device_class"] == "temperature" and state.attributes["friendly_name"].endswith("Obývák Temperature")
    assert state.attributes["chip"] == "BME280" and state.attributes["address"] == "0x76"
    assert hass.states.get(entity_id(hass, "sensor", "i2c_76_pressure")).attributes["device_class"] == "atmospheric_pressure"
    assert entity_id(hass, "sensor", "i2c_23_illuminance") is None  # not exposed

    gateway.sensors["sensors"][1]["expose"] = True
    gateway.sensors["sensors"][0]["name"] = "Ložnice"
    gateway.sensors["sensors"][0]["values"].pop("humidity")
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()
    assert hass.states.get(entity_id(hass, "sensor", "i2c_23_illuminance")).state == "120.0"
    assert hass.states.get(entity_id(hass, "sensor", "i2c_76_humidity")).state == STATE_UNAVAILABLE

    gateway.sensors["sensors"][0]["expose"] = False
    await entry.runtime_data.async_refresh()
    await hass.async_block_till_done()
    assert hass.states.get(temp).state == STATE_UNAVAILABLE


async def test_firmware_without_sensors(hass: HomeAssistant, gateway) -> None:
    """Older firmware (404 on /api/sensors): the entry loads, no I2C sensor entities."""
    gateway.sensors = None
    entry = await setup_entry(hass, gateway)
    assert entry.state is ConfigEntryState.LOADED
    assert entity_id(hass, "sensor", "i2c_76_temperature") is None


async def test_modbus_device(hass: HomeAssistant, gateway) -> None:
    """A Modbus device shown through the integration is its own device via the gateway, with an entity per
    template entity of the right kind; writes go to /api/modbus/write; pushed reads update the values."""
    from homeassistant.helpers import device_registry as dr

    await setup_entry(hass, gateway)
    reg = er.async_get(hass)
    eid = lambda platform, key: reg.async_get_entity_id(platform, DOMAIN, f"{MAC}_mb_d1_{key}")
    voltage = hass.states.get(eid("sensor", "voltage"))
    assert voltage.state == "52.31" and voltage.attributes["unit_of_measurement"] == "V" and voltage.attributes["device_class"] == "voltage"
    assert hass.states.get(eid("sensor", "high_voltage_alarm")).attributes["options"] == ["off", "on"]
    assert hass.states.get(eid("binary_sensor", "fault")).state == STATE_OFF
    assert hass.states.get(eid("switch", "relay")).state == STATE_ON
    thr = hass.states.get(eid("number", "high_voltage_threshold"))
    assert float(thr.state) == 58.4 and thr.attributes["min"] == 5 and thr.attributes["max"] == 350
    assert hass.states.get(eid("select", "shunt")).state == "100 A"
    assert eid("button", "reset_energy") is not None
    assert reg.async_get_entity_id("sensor", DOMAIN, f"{MAC}_mb_d2_voltage") is None  # MQTT only
    # a virtual device (emulation): what it shows, nothing to write
    assert hass.states.get(reg.async_get_entity_id("sensor", DOMAIN, f"{MAC}_mb_d3_voltage")).state == "52.31"
    for platform, key in (("number", "high_voltage_threshold"), ("select", "shunt"), ("button", "reset_energy"), ("switch", "relay")):
        assert reg.async_get_entity_id(platform, DOMAIN, f"{MAC}_mb_d3_{key}") is None

    entry_id = hass.config_entries.async_entries(DOMAIN)[0].entry_id
    devices = dr.async_get(hass)
    meter = devices.async_get_device_by_identifier((DOMAIN, f"{MAC}_mb_d1"), entry_id)
    gw = devices.async_get_device_by_identifier((DOMAIN, MAC), entry_id)
    assert meter.name == "Battery meter" and meter.model == "PZEM-017 / PZEM-003" and meter.via_device_id == gw.id

    await hass.services.async_call("number", "set_value", {"entity_id": eid("number", "high_voltage_threshold"), "value": 57}, blocking=True)
    await hass.services.async_call("select", "select_option", {"entity_id": eid("select", "shunt"), "option": "200 A"}, blocking=True)
    await hass.services.async_call("switch", "turn_off", {"entity_id": eid("switch", "relay")}, blocking=True)
    await hass.services.async_call("button", "press", {"entity_id": eid("button", "reset_energy")}, blocking=True)
    assert ("/api/modbus/write", {"device": "d1", "key": "high_voltage_threshold", "value": 57.0}) in gateway.posts
    assert ("/api/modbus/write", {"device": "d1", "key": "shunt", "value": "200 A"}) in gateway.posts
    assert ("/api/modbus/write", {"device": "d1", "key": "relay", "value": "OFF"}) in gateway.posts
    assert ("/api/modbus/write", {"device": "d1", "key": "reset_energy"}) in gateway.posts

    await push(hass, gateway, "modbus/d1", {**gateway.modbus["devices"][0]["values"], "voltage": 53.0})
    assert hass.states.get(eid("sensor", "voltage")).state == "53.0"
    await push(hass, gateway, "modbus/d1/availability", "offline")
    assert hass.states.get(eid("sensor", "voltage")).state == STATE_UNAVAILABLE
    assert gateway.template_gets == 1  # the template is fetched once while its revision stays


async def test_firmware_without_modbus(hass: HomeAssistant, gateway) -> None:
    gateway.modbus = None
    entry = await setup_entry(hass, gateway)
    assert entry.state is ConfigEntryState.LOADED
    assert er.async_get(hass).async_get_entity_id("sensor", DOMAIN, f"{MAC}_mb_d1_voltage") is None


async def test_gpio_device(hass: HomeAssistant, gateway) -> None:
    """GPIO of JP1 shown through the integration: the device "<host> GPIO" via the gateway with a binary sensor
    per input, a switch per output, a light per PWM pin (brightness = duty) and two sensors per counter;
    commands go to /api/gpio/set; the push of topic gpio updates them; a pin only for MQTT has no entity."""
    from homeassistant.helpers import device_registry as dr

    await setup_entry(hass, gateway)
    eid = lambda platform, key: er.async_get(hass).async_get_entity_id(platform, DOMAIN, f"{MAC}_gpio_{key}")
    assert hass.states.get(eid("binary_sensor", "3_input_level")).state == STATE_OFF
    relay = hass.states.get(eid("switch", "2_output_level"))
    assert relay.state == STATE_ON and relay.attributes["friendly_name"].endswith("Relay") and relay.attributes["jp1_pin"] == 9
    led = hass.states.get(eid("light", "1_pwm_duty"))
    assert led.state == STATE_ON and led.attributes["brightness"] == 102
    assert hass.states.get(eid("sensor", "45_counter_count")).state == "120"
    freq = hass.states.get(eid("sensor", "45_counter_frequency"))
    assert float(freq.state) == 1.0 and freq.attributes["unit_of_measurement"] == "Hz"
    assert eid("switch", "32_output_level") is None  # MQTT only

    entry_id = hass.config_entries.async_entries(DOMAIN)[0].entry_id
    devices = dr.async_get(hass)
    dev = devices.async_get_device_by_identifier((DOMAIN, f"{MAC}_gpio"), entry_id)
    gw = devices.async_get_device_by_identifier((DOMAIN, MAC), entry_id)
    assert dev.name == "p4gw-e30a08 GPIO" and dev.via_device_id == gw.id

    await hass.services.async_call("switch", "turn_off", {"entity_id": eid("switch", "2_output_level")}, blocking=True)
    await hass.services.async_call("light", "turn_on", {"entity_id": eid("light", "1_pwm_duty"), "brightness": 255}, blocking=True)
    await hass.services.async_call("light", "turn_off", {"entity_id": eid("light", "1_pwm_duty")}, blocking=True)
    assert ("/api/gpio/set", {"gpio": 2, "level": 0}) in gateway.posts
    assert ("/api/gpio/set", {"gpio": 1, "duty": 100.0}) in gateway.posts
    assert ("/api/gpio/set", {"gpio": 1, "duty": 0}) in gateway.posts
    assert hass.states.get(eid("switch", "2_output_level")).state == STATE_OFF
    await hass.services.async_call("light", "turn_on", {"entity_id": eid("light", "1_pwm_duty")}, blocking=True)
    assert gateway.posts[-1] == ("/api/gpio/set", {"gpio": 1, "duty": 100.0})  # back to the last duty

    await push(hass, gateway, "gpio", {"3": 1, "2": 1, "1": 25.0, "45": {"count": 130, "rate": 4.0}})
    assert hass.states.get(eid("binary_sensor", "3_input_level")).state == STATE_ON
    assert hass.states.get(eid("switch", "2_output_level")).state == STATE_ON
    assert hass.states.get(eid("light", "1_pwm_duty")).attributes["brightness"] == 64
    assert hass.states.get(eid("sensor", "45_counter_count")).state == "130"
    assert float(hass.states.get(eid("sensor", "45_counter_frequency")).state) == 2.0  # both edges: two counts a period


async def test_gpio_pin_off_or_changed(hass: HomeAssistant, gateway) -> None:
    """A pin that went off (radio B took its pins) or changed its mode is unavailable; the new mode's entity appears."""
    await setup_entry(hass, gateway)
    eid = lambda platform, key: er.async_get(hass).async_get_entity_id(platform, DOMAIN, f"{MAC}_gpio_{key}")
    gateway.gpio["pins"][1].update(active=False, error="radio B: UART TX (radio B has the role zigbee_router)")
    gateway.gpio["pins"][0].update(mode="output", level=1)
    coordinator = hass.config_entries.async_entries(DOMAIN)[0].runtime_data
    await coordinator.async_refresh()
    await hass.async_block_till_done()
    assert hass.states.get(eid("switch", "2_output_level")).state == STATE_UNAVAILABLE
    assert hass.states.get(eid("binary_sensor", "3_input_level")).state == STATE_UNAVAILABLE
    assert hass.states.get(eid("switch", "3_output_level")).state == STATE_ON


async def test_firmware_without_gpio(hass: HomeAssistant, gateway) -> None:
    gateway.gpio = None
    entry = await setup_entry(hass, gateway)
    assert entry.state is ConfigEntryState.LOADED
    assert er.async_get(hass).async_get_entity_id("switch", DOMAIN, f"{MAC}_gpio_2_output_level") is None


async def test_gpio_strip_and_onewire(hass: HomeAssistant, gateway) -> None:
    """A WS2812 strip is an RGB light with the rainbow effect; each DS18B20 of a 1-Wire bus a temperature sensor,
    one found later is added from the push."""
    await setup_entry(hass, gateway)
    eid = lambda platform, key: er.async_get(hass).async_get_entity_id(platform, DOMAIN, f"{MAC}_gpio_{key}")
    strip = hass.states.get(eid("light", "33_ws2812_light"))
    assert strip.state == STATE_ON and strip.attributes["rgb_color"] == (255, 80, 0) and strip.attributes["brightness"] == 200
    assert "rainbow" in strip.attributes["effect_list"]
    temp = hass.states.get(eid("sensor", "46_onewire_28ff641e0f16034c"))
    assert float(temp.state) == 21.5 and temp.attributes["unit_of_measurement"] == "°C" and temp.attributes["friendly_name"].endswith("Floor 034c")

    await hass.services.async_call("light", "turn_on", {"entity_id": eid("light", "33_ws2812_light"), "rgb_color": [0, 0, 255],
                                                        "brightness": 50, "effect": "rainbow"}, blocking=True)
    assert ("/api/gpio/set", {"gpio": 33, "on": True, "color": [0, 0, 255], "brightness": 50, "effect": "rainbow"}) in gateway.posts
    await hass.services.async_call("light", "turn_off", {"entity_id": eid("light", "33_ws2812_light")}, blocking=True)
    assert gateway.posts[-1] == ("/api/gpio/set", {"gpio": 33, "on": False})

    await push(hass, gateway, "gpio", {"33": {"state": "OFF", "color": {"r": 0, "g": 0, "b": 255}, "brightness": 50, "effect": "rainbow"},
                                       "46": {"28ff641e0f16034c": 22.0, "28aa000000000099": 19.25}})
    assert hass.states.get(eid("light", "33_ws2812_light")).state == STATE_OFF
    assert float(hass.states.get(eid("sensor", "46_onewire_28ff641e0f16034c")).state) == 22.0
    assert float(hass.states.get(eid("sensor", "46_onewire_28aa000000000099")).state) == 19.25  # found later
    await push(hass, gateway, "gpio", {"46": {"28ff641e0f16034c": None}})
    assert hass.states.get(eid("sensor", "46_onewire_28ff641e0f16034c")).state == "unknown"
    assert hass.states.get(eid("sensor", "46_onewire_28aa000000000099")).state == STATE_UNAVAILABLE  # gone from the bus
