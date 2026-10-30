#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"

static const char *TAG = "gw_notify";

/* Notifications of the gateway in four topics (security, system, network, sound) and three levels.
 * Each goes to the log and as {"seq", "topic", "level", "text", "time"} to the push channel (/api/ws
 * and MQTT topic notify); the last NOTIFY_KEEP stay in RAM, so a client that reconnects (Home
 * Assistant after a restart of the gateway) fetches what it missed from /api/notify?since=seq. */
#define NOTIFY_KEEP 32
#define NOTIFY_TEXT 200

typedef struct {
    uint32_t seq;
    char topic[12];
    uint8_t level;
    int64_t time;
    char text[NOTIFY_TEXT];
} entry_t;

static const char *const s_levels[] = {"info", "warning", "critical"};
static entry_t *s_ring;
static uint32_t s_seq;
static uint32_t s_boot; /* random per run: a client tells a restart of the gateway from seq alone too late */
static SemaphoreHandle_t s_lock;
static void (*s_listener)(const char *topic, gw_notify_level_t level, const char *text);

static cJSON *entry_json(const entry_t *e)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "seq", e->seq);
    cJSON_AddNumberToObject(json, "boot", s_boot);
    cJSON_AddStringToObject(json, "topic", e->topic);
    cJSON_AddStringToObject(json, "level", s_levels[e->level]);
    cJSON_AddStringToObject(json, "text", e->text);
    cJSON_AddNumberToObject(json, "time", (double)e->time);
    return json;
}

void gw_notify(const char *topic, gw_notify_level_t level, const char *fmt, ...)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
        s_boot = esp_random() & 0x7fffffff;
        s_ring = heap_caps_calloc(NOTIFY_KEEP, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    }
    entry_t e = {.level = level};
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.text, sizeof(e.text), fmt, ap);
    va_end(ap);
    strlcpy(e.topic, topic, sizeof(e.topic));
    time_t t = time(NULL);
    e.time = t > 1700000000 ? (int64_t)t : 0;
    ESP_LOG_LEVEL(level == GW_NOTIFY_INFO ? ESP_LOG_INFO : ESP_LOG_WARN, TAG, "[%s] %s", topic, e.text);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    e.seq = ++s_seq;
    if (s_ring) {
        s_ring[e.seq % NOTIFY_KEEP] = e;
    }
    xSemaphoreGive(s_lock);

    cJSON *json = entry_json(&e);
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (payload) {
        gw_mqtt_publish("notify", payload, false);
        cJSON_free(payload);
    }
    if (s_listener) {
        s_listener(e.topic, level, e.text);
    }
}

void gw_notify_set_listener(void (*listener)(const char *topic, gw_notify_level_t level, const char *text))
{
    s_listener = listener;
}

/* The first notification of a run: why the gateway started. */
void gw_notify_boot(bool after_failure)
{
    esp_reset_reason_t r = esp_reset_reason();
    const char *version = esp_app_get_description()->version;
    switch (r) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
        gw_notify("system", GW_NOTIFY_CRITICAL, "Gateway restarted after a crash (%s), %s; core dump under System > Diagnostics",
                  r == ESP_RST_PANIC ? "panic" : "watchdog", version);
        break;
    case ESP_RST_BROWNOUT:
        gw_notify("system", GW_NOTIFY_CRITICAL, "Gateway restarted after a supply voltage drop (brownout), %s", version);
        break;
    case ESP_RST_POWERON:
        gw_notify("system", GW_NOTIFY_WARNING, "Gateway powered on, %s", version);
        break;
    default:
        if (after_failure) {
            gw_notify("system", GW_NOTIFY_WARNING, "Gateway restarted to recover the C6 radio, %s", version);
        } else {
            gw_notify("system", GW_NOTIFY_INFO, "Gateway restarted, %s", version);
        }
        break;
    }
}

/* GET /api/notify?since=seq: kept notifications newer than seq, oldest first, and the last seq */
static esp_err_t notify_get(httpd_req_t *req)
{
    char query[32], value[12];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        since = strtoul(value, NULL, 10);
    }
    cJSON *json = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(json, "notifications");
    if (s_lock) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        uint32_t first = s_seq > NOTIFY_KEEP ? s_seq - NOTIFY_KEEP + 1 : 1;
        for (uint32_t seq = since + 1 > first ? since + 1 : first; seq <= s_seq && s_ring; seq++) {
            cJSON_AddItemToArray(list, entry_json(&s_ring[seq % NOTIFY_KEEP]));
        }
        xSemaphoreGive(s_lock);
    }
    cJSON_AddNumberToObject(json, "seq", s_seq);
    cJSON_AddNumberToObject(json, "boot", s_boot);
    return gw_http_send_json(req, json);
}

void gw_notify_register(void)
{
    gw_http_register("/api/notify", HTTP_GET, notify_get);
}
