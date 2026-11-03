#pragma once
#include "esp_err.h"

/* Modbus devices on RS485 (J4) and Modbus TCP with JSON templates (docs/modbus.md): polling, writes,
 * API /api/modbus*, MQTT discovery (one Home Assistant device per Modbus device), Telegram /modbus. */
esp_err_t modbus_init(void);
