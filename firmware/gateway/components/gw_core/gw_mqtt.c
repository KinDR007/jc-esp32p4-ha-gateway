#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
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
#define KEY_TLS "mqtt_tls"       /* str, "ca": own CA certificate, else the public CA bundle */
#define KEY_CA "mqtt_ca"         /* str, PEM of the own CA (or the broker's self-signed certificate) */
#define KEY_NO_CN "mqtt_no_cn"   /* u8, do not check the server name in its certificate */
#define KEY_CLIENT_ID "mqtt_cid" /* str, empty = hostname */
#define KEY_KEEPALIVE "mqtt_ka"  /* u8 seconds, 0 = default */
#define KEY_BASE "mqtt_base"     /* str, empty = p4gw/<hostname> */
#define KEY_DISCOVERY "mqtt_disc" /* u8, 0 = no Home Assistant discovery (entities removed) */
#define KEY_INTERVAL "mqtt_int"  /* str seconds, state publish interval, empty = default */
#define DEFAULT_KEEPALIVE 30
#define DEFAULT_INTERVAL_S 30
#define CA_MAX 3999 /* NVS strings: 4000 bytes with the terminator */

static esp_mqtt_client_handle_t s_client;
static SemaphoreHandle_t s_lock;
static bool s_connected;
static char s_base[64];   /* p4gw/<hostname> */
static char s_prefix[32]; /* discovery prefix */
static char s_last_error[96];
static uint32_t s_publishes;
static char *s_ca;          /* esp-mqtt keeps the pointer, not a copy */
static bool s_discovery = true;
static uint32_t s_interval_s = DEFAULT_INTERVAL_S;
static TaskHandle_t s_publish_task;

static bool has_prefix(const char *s, const char *p)
{
    return strncmp(s, p, strlen(p)) == 0;
}

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
/* Listener of other components (Zigbee2MQTT): subscribes on every connect, gets whole messages. */
#define LISTENER_MAX_MSG (768 * 1024)
static gw_mqtt_listener_t s_listener;
static char *s_rx_buf, s_rx_topic[160];
static size_t s_rx_len;

void gw_mqtt_set_listener(const gw_mqtt_listener_t *listener)
{
    s_listener = *listener;
}

esp_err_t gw_mqtt_subscribe(const char *filter)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_subscribe(s_client, filter, 0) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_mqtt_unsubscribe(const char *filter)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_unsubscribe(s_client, filter) >= 0 ? ESP_OK : ESP_FAIL;
}

esp_err_t gw_mqtt_publish_to(const char *full_topic, const char *payload)
{
    if (!s_client || !s_connected) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_mqtt_client_publish(s_client, full_topic, payload, 0, 1, 0) >= 0 ? ESP_OK : ESP_FAIL;
}

/* A message can arrive in several chunks (bigger than the client buffer): put it together in PSRAM. */
static void listener_data(esp_mqtt_event_handle_t e)
{
    if (!s_listener.on_message) {
        return;
    }
    if (e->current_data_offset == 0) {
        free(s_rx_buf);
        s_rx_buf = NULL;
        if (e->total_data_len > LISTENER_MAX_MSG || e->topic_len >= (int)sizeof(s_rx_topic)) {
            ESP_LOGW(TAG, "dropping %d B message on %.*s", e->total_data_len, e->topic_len, e->topic);
            return;
        }
        s_rx_buf = heap_caps_malloc(e->total_data_len + 1, MALLOC_CAP_SPIRAM);
        snprintf(s_rx_topic, sizeof(s_rx_topic), "%.*s", e->topic_len, e->topic);
        s_rx_len = e->total_data_len;
    }
    if (!s_rx_buf || e->current_data_offset + e->data_len > (int)s_rx_len) {
        return;
    }
    memcpy(s_rx_buf + e->current_data_offset, e->data, e->data_len);
    if (e->current_data_offset + e->data_len == (int)s_rx_len) {
        s_rx_buf[s_rx_len] = '\0';
        s_listener.on_message(s_rx_topic, s_rx_buf, s_rx_len);
        free(s_rx_buf);
        s_rx_buf = NULL;
    }
}

static void (*s_ext_discovery)(void);
static void (*s_ext_state)(cJSON *state);

void gw_mqtt_set_extension(void (*discovery)(void), void (*state)(cJSON *state))
{
    s_ext_discovery = discovery;
    s_ext_state = state;
}

bool gw_mqtt_connected(void)
{
    return s_client && s_connected;
}

