#include "p4gw_telemetry.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define REFRESH_MS 10000
#define TASK_STACK 4096

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_json[P4GW_TELEMETRY_MAX];
static size_t s_len;

static const char *reset_name(esp_reset_reason_t r)
{
    switch (r) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "external";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_BROWNOUT: return "brownout";
    default: return "other";
    }
}

static void refresh_task(void *arg)
{
    const char *fw = arg;
    temperature_sensor_handle_t temp = NULL;
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &temp) != ESP_OK || temperature_sensor_enable(temp) != ESP_OK) {
        temp = NULL;
    }
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    char buf[P4GW_TELEMETRY_MAX];
    while (true) {
        /* Only integer conversions without length modifiers above l: the RCP links newlib nano printf,
         * which has neither %llu nor floats (it then reads the wrong arguments and crashes). */
        float t = 0;
        char tbuf[16] = "null";
        if (temp && temperature_sensor_get_celsius(temp, &t) == ESP_OK) {
            int t10 = (int)(t * 10 + (t < 0 ? -0.5f : 0.5f));
            snprintf(tbuf, sizeof(tbuf), "%s%d.%d", t10 < 0 ? "-" : "", abs(t10) / 10, abs(t10) % 10);
        }
        int n = snprintf(buf, sizeof(buf),
                         "{\"fw\":\"%s\",\"ver\":\"%s\",\"temp_c\":%s,\"heap_free\":%u,\"heap_min\":%u,\"uptime_s\":%lu,"
                         "\"reset\":\"%s\",\"rev\":%u}",
                         fw, esp_app_get_description()->version, tbuf, (unsigned)esp_get_free_heap_size(),
                         (unsigned)esp_get_minimum_free_heap_size(),
                         (unsigned long)(esp_timer_get_time() / 1000000), reset_name(esp_reset_reason()),
                         (unsigned)chip.revision);
        if (n > 0 && (size_t)n < sizeof(buf)) {
            portENTER_CRITICAL(&s_mux);
            memcpy(s_json, buf, n + 1);
            s_len = n;
            portEXIT_CRITICAL(&s_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(REFRESH_MS));
    }
}

void p4gw_telemetry_start(const char *fw)
{
    xTaskCreate(refresh_task, "p4gw_tel", TASK_STACK, (void *)fw, 1, NULL);
}

size_t p4gw_telemetry_get(char *buf, size_t size)
{
    size_t n = 0;
    portENTER_CRITICAL(&s_mux);
    if (s_len && s_len < size) {
        memcpy(buf, s_json, s_len + 1);
        n = s_len;
    }
    portEXIT_CRITICAL(&s_mux);
    return n;
}

/*
 * Called by the patched OpenThread NCP (in the OpenThread task) for vendor properties it does not
 * handle itself: points *data at a copy of the cached JSON. No stack buffer, no driver calls.
 */
int esp_openthread_ncp_vendor_get_hook(uint32_t key, const uint8_t **data)
{
    static char out[P4GW_TELEMETRY_MAX];
    if (key != P4GW_SPINEL_PROP_TELEMETRY) {
        return -1;
    }
    size_t n = p4gw_telemetry_get(out, sizeof(out));
    *data = (const uint8_t *)out;
    return n ? (int)n : -1;
}
