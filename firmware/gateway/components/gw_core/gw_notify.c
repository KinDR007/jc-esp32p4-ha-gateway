#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "gw.h"

static const char *TAG = "gw_notify";

static const char *const s_levels[] = {"info", "warning", "critical"};

/* One notification: to the log and as {"topic", "level", "text", "time"} on the push channel
 * (/api/ws and MQTT topic notify), where the Home Assistant integration and the Telegram bot
 * pick it up. */
void gw_notify(const char *topic, gw_notify_level_t level, const char *fmt, ...)
{
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    ESP_LOG_LEVEL(level == GW_NOTIFY_INFO ? ESP_LOG_INFO : ESP_LOG_WARN, TAG, "[%s] %s", topic, text);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "topic", topic);
    cJSON_AddStringToObject(json, "level", s_levels[level]);
    cJSON_AddStringToObject(json, "text", text);
    time_t t = time(NULL);
    cJSON_AddNumberToObject(json, "time", t > 1700000000 ? (double)t : 0);
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        gw_mqtt_publish("notify", payload, false);
        cJSON_free(payload);
    }
}