const char *gw_mqtt_link(void)
{
    return !s_client ? "off" : s_connected ? "connected" : "disconnected";
}

void gw_mqtt_publish(const char *suffix, const char *payload, bool retain)
{
    char buf[128];
    gw_http_push(suffix, payload);
    if (!gw_mqtt_connected()) {
        return;
    }
    topic(buf, sizeof(buf), suffix);
    esp_mqtt_client_publish(s_client, buf, payload, 0, 1, retain);
}

const char *gw_mqtt_base_topic(void)
{
    return s_base;
}

void gw_mqtt_discovery(const char *component, const char *object_id, cJSON *cfg)
{
    gw_state_t *st = gw_state();
    char buf[160];

    if (!gw_mqtt_connected()) {
        cJSON_Delete(cfg);
        return;
    }
    snprintf(buf, sizeof(buf), "%s/%s/%s/%s/config", s_prefix, component, st->hostname, object_id);
    if (!s_discovery) {
        cJSON_Delete(cfg);
        cfg = NULL; /* discovery switched off: remove what an earlier run created */
    }
    if (!cfg) {
        /* Empty retained config removes the entity from Home Assistant. */
        esp_mqtt_client_publish(s_client, buf, "", 0, 1, 1);
        return;
    }
    char id[96];
    snprintf(id, sizeof(id), "%s_%s", st->hostname, object_id);
    if (strcmp(component, "device_automation") != 0) {
        cJSON_AddStringToObject(cfg, "unique_id", id);
        cJSON_AddStringToObject(cfg, "object_id", id);
        char avail[96];
        topic(avail, sizeof(avail), "availability");
        cJSON_AddStringToObject(cfg, "availability_topic", avail);
    }
    cJSON_AddItemToObject(cfg, "device", device_json());
    char *payload = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    esp_mqtt_client_publish(s_client, buf, payload, 0, 1, 1);
    cJSON_free(payload);
}

