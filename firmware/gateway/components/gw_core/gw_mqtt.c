#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "mqtt_client.h"

/*
 * Optional MQTT link with Home Assistant discovery: gateway state as entities, role select
 * and restart button as commands. Configured from the web UI, stored in NVS.
 */

static const char *TAG = "gw_mqtt";

#define KEY_ENABLED "mqtt_en"
#define KEY_URI "mqtt_uri"
#define KEY_USER "mqtt_user"
#define KEY_PASS "mqtt_pass"
#define KEY_PREFIX "mqtt_prefix"
#define PUBLISH_INTERVAL_MS 30000

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static bool s_connected;
static char s_base[64];   /* p4gw/<hostname> */
static char s_prefix[32]; /* discovery prefix */
static char s_last_error[64];
static uint32_t s_publishes;

static void topic(char *out, size_t len, const char *suffix)
{
    snprintf(out, len, "%s/%s", s_base, suffix);
}

static cJSON *device_json(void)
{
    const esp_app_desc_t *app = esp_app_get_description();
    gw_state_t *st = gw_state();
    cJSON *dev = cJSON_CreateObject();
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    cJSON_AddItemToArray(ids, cJSON_CreateString(st->hostname));
    cJSON_AddStringToObject(dev, "name", st->hostname);
    cJSON_AddStringToObject(dev, "manufacturer", "Guition");
    cJSON_AddStringToObject(dev, "model", "JC-ESP32P4-M3-DEV gateway");
    cJSON_AddStringToObject(dev, "sw_version", app->version);
    cJSON *eth = gw_eth_status();
    const char *ip = GW_JSON_STR(eth, "ipv4");
    if (ip) {
        char url[48];
        snprintf(url, sizeof(url), "http://%s/", ip);
        cJSON_AddStringToObject(dev, "configuration_url", url);
    }
    cJSON_Delete(eth);
    return dev;
}

/* One retained discovery config per entity; HA builds the device from them. */
static void publish_entity(const char *component, const char *object_id, const char *name, const char *value_template,
                           void (*extra)(cJSON *))
{
    gw_state_t *st = gw_state();
    char buf[128];
    cJSON *cfg = cJSON_CreateObject();

    cJSON_AddStringToObject(cfg, "name", name);
    snprintf(buf, sizeof(buf), "%s_%s", st->hostname, object_id);
    cJSON_AddStringToObject(cfg, "unique_id", buf);
    snprintf(buf, sizeof(buf), "%s_%s", st->hostname, object_id);
    cJSON_AddStringToObject(cfg, "object_id", buf);
    topic(buf, sizeof(buf), "availability");
    cJSON_AddStringToObject(cfg, "availability_topic", buf);
    if (value_template) {
        topic(buf, sizeof(buf), "state");
        cJSON_AddStringToObject(cfg, "state_topic", buf);
        cJSON_AddStringToObject(cfg, "value_template", value_template);
    }
    if (extra) {
        extra(cfg);
    }
    cJSON_AddItemToObject(cfg, "device", device_json());

    char *payload = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/config", s_prefix, component, st->hostname, object_id);
    esp_mqtt_client_publish(s_client, buf, payload, 0, 1, 1);
    cJSON_free(payload);
}

static void diag(cJSON *cfg)
{
    cJSON_AddStringToObject(cfg, "entity_category", "diagnostic");
}

static void uptime_extra(cJSON *cfg)
{
    diag(cfg);
    cJSON_AddStringToObject(cfg, "device_class", "duration");
    cJSON_AddStringToObject(cfg, "unit_of_measurement", "s");
    cJSON_AddStringToObject(cfg, "state_class", "total_increasing");
}

static void temp_extra(cJSON *cfg)
{
    diag(cfg);
    cJSON_AddStringToObject(cfg, "device_class", "temperature");
    cJSON_AddStringToObject(cfg, "unit_of_measurement", "°C");
    cJSON_AddStringToObject(cfg, "state_class", "measurement");
}

static void heap_extra(cJSON *cfg)
{
    diag(cfg);
    cJSON_AddStringToObject(cfg, "unit_of_measurement", "B");
    cJSON_AddStringToObject(cfg, "state_class", "measurement");
}

static void connectivity_extra(cJSON *cfg)
{
    cJSON_AddStringToObject(cfg, "device_class", "connectivity");
    cJSON_AddStringToObject(cfg, "payload_on", "ON");
    cJSON_AddStringToObject(cfg, "payload_off", "OFF");
}

static void role_select_extra(cJSON *cfg)
{
    char buf[96];
    topic(buf, sizeof(buf), "role/set");
    cJSON_AddStringToObject(cfg, "command_topic", buf);
    cJSON *options = cJSON_AddArrayToObject(cfg, "options");
    for (int r = GW_ROLE_THREAD_BR; r < GW_ROLE_MAX; r++) {
        cJSON_AddItemToArray(options, cJSON_CreateString(gw_role_name(r)));
    }
    cJSON_AddStringToObject(cfg, "icon", "mdi:swap-horizontal");
}

