#pragma once

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The board's one I2C bus: ES8311 codec (0x18), connector CN3 (1 GND, 2 3.3 V, 3 SCL, 4 SDA), header JP1
 * (pin 23 SDA, pin 25 SCL) and the display and camera connectors; shared by the sound component and the
 * I2C sensors (docs/hardware.md, docs/sensors.md). */
#define GW_I2C_SDA 7
#define GW_I2C_SCL 8
#define GW_I2C_CODEC_ADDR 0x18

esp_err_t gw_i2c_init(void);
/* NULL when gw_i2c_init() failed */
i2c_master_bus_handle_t gw_i2c_bus(void);

#ifdef __cplusplus
}
#endif
