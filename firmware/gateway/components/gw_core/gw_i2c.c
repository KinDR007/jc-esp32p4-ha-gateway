#include "esp_log.h"
#include "gw_i2c.h"

static const char *TAG = "gw_i2c";
static i2c_master_bus_handle_t s_bus;

esp_err_t gw_i2c_init(void)
{
    if (s_bus) {
        return ESP_OK;
    }
    i2c_master_bus_config_t cfg = {
        .i2c_port = -1,
        .sda_io_num = GW_I2C_SDA,
        .scl_io_num = GW_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* weak; the board has 2.2 kΩ and 5.1 kΩ pull-ups (camera and touch connectors) */
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2C bus: %s", esp_err_to_name(err));
        s_bus = NULL;
    }
    return err;
}

i2c_master_bus_handle_t gw_i2c_bus(void)
{
    return s_bus;
}