static void restart_extra(cJSON *cfg)
{
    char buf[96];
    topic(buf, sizeof(buf), "restart/set");
    cJSON_AddStringToObject(cfg, "command_topic", buf);
    cJSON_AddStringToObject(cfg, "device_class", "restart");
    cJSON_AddStringToObject(cfg, "entity_category", "config");
}

static void publish_discovery(void)
{
    publish_entity("sensor", "role", "Active role", "{{ value_json.role }}", NULL);
    publish_entity("select", "desired_role", "Role", "{{ value_json.desired }}", role_select_extra);
    publish_entity("sensor", "maintenance", "Maintenance reason", "{{ value_json.maintenance }}", diag);
    publish_entity("sensor", "uptime", "Uptime", "{{ value_json.uptime }}", uptime_extra);
    publish_entity("sensor", "temperature", "Chip temperature", "{{ value_json.temperature }}", temp_extra);
    publish_entity("sensor", "heap_free", "Free internal heap", "{{ value_json.heap_free }}", heap_extra);
    publish_entity("binary_sensor", "ethernet", "Ethernet", "{{ value_json.eth_link }}", connectivity_extra);
    publish_entity("sensor", "ip", "IP address", "{{ value_json.ipv4 }}", diag);
    publish_entity("sensor", "c6_firmware", "C6 firmware", "{{ value_json.c6 }}", diag);
    publish_entity("sensor", "thread_state", "Thread state", "{{ value_json.thread_role | default('n/a') }}", NULL);
    publish_entity("binary_sensor", "zigbee_client", "Zigbee host connected", "{{ value_json.zigbee_client }}",
                   connectivity_extra);
    publish_entity("button", "restart", "Restart", NULL, restart_extra);
}

static void publish_state(void)
{
    gw_state_t *st = gw_state();
    cJSON *state = cJSON_CreateObject();
    char buf[96];

    cJSON_AddStringToObject(state, "role", gw_role_name(st->active));
    cJSON_AddStringToObject(state, "desired", gw_role_name(st->desired));
    cJSON_AddStringToObject(state, "maintenance", st->maintenance[0] ? st->maintenance : "none");
    cJSON_AddNumberToObject(state, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddNumberToObject(state, "heap_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    float temp;
    if (gw_api_temperature(&temp)) {
        cJSON_AddNumberToObject(state, "temperature", (int)(temp * 10) / 10.0);
    }
    cJSON *eth = gw_eth_status();
    cJSON_AddStringToObject(state, "eth_link", cJSON_IsTrue(cJSON_GetObjectItem(eth, "link")) ? "ON" : "OFF");
    const char *ip = GW_JSON_STR(eth, "ipv4");
    cJSON_AddStringToObject(state, "ipv4", ip ? ip : "");
    cJSON_Delete(eth);
    gw_config_get_str(GW_KEY_C6_BUNDLE, buf, sizeof(buf));
    cJSON_AddStringToObject(state, "c6", buf);

    cJSON *role = gw_api_role_status();
    if (role) {
        /* Only the Thread roles report an OpenThread device role. */
        const char *thread_role = GW_JSON_STR(role, "role");
        if (thread_role) {
            cJSON_AddStringToObject(state, "thread_role", thread_role);
        }
        cJSON *client = cJSON_GetObjectItem(role, "client_connected");
        if (client) {
            cJSON_AddStringToObject(state, "zigbee_client", cJSON_IsTrue(client) ? "ON" : "OFF");
        }
        cJSON_Delete(role);
    }

    char *payload = cJSON_PrintUnformatted(state);
    cJSON_Delete(state);
    topic(buf, sizeof(buf), "state");
    esp_mqtt_client_publish(s_client, buf, payload, 0, 0, 1);
    cJSON_free(payload);
    s_publishes++;
}

static void handle_command(const char *t, int tlen, const char *data, int dlen)
{
    char name[64], payload[32];
    snprintf(name, sizeof(name), "%.*s", tlen, t);
    snprintf(payload, sizeof(payload), "%.*s", dlen, data);
    char expect[96];

    topic(expect, sizeof(expect), "role/set");
    if (strcmp(name, expect) == 0) {
        gw_role_t role;
        if (!gw_role_from_name(payload, &role) || role == GW_ROLE_NONE) {
            ESP_LOGW(TAG, "ignoring role '%s'", payload);
            return;
        }
        if (role == gw_state()->desired && role == gw_state()->active) {
            return;
        }
        ESP_LOGW(TAG, "role switch to %s via MQTT", payload);
        gw_config_set_u8(GW_KEY_ROLE, role);
        gw_config_set_u8(GW_KEY_C6_TRIES, 0);
        gw_restart_later(1000);
        return;
    }
    topic(expect, sizeof(expect), "restart/set");
    if (strcmp(name, expect) == 0) {
        gw_restart_later(1000);
    }
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    esp_mqtt_event_handle_t event = data;
    char buf[96];

    switch ((esp_mqtt_event_id_t)id) {
    case MQTT_EVENT_CONNECTED:
        s_connected = true;
        s_last_error[0] = '\0';
        ESP_LOGI(TAG, "connected");
        topic(buf, sizeof(buf), "availability");
        esp_mqtt_client_publish(s_client, buf, "online", 0, 1, 1);
        topic(buf, sizeof(buf), "role/set");
        esp_mqtt_client_subscribe(s_client, buf, 1);
        topic(buf, sizeof(buf), "restart/set");
        esp_mqtt_client_subscribe(s_client, buf, 1);
        publish_discovery();
        publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        handle_command(event->topic, event->topic_len, event->data, event->data_len);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            snprintf(s_last_error, sizeof(s_last_error), "connection refused (code %d)",
                     event->error_handle->connect_return_code);
        } else {
            snprintf(s_last_error, sizeof(s_last_error), "transport error (errno %d)",
                     event->error_handle->esp_transport_sock_errno);
        }
        break;
    default:
        break;
    }
}

static void stop_client(void)
{
    if (s_client) {
        esp_mqtt_client_stop(s_client);
        esp_mqtt_client_destroy(s_client);
        s_client = NULL;
        s_connected = false;
    }
}

static void start_client(void)
{
    char uri[128], user[64], pass[64], lwt[96];
    gw_state_t *st = gw_state();

    if (!gw_config_get_u8(KEY_ENABLED, 0) || !gw_config_get_str(KEY_URI, uri, sizeof(uri)) || !uri[0]) {
        return;
    }
    gw_config_get_str(KEY_USER, user, sizeof(user));
    gw_config_get_str(KEY_PASS, pass, sizeof(pass));
    if (!gw_config_get_str(KEY_PREFIX, s_prefix, sizeof(s_prefix)) || !s_prefix[0]) {
        strlcpy(s_prefix, "homeassistant", sizeof(s_prefix));
    }
    snprintf(s_base, sizeof(s_base), "p4gw/%s", st->hostname);
    topic(lwt, sizeof(lwt), "availability");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = user[0] ? user : NULL,
        .credentials.client_id = st->hostname,
        .credentials.authentication.password = pass[0] ? pass : NULL,
        .session.last_will = {.topic = lwt, .msg = "offline", .qos = 1, .retain = 1},
        .session.keepalive = 30,
        .network.reconnect_timeout_ms = 10000,
        .task.stack_size = 6144,
    };
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        strlcpy(s_last_error, "invalid configuration", sizeof(s_last_error));
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "broker %s", uri);
}

