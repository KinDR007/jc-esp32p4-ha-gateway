"""Constants of the P4 gateway integration."""

from datetime import timedelta
import logging

DOMAIN = "p4gw"
LOGGER = logging.getLogger(__package__)

SCAN_INTERVAL = timedelta(seconds=30)
# Pause before the push connection is opened again after it dropped.
RECONNECT_DELAY = 10

MANUFACTURER = "Guition"
MODEL = "JC-ESP32P4-M3-DEV gateway"

# Roles a user can switch to (the gateway falls back to "none" by itself).
ROLES = ["thread_br", "thread_router", "zigbee_coordinator", "zigbee_router", "esphome"]
ZIGBEE_ROLES = ("zigbee_coordinator", "zigbee_router")
