#pragma once
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

/* I2C sensors on header JP1 (docs/sensors.md): scan, identification, configuration, readings, API,
 * MQTT discovery. Needs gw_i2c_init(); without the bus it only answers the API. */
esp_err_t sensors_init(void);

/* Second bus on free pins of JP1 (gpio_ctl), NULL when it goes: sensors with "bus": 2 use it; a new bus
 * is scanned at once. Its devices are released before this returns. */
void sensors_set_bus2(i2c_master_bus_handle_t bus, int sda, int scl, uint32_t hz);