static void publish_task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(PUBLISH_INTERVAL_MS));
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_client && s_connected) {
            publish_state();
        }
        xSemaphoreGive(s_lock);
    }
}

static esp_err_t mqtt_get(httpd_req_t *req)
{
    char buf[128];
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", gw_config_get_u8(KEY_ENABLED, 0));
    gw_config_get_str(KEY_URI, buf, sizeof(buf));
    cJSON_AddStringToObject(json, "uri", buf);
    gw_config_get_str(KEY_USER, buf, sizeof(buf));
    cJSON_AddStringToObject(json, "user", buf);
    cJSON_AddBoolToObject(json, "password_set", gw_config_get_str(KEY_PASS, buf, sizeof(buf)) && buf[0]);
    cJSON_AddStringToObject(json, "prefix", s_prefix[0] ? s_prefix : "homeassistant");
    cJSON_AddStringToObject(json, "base_topic", s_base);
    cJSON_AddBoolToObject(json, "connected", s_connected);
    cJSON_AddStringToObject(json, "last_error", s_last_error);
    cJSON_AddNumberToObject(json, "publishes", s_publishes);
    return gw_http_send_json(req, json);
}

static esp_err_t mqtt_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *uri = GW_JSON_STR(body, "uri");
    const char *user = GW_JSON_STR(body, "user");
    const char *pass = GW_JSON_STR(body, "password");
    const char *prefix = GW_JSON_STR(body, "prefix");
    cJSON *enabled = cJSON_GetObjectItem(body, "enabled");

    if ((uri && (strlen(uri) > 120 || (strncmp(uri, "mqtt://", 7) && strncmp(uri, "mqtts://", 8)))) ||
        (user && strlen(user) > 63) || (pass && strlen(pass) > 63) || (prefix && strlen(prefix) > 31)) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "uri must start with mqtt:// or mqtts://, fields max 63 chars");
    }
    if (uri) {
        gw_config_set_str(KEY_URI, uri);
    }
    if (user) {
        gw_config_set_str(KEY_USER, user);
    }
    if (pass) {
        gw_config_set_str(KEY_PASS, pass);
    }
    if (prefix) {
        gw_config_set_str(KEY_PREFIX, prefix);
    }
    if (cJSON_IsBool(enabled)) {
        gw_config_set_u8(KEY_ENABLED, cJSON_IsTrue(enabled));
    }
    cJSON_Delete(body);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    stop_client();
    start_client();
    xSemaphoreGive(s_lock);
    return mqtt_get(req);
}

esp_err_t gw_mqtt_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    gw_http_register("/api/mqtt", HTTP_GET, mqtt_get);
    gw_http_register("/api/mqtt", HTTP_POST, mqtt_post);
    snprintf(s_base, sizeof(s_base), "p4gw/%s", gw_state()->hostname);
    start_client();
    return xTaskCreate(publish_task, "mqtt_pub", 4096, NULL, 3, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
