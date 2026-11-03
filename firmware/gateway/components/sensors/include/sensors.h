#pragma once
#include "esp_err.h"

/* I2C sensors on header JP1 (docs/sensors.md): scan, identification, configuration, readings, API,
 * MQTT discovery. Needs gw_i2c_init(); without the bus it only answers the API. */
esp_err_t sensors_init(void);