void gw_mqtt_entity(const char *component, const char *object_id, const char *name, const char *value_template,
                           void (*extra)(cJSON *))
{
    char buf[128];
    cJSON *cfg = cJSON_CreateObject();

    cJSON_AddStringToObject(cfg, "name", name);
    if (value_template) {
        topic(buf, sizeof(buf), "state");
        cJSON_AddStringToObject(cfg, "state_topic", buf);
        cJSON_AddStringToObject(cfg, "value_template", value_template);
    }
    if (extra) {
        extra(cfg);
    }
    gw_mqtt_discovery(component, object_id, cfg); /* adds ids, availability and device */
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
    gw_mqtt_entity("sensor", "role", "Active role", "{{ value_json.role }}", NULL);
    gw_mqtt_entity("select", "desired_role", "Role", "{{ value_json.desired }}", role_select_extra);
    gw_mqtt_entity("sensor", "maintenance", "Maintenance reason", "{{ value_json.maintenance }}", diag);
    gw_mqtt_entity("sensor", "uptime", "Uptime", "{{ value_json.uptime }}", uptime_extra);
    gw_mqtt_entity("sensor", "temperature", "Chip temperature", "{{ value_json.temperature }}", temp_extra);
    gw_mqtt_entity("sensor", "heap_free", "Free internal heap", "{{ value_json.heap_free }}", heap_extra);
    gw_mqtt_entity("binary_sensor", "ethernet", "Ethernet", "{{ value_json.eth_link }}", connectivity_extra);
    gw_mqtt_entity("sensor", "ip", "IP address", "{{ value_json.ipv4 }}", diag);
    gw_mqtt_entity("sensor", "c6_firmware", "C6 firmware", "{{ value_json.c6 }}", diag);
    gw_mqtt_entity("sensor", "thread_state", "Thread state", "{{ value_json.thread_role | default('n/a') }}", NULL);
    gw_mqtt_entity("binary_sensor", "zigbee_client", "Zigbee host connected", "{{ value_json.zigbee_client }}",
                   connectivity_extra);
    gw_mqtt_entity("button", "restart", "Restart", NULL, restart_extra);
    gw_update_mqtt_discovery();
    if (s_ext_discovery) {
        s_ext_discovery();
    }
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

    if (s_ext_state) {
        s_ext_state(state);
    }
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
    char name[96], payload[32];
    snprintf(name, sizeof(name), "%.*s", tlen, t);
    snprintf(payload, sizeof(payload), "%.*s", dlen, data);
    char expect[96];

    /* Home Assistant came (back) online: its retained discovery may be gone (broker without persistence). */
    snprintf(expect, sizeof(expect), "%s/status", s_prefix);
    if (strcmp(name, expect) == 0) {
        if (strcmp(payload, "online") == 0) {
            ESP_LOGI(TAG, "Home Assistant online, discovery again");
            publish_discovery();
            publish_state();
            gw_update_mqtt_state();
        }
        return;
    }

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
    topic(expect, sizeof(expect), "update/install");
    if (strcmp(name, expect) == 0) {
        esp_err_t err = gw_update_install();
        ESP_LOGW(TAG, "update via MQTT: %s", esp_err_to_name(err));
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
        topic(buf, sizeof(buf), "update/install");
        esp_mqtt_client_subscribe(s_client, buf, 1);
        if (s_discovery) {
            snprintf(buf, sizeof(buf), "%s/status", s_prefix);
            esp_mqtt_client_subscribe(s_client, buf, 1);
        }
        publish_discovery();
        publish_state();
        gw_update_mqtt_state();
        if (s_listener.on_connect) {
            s_listener.on_connect();
        }
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_connected = false;
        ESP_LOGW(TAG, "disconnected");
        break;
    case MQTT_EVENT_DATA:
        if (event->current_data_offset == 0 && event->topic_len) {
            handle_command(event->topic, event->topic_len, event->data, event->data_len);
        }
        listener_data(event);
        break;
    case MQTT_EVENT_ERROR:
        if (event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED) {
            snprintf(s_last_error, sizeof(s_last_error), "connection refused (code %d)",
                     event->error_handle->connect_return_code);
        } else if (event->error_handle->esp_tls_last_esp_err || event->error_handle->esp_tls_stack_err) {
            /* mbedTLS codes are negative: -0x2700 certificate verification failed (CA or name),
             * -0x3000 not trusted by the certificate bundle */
            int code = event->error_handle->esp_tls_stack_err;
            code = code < 0 ? -code : code;
            const char *why = code == 0x2700 ? "server certificate not accepted (CA or server name)"
                              : code == 0x3000 ? "server certificate not signed by a public CA"
                                               : esp_err_to_name(event->error_handle->esp_tls_last_esp_err);
            snprintf(s_last_error, sizeof(s_last_error), "TLS: %s (mbedTLS -0x%04x)", why, code);
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
    char uri[128], user[64], pass[64], lwt[96], client_id[64], tls[8], base[64], interval[8];
    gw_state_t *st = gw_state();

    s_last_error[0] = '\0';
    if (!gw_config_get_u8(KEY_ENABLED, 0) || !gw_config_get_str(KEY_URI, uri, sizeof(uri)) || !uri[0]) {
        return;
    }
    gw_config_get_str(KEY_USER, user, sizeof(user));
    gw_config_get_str(KEY_PASS, pass, sizeof(pass));
    if (!gw_config_get_str(KEY_PREFIX, s_prefix, sizeof(s_prefix)) || !s_prefix[0]) {
        strlcpy(s_prefix, "homeassistant", sizeof(s_prefix));
    }
    if (gw_config_get_str(KEY_BASE, base, sizeof(base)) && base[0]) {
        strlcpy(s_base, base, sizeof(s_base));
    } else {
        snprintf(s_base, sizeof(s_base), "p4gw/%s", st->hostname);
    }
    if (!gw_config_get_str(KEY_CLIENT_ID, client_id, sizeof(client_id)) || !client_id[0]) {
        strlcpy(client_id, st->hostname, sizeof(client_id));
    }
    s_discovery = gw_config_get_u8(KEY_DISCOVERY, 1);
    s_interval_s = gw_config_get_str(KEY_INTERVAL, interval, sizeof(interval)) && atoi(interval) >= 10
                       ? atoi(interval) : DEFAULT_INTERVAL_S;
    uint8_t keepalive = gw_config_get_u8(KEY_KEEPALIVE, 0);
    topic(lwt, sizeof(lwt), "availability");

    esp_mqtt_client_config_t cfg = {
        .broker.address.uri = uri,
        .credentials.username = user[0] ? user : NULL,
        .credentials.client_id = client_id,
        .credentials.authentication.password = pass[0] ? pass : NULL,
        .session.last_will = {.topic = lwt, .msg = "offline", .qos = 1, .retain = 1},
        .session.keepalive = keepalive ? keepalive : DEFAULT_KEEPALIVE,
        .network.reconnect_timeout_ms = 10000,
        .task.stack_size = 6144,
    };
    /* TLS (mqtts://, wss://): the server certificate is always verified, by the public CA bundle or
     * by an own CA; a self-signed broker certificate works as its own CA. */
    free(s_ca);
    s_ca = NULL;
    /* only for a TLS scheme: esp-mqtt refuses TLS settings with mqtt:// or ws:// */
    bool secure = has_prefix(uri, "mqtts://") || has_prefix(uri, "wss://");
    if (secure && gw_config_get_str(KEY_TLS, tls, sizeof(tls)) && strcmp(tls, "ca") == 0) {
        s_ca = heap_caps_malloc(CA_MAX + 1, MALLOC_CAP_SPIRAM);
        if (s_ca && !gw_config_get_str(KEY_CA, s_ca, CA_MAX + 1)) {
            free(s_ca);
            s_ca = NULL;
        }
    }
    if (s_ca) {
        cfg.broker.verification.certificate = s_ca;
    } else if (secure) {
        cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    cfg.broker.verification.skip_cert_common_name_check = secure && gw_config_get_u8(KEY_NO_CN, 0);
    s_client = esp_mqtt_client_init(&cfg);
    if (!s_client) {
        strlcpy(s_last_error, "invalid configuration", sizeof(s_last_error));
        return;
    }
    esp_mqtt_client_register_event(s_client, ESP_EVENT_ANY_ID, on_event, NULL);
    esp_mqtt_client_start(s_client);
    ESP_LOGI(TAG, "broker %s as %s%s", uri, client_id, s_ca ? ", own CA" : "");
}

static void publish_task(void *arg)
{
    while (true) {
        /* woken by a settings change, so a new interval applies at once */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_interval_s * 1000));
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
    gw_config_get_str(KEY_TLS, buf, sizeof(buf));
    cJSON_AddStringToObject(json, "tls", strcmp(buf, "ca") == 0 ? "ca" : "bundle");
    char *ca = heap_caps_malloc(CA_MAX + 1, MALLOC_CAP_SPIRAM);
    cJSON_AddStringToObject(json, "ca", ca && gw_config_get_str(KEY_CA, ca, CA_MAX + 1) ? ca : "");
    free(ca);
    cJSON_AddBoolToObject(json, "skip_cn_check", gw_config_get_u8(KEY_NO_CN, 0));
    gw_config_get_str(KEY_CLIENT_ID, buf, sizeof(buf));
    cJSON_AddStringToObject(json, "client_id", buf);
    cJSON_AddNumberToObject(json, "keepalive", gw_config_get_u8(KEY_KEEPALIVE, 0) ?: DEFAULT_KEEPALIVE);
    gw_config_get_str(KEY_BASE, buf, sizeof(buf));
    cJSON_AddStringToObject(json, "base", buf);
    cJSON_AddBoolToObject(json, "discovery", gw_config_get_u8(KEY_DISCOVERY, 1));
    cJSON_AddNumberToObject(json, "interval", s_interval_s);
    cJSON_AddStringToObject(json, "base_topic", s_base);
    cJSON_AddBoolToObject(json, "connected", s_connected);
    cJSON_AddStringToObject(json, "last_error", s_last_error);
    cJSON_AddNumberToObject(json, "publishes", s_publishes);
    return gw_http_send_json(req, json);
}

static bool valid_topic(const char *t)
{
    size_t n = strlen(t);
    return n < 64 && !strpbrk(t, "#+ ") && (n == 0 || (t[0] != '/' && t[n - 1] != '/'));
}

static bool ca_stored(void)
{
    char *ca = heap_caps_malloc(CA_MAX + 1, MALLOC_CAP_SPIRAM);
    bool stored = ca && gw_config_get_str(KEY_CA, ca, CA_MAX + 1) && ca[0];
    free(ca);
    return stored;
}


static esp_err_t mqtt_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json_max(req, 8192); /* the CA certificate */
    if (!body) {
        return ESP_OK;
    }
    const char *uri = GW_JSON_STR(body, "uri");
    const char *user = GW_JSON_STR(body, "user");
    const char *pass = GW_JSON_STR(body, "password");
    const char *prefix = GW_JSON_STR(body, "prefix");
    const char *tls = GW_JSON_STR(body, "tls");
    const char *ca = GW_JSON_STR(body, "ca");
    const char *client_id = GW_JSON_STR(body, "client_id");
    const char *base = GW_JSON_STR(body, "base");
    cJSON *enabled = cJSON_GetObjectItem(body, "enabled");
    cJSON *no_cn = cJSON_GetObjectItem(body, "skip_cn_check");
    cJSON *discovery = cJSON_GetObjectItem(body, "discovery");
    cJSON *keepalive = cJSON_GetObjectItem(body, "keepalive");
    cJSON *interval = cJSON_GetObjectItem(body, "interval");
    const char *error = NULL;
    bool uses_ca = tls ? strcmp(tls, "ca") == 0 : false;

    if (uri && (strlen(uri) > 120 || !(has_prefix(uri, "mqtt://") || has_prefix(uri, "mqtts://") ||
                                       has_prefix(uri, "ws://") || has_prefix(uri, "wss://")))) {
        error = "uri: mqtt://, mqtts://, ws:// or wss://host:port, max 120 characters";
    } else if ((user && strlen(user) > 63) || (pass && strlen(pass) > 63) || (client_id && strlen(client_id) > 63)) {
        error = "user, password and client ID: max 63 characters";
    } else if (prefix && (!valid_topic(prefix) || !prefix[0] || strlen(prefix) > 31)) {
        error = "prefix: a topic without wildcards and outer slashes, max 31 characters";
    } else if (base && !valid_topic(base)) {
        error = "base topic: without #, +, spaces and outer slashes, max 63 characters";
    } else if (tls && strcmp(tls, "ca") != 0 && strcmp(tls, "bundle") != 0) {
        error = "tls: bundle (public CA) or ca (own certificate)";
    } else if (ca && ca[0] && (strlen(ca) > CA_MAX || !strstr(ca, "-----BEGIN CERTIFICATE-----"))) {
        error = "ca: PEM certificate (-----BEGIN CERTIFICATE-----), max 3999 characters";
    } else if (uses_ca && !(ca && ca[0]) && !ca_stored()) {
        error = "own CA selected: paste its certificate";
    } else if (keepalive && (!cJSON_IsNumber(keepalive) || keepalive->valueint < 10 || keepalive->valueint > 240)) {
        error = "keepalive: 10-240 s";
    } else if (interval && (!cJSON_IsNumber(interval) || interval->valueint < 10 || interval->valueint > 3600)) {
        error = "interval: 10-3600 s";
    }
    if (error) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, error);
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
    if (tls) {
        gw_config_set_str(KEY_TLS, tls);
    }
    if (ca) {
        gw_config_set_str(KEY_CA, ca);
    }
    if (client_id) {
        gw_config_set_str(KEY_CLIENT_ID, client_id);
    }
    if (base) {
        gw_config_set_str(KEY_BASE, base);
    }
    if (keepalive) {
        gw_config_set_u8(KEY_KEEPALIVE, keepalive->valueint);
    }
    if (interval) {
        char text[8];
        snprintf(text, sizeof(text), "%d", interval->valueint);
        gw_config_set_str(KEY_INTERVAL, text);
    }
    if (cJSON_IsBool(no_cn)) {
        gw_config_set_u8(KEY_NO_CN, cJSON_IsTrue(no_cn));
    }
    if (cJSON_IsBool(discovery)) {
        gw_config_set_u8(KEY_DISCOVERY, cJSON_IsTrue(discovery));
    }
    if (cJSON_IsBool(enabled)) {
        gw_config_set_u8(KEY_ENABLED, cJSON_IsTrue(enabled));
    }
    cJSON_Delete(body);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* Discovery switched off: remove the entities while still connected with the old settings. */
    if (cJSON_IsBool(discovery) && !gw_config_get_u8(KEY_DISCOVERY, 1) && s_discovery && gw_mqtt_connected()) {
        s_discovery = false;
        publish_discovery();
        vTaskDelay(pdMS_TO_TICKS(500)); /* let the removals go out before the client stops */
    }
    stop_client();
    start_client();
    xSemaphoreGive(s_lock);
    if (s_publish_task) {
        xTaskNotifyGive(s_publish_task);
    }
    return mqtt_get(req);
}

esp_err_t gw_mqtt_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    gw_http_register("/api/mqtt", HTTP_GET, mqtt_get);
    gw_http_register("/api/mqtt", HTTP_POST, mqtt_post);
    snprintf(s_base, sizeof(s_base), "p4gw/%s", gw_state()->hostname);
    start_client();
    return xTaskCreate(publish_task, "mqtt_pub", 4096, NULL, 3, &s_publish_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
