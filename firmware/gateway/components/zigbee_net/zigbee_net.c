/*
 * Zigbee network map, devices and operations through Zigbee2MQTT (Zigbee › Network).
 *
 * The gateway does not talk to the Zigbee network itself: in the coordinator role Zigbee2MQTT owns
 * the coordinator, in other roles the network belongs to another adapter. Zigbee2MQTT publishes
 * everything on the MQTT broker the gateway already uses:
 *   +/bridge/info               instances (retained), the one whose serial port is this gateway is preferred
 *   <base>/bridge/devices       devices (retained)
 *   <base>/<device>             state: linkquality, last_seen, update
 *   <base>/<device>/availability
 *   <base>/bridge/request/...   operations, answered on <base>/bridge/response/... with our transaction
 */
#include "zigbee_net.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"

static const char *TAG = "zigbee_net";

#define KEY_BASE "z2m_base" /* str, Zigbee2MQTT base topic chosen in the UI */
#define REQUEST_WAIT pdMS_TO_TICKS(25000)
#define MAP_TIMEOUT_US (600 * 1000000LL) /* a big network takes Zigbee2MQTT several minutes */

static SemaphoreHandle_t s_lock;       /* all state below */
static SemaphoreHandle_t s_req_lock;   /* one operation at a time */
static SemaphoreHandle_t s_req_done;
static cJSON *s_instances;             /* [{base, version, ...}] */
static char s_base[64];                /* selected instance */
static bool s_base_subscribed;
static int64_t s_subscribed_us;
static cJSON *s_devices;               /* compact list */
static cJSON *s_states;                /* friendly name -> {lqi, last_seen, availability, update} */
static cJSON *s_map;                   /* {at, nodes, links} */
static int64_t s_map_started_us;       /* > 0 while a map request runs */
static char s_map_error[96];
static char s_req_transaction[24];
static char *s_req_response;
static uint32_t s_seq;

static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0;
}

static const char *str(cJSON *obj, const char *key)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

static void copy_str(cJSON *to, const char *key, cJSON *from, const char *from_key)
{
    const char *v = str(from, from_key);
    if (v && v[0]) {
        cJSON_AddStringToObject(to, key, v);
    }
}

static void copy_num(cJSON *to, const char *key, cJSON *from, const char *from_key)
{
    cJSON *v = cJSON_GetObjectItem(from, from_key);
    if (cJSON_IsNumber(v)) {
        cJSON_AddNumberToObject(to, key, v->valuedouble);
    }
}

/* ---------------------------------------------------------------- instances */

static bool uses_this_gateway(cJSON *info)
{
    cJSON *eth = gw_eth_status();
    const char *ip = str(eth, "ipv4");
    const char *port = str(cJSON_GetObjectItem(cJSON_GetObjectItem(info, "config"), "serial"), "port");
    bool yes = ip && ip[0] && port && strstr(port, ip);
    cJSON_Delete(eth);
    return yes;
}

static void select_base(void)
{
    if (s_base[0]) {
        return;
    }
    char saved[64] = "";
    gw_config_get_str(KEY_BASE, saved, sizeof(saved));
    cJSON *i, *pick = NULL;
    cJSON_ArrayForEach(i, s_instances)
    {
        const char *b = str(i, "base");
        if (saved[0] && b && strcmp(b, saved) == 0) {
            pick = i;
            break;
        }
        if (!pick || (cJSON_IsTrue(cJSON_GetObjectItem(i, "this_gateway")) &&
                      !cJSON_IsTrue(cJSON_GetObjectItem(pick, "this_gateway")))) {
            pick = i;
        }
    }
    if (pick) {
        strlcpy(s_base, str(pick, "base"), sizeof(s_base));
        s_base_subscribed = false;
    }
}

static void subscribe_base(void)
{
    if (s_base[0] && !s_base_subscribed) {
        char filter[80];
        snprintf(filter, sizeof(filter), "%s/#", s_base);
        s_base_subscribed = gw_mqtt_subscribe(filter) == ESP_OK;
        if (s_base_subscribed) {
            s_subscribed_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Zigbee2MQTT at %s", s_base);
        }
    }
}

static void handle_info(const char *base, const char *data)
{
    cJSON *info = cJSON_Parse(data);
    if (!info) {
        return;
    }
    cJSON *n = cJSON_CreateObject();
    cJSON_AddStringToObject(n, "base", base);
    copy_str(n, "version", info, "version");
    cJSON *coord = cJSON_GetObjectItem(info, "coordinator"), *meta = cJSON_GetObjectItem(coord, "meta");
    copy_str(n, "coordinator", coord, "type");
    copy_str(n, "coordinator_ieee", coord, "ieee_address");
    cJSON *rev = cJSON_GetObjectItem(meta, "revision");
    char fw[32] = "";
    if (cJSON_IsString(rev)) {
        strlcpy(fw, rev->valuestring, sizeof(fw));
    } else if (cJSON_IsNumber(rev)) {
        snprintf(fw, sizeof(fw), "%.0f", rev->valuedouble);
    }
    if (fw[0]) {
        cJSON_AddStringToObject(n, "coordinator_fw", fw);
    }
    cJSON *net = cJSON_GetObjectItem(info, "network");
    copy_num(n, "channel", net, "channel");
    copy_num(n, "pan_id", net, "pan_id");
    cJSON *epid = cJSON_GetObjectItem(net, "extended_pan_id");
    if (cJSON_IsString(epid)) {
        cJSON_AddStringToObject(n, "epid", epid->valuestring);
    } else if (cJSON_IsNumber(epid)) {
        cJSON_AddNumberToObject(n, "epid_num", epid->valuedouble);
    }
    cJSON_AddBoolToObject(n, "permit_join", cJSON_IsTrue(cJSON_GetObjectItem(info, "permit_join")));
    copy_num(n, "permit_join_end", info, "permit_join_end");         /* 2.x: unix ms */
    copy_num(n, "permit_join_timeout", info, "permit_join_timeout"); /* 1.x: seconds left */
    cJSON_AddBoolToObject(n, "this_gateway", uses_this_gateway(info));
    copy_str(n, "serial", cJSON_GetObjectItem(cJSON_GetObjectItem(info, "config"), "serial"), "port");
    cJSON_Delete(info);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = 0;
    cJSON *i;
    cJSON_ArrayForEach(i, s_instances)
    {
        if (strcmp(str(i, "base"), base) == 0) {
            break;
        }
        idx++;
    }
    if (idx < cJSON_GetArraySize(s_instances)) {
        cJSON_ReplaceItemInArray(s_instances, idx, n);
    } else {
        cJSON_AddItemToArray(s_instances, n);
    }
    select_base();
    subscribe_base();
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- devices, states, map */

static void handle_devices(const char *data, size_t len)
{
    cJSON *all = cJSON_Parse(data);
    if (!cJSON_IsArray(all)) {
        ESP_LOGW(TAG, "bridge/devices: %u B not a JSON array", (unsigned)len);
        cJSON_Delete(all);
        return;
    }
    ESP_LOGI(TAG, "%d devices (%u B)", cJSON_GetArraySize(all), (unsigned)len);
    cJSON *list = cJSON_CreateArray(), *d;
    cJSON_ArrayForEach(d, all)
    {
        cJSON *c = cJSON_CreateObject(), *def = cJSON_GetObjectItem(d, "definition");
        copy_str(c, "ieee", d, "ieee_address");
        copy_str(c, "name", d, "friendly_name");
        copy_str(c, "type", d, "type");
        copy_num(c, "nwk", d, "network_address");
        copy_str(c, "power", d, "power_source");
        copy_str(c, "manufacturer", d, "manufacturer");
        copy_str(c, "model_id", d, "model_id");
        copy_str(c, "sw", d, "software_build_id");
        copy_str(c, "date_code", d, "date_code");
        copy_str(c, "interview", d, "interview_state");
        if (!cJSON_GetObjectItem(c, "interview") && cJSON_IsBool(cJSON_GetObjectItem(d, "interview_completed"))) {
            cJSON_AddStringToObject(c, "interview",
                                    cJSON_IsTrue(cJSON_GetObjectItem(d, "interview_completed")) ? "SUCCESSFUL" : "PENDING");
        }
        cJSON_AddBoolToObject(c, "supported", !cJSON_IsFalse(cJSON_GetObjectItem(d, "supported")));
        cJSON_AddBoolToObject(c, "disabled", cJSON_IsTrue(cJSON_GetObjectItem(d, "disabled")));
        if (def) {
            copy_str(c, "vendor", def, "vendor");
            copy_str(c, "model", def, "model");
            copy_str(c, "description", def, "description");
            cJSON_AddBoolToObject(c, "ota", cJSON_IsTrue(cJSON_GetObjectItem(def, "supports_ota")));
            /* names of what Zigbee2MQTT exposes (identify, tx_power, ...), also inside composites */
            cJSON *names = cJSON_AddArrayToObject(c, "exposes"), *e, *f;
            cJSON_ArrayForEach(e, cJSON_GetObjectItem(def, "exposes"))
            {
                const char *name = str(e, "property") ? str(e, "property") : str(e, "name");
                if (name) {
                    cJSON_AddItemToArray(names, cJSON_CreateString(name));
                }
                cJSON_ArrayForEach(f, cJSON_GetObjectItem(e, "features"))
                {
                    const char *fn = str(f, "property");
                    if (fn) {
                        cJSON_AddItemToArray(names, cJSON_CreateString(fn));
                    }
                }
            }
        }
        cJSON_AddItemToArray(list, c);
    }
    cJSON_Delete(all);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_Delete(s_devices);
    s_devices = list;
    xSemaphoreGive(s_lock);
}

static cJSON *state_of(const char *name)
{
    cJSON *s = cJSON_GetObjectItem(s_states, name);
    if (!s) {
        s = cJSON_AddObjectToObject(s_states, name);
    }
    return s;
}

static void set_field(cJSON *obj, const char *key, cJSON *value)
{
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddItemToObject(obj, key, value);
}

static void handle_state(const char *name, const char *data)
{
    cJSON *j = cJSON_Parse(data);
    if (!cJSON_IsObject(j)) {
        cJSON_Delete(j);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *s = state_of(name), *v;
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "linkquality"))) {
        set_field(s, "lqi", cJSON_CreateNumber(v->valuedouble));
    }
    if ((v = cJSON_GetObjectItem(j, "last_seen")) && (cJSON_IsString(v) || cJSON_IsNumber(v))) {
        set_field(s, "last_seen", cJSON_Duplicate(v, false));
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "battery"))) {
        set_field(s, "battery", cJSON_CreateNumber(v->valuedouble));
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(j, "tx_power"))) {
        set_field(s, "tx_power", cJSON_CreateNumber(v->valuedouble));
    }
    const char *upd = str(cJSON_GetObjectItem(j, "update"), "state");
    if (upd) {
        set_field(s, "update", cJSON_CreateString(upd));
    }
    set_field(s, "seen_at", cJSON_CreateNumber((double)now_unix()));
    xSemaphoreGive(s_lock);
    cJSON_Delete(j);
}

static void handle_availability(const char *name, const char *data)
{
    cJSON *j = cJSON_Parse(data);
    const char *state = j ? str(j, "state") : NULL;
    bool online = state ? strcmp(state, "online") == 0 : strncmp(data, "online", 6) == 0;
    cJSON_Delete(j);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    set_field(state_of(name), "availability", cJSON_CreateString(online ? "online" : "offline"));
    xSemaphoreGive(s_lock);
}

static void handle_map(const char *data)
{
    cJSON *r = cJSON_Parse(data);
    cJSON *value = cJSON_GetObjectItem(cJSON_GetObjectItem(r, "data"), "value");
    const char *status = str(r, "status");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_map_started_us = 0;
    if (!status || strcmp(status, "ok") != 0 || !cJSON_IsObject(value)) {
        snprintf(s_map_error, sizeof(s_map_error), "%s", str(r, "error") ? str(r, "error") : "no map in the answer");
    } else {
        cJSON *map = cJSON_CreateObject(), *nodes = cJSON_AddArrayToObject(map, "nodes"),
              *links = cJSON_AddArrayToObject(map, "links"), *x;
        cJSON_AddNumberToObject(map, "at", (double)now_unix());
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(value, "nodes"))
        {
            cJSON *n = cJSON_CreateObject();
            copy_str(n, "ieee", x, "ieeeAddr");
            copy_str(n, "name", x, "friendlyName");
            copy_str(n, "type", x, "type");
            copy_num(n, "nwk", x, "networkAddress");
            cJSON *failed = cJSON_GetObjectItem(x, "failed");
            cJSON_AddBoolToObject(n, "failed", cJSON_IsArray(failed) && cJSON_GetArraySize(failed) > 0);
            cJSON_AddItemToArray(nodes, n);
        }
        cJSON_ArrayForEach(x, cJSON_GetObjectItem(value, "links"))
        {
            /* Zigbee2MQTT 2.x: sourceIeeeAddr/linkquality, 1.x: source.ieeeAddr/lqi */
            cJSON *l = cJSON_CreateObject();
            const char *src = str(x, "sourceIeeeAddr"), *dst = str(x, "targetIeeeAddr");
            src = src ? src : str(cJSON_GetObjectItem(x, "source"), "ieeeAddr");
            dst = dst ? dst : str(cJSON_GetObjectItem(x, "target"), "ieeeAddr");
            cJSON_AddStringToObject(l, "s", src ? src : "");
            cJSON_AddStringToObject(l, "t", dst ? dst : "");
            cJSON *lq = cJSON_GetObjectItem(x, "linkquality");
            lq = cJSON_IsNumber(lq) ? lq : cJSON_GetObjectItem(x, "lqi");
            cJSON_AddNumberToObject(l, "lqi", cJSON_IsNumber(lq) ? lq->valuedouble : 0);
            copy_num(l, "depth", x, "depth");
            copy_num(l, "rel", x, "relationship");
            cJSON_AddItemToArray(links, l);
        }
        cJSON_Delete(s_map);
        s_map = map;
        s_map_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(r);
}

/* ---------------------------------------------------------------- MQTT */

static void on_connect(void)
{
    gw_mqtt_subscribe("+/bridge/info");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_base_subscribed = false;
    subscribe_base();
    xSemaphoreGive(s_lock);
}

static void on_message(const char *topic, const char *data, size_t len)
{
    const char *slash = strchr(topic, '/');
    if (slash && strcmp(slash, "/bridge/info") == 0) {
        char base[64];
        snprintf(base, sizeof(base), "%.*s", (int)(slash - topic), topic);
        handle_info(base, data);
        return;
    }
    size_t blen = strlen(s_base);
    if (!blen || strncmp(topic, s_base, blen) != 0 || topic[blen] != '/') {
        return;
    }
    const char *rest = topic + blen + 1;
    if (strcmp(rest, "bridge/devices") == 0) {
        handle_devices(data, len);
    } else if (strcmp(rest, "bridge/response/networkmap") == 0) {
        handle_map(data);
    } else if (strncmp(rest, "bridge/response/", 16) == 0) {
        cJSON *r = cJSON_Parse(data);
        const char *tr = r ? str(r, "transaction") : NULL;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (tr && s_req_transaction[0] && strcmp(tr, s_req_transaction) == 0) {
            free(s_req_response);
            s_req_response = strdup(data);
            s_req_transaction[0] = '\0';
            xSemaphoreGive(s_req_done);
        }
        xSemaphoreGive(s_lock);
        cJSON_Delete(r);
    } else if (strncmp(rest, "bridge/", 7) == 0) {
        return;
    } else {
        size_t rlen = strlen(rest);
        if (rlen > 13 && strcmp(rest + rlen - 13, "/availability") == 0) {
            char name[96];
            snprintf(name, sizeof(name), "%.*s", (int)(rlen - 13), rest);
            handle_availability(name, data);
        } else if (!(rlen > 4 && (strcmp(rest + rlen - 4, "/set") == 0 || strcmp(rest + rlen - 4, "/get") == 0)) &&
                   len && data[0] == '{') {
            handle_state(rest, data);
        }
    }
}

/* ---------------------------------------------------------------- API */

static esp_err_t net_get(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "mqtt", gw_mqtt_connected());
    /* the retained device list sometimes does not arrive after a (re)connect: subscribing again resends it;
     * outside s_lock, the MQTT task may wait for it while holding the client */
    char filter[80] = "";
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_base_subscribed && !s_devices && gw_mqtt_connected() && esp_timer_get_time() - s_subscribed_us > 30000000) {
        snprintf(filter, sizeof(filter), "%s/#", s_base);
        s_subscribed_us = esp_timer_get_time();
    }
    xSemaphoreGive(s_lock);
    if (filter[0]) {
        ESP_LOGW(TAG, "no device list, subscribing to %s again", filter);
        gw_mqtt_unsubscribe(filter);
        gw_mqtt_subscribe(filter);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddItemToObject(json, "instances", cJSON_Duplicate(s_instances, true));
    cJSON_AddStringToObject(json, "base", s_base);
    cJSON_AddItemToObject(json, "devices", s_devices ? cJSON_Duplicate(s_devices, true) : cJSON_CreateArray());
    cJSON_AddItemToObject(json, "states", cJSON_Duplicate(s_states, true));
    cJSON_AddItemToObject(json, "map", s_map ? cJSON_Duplicate(s_map, true) : cJSON_CreateNull());
    if (s_map_started_us && esp_timer_get_time() - s_map_started_us > MAP_TIMEOUT_US) {
        s_map_started_us = 0;
        strlcpy(s_map_error, "Zigbee2MQTT did not answer in 10 minutes", sizeof(s_map_error));
    }
    cJSON_AddBoolToObject(json, "map_running", s_map_started_us != 0);
    cJSON_AddStringToObject(json, "map_error", s_map_error);
    xSemaphoreGive(s_lock);
    cJSON_AddNumberToObject(json, "now", (double)now_unix());
    return gw_http_send_json(req, json);
}

static esp_err_t publish(const char *path, cJSON *payload)
{
    char topic[128];
    snprintf(topic, sizeof(topic), "%s/bridge/request/%s", s_base, path);
    char *text = cJSON_PrintUnformatted(payload);
    esp_err_t err = text ? gw_mqtt_publish_to(topic, text) : ESP_ERR_NO_MEM;
    cJSON_free(text);
    return err;
}

static esp_err_t map_post(httpd_req_t *req)
{
    if (!s_base[0]) {
        return gw_http_send_error(req, 409, "no Zigbee2MQTT found on the MQTT broker");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool running = s_map_started_us != 0;
    if (!running) {
        s_map_started_us = esp_timer_get_time();
        s_map_error[0] = '\0';
    }
    xSemaphoreGive(s_lock);
    if (running) {
        return gw_http_send_error(req, 409, "a map request is running");
    }
    cJSON *p = cJSON_CreateObject();
    cJSON_AddStringToObject(p, "type", "raw");
    cJSON_AddBoolToObject(p, "routes", false);
    esp_err_t err = publish("networkmap", p);
    cJSON_Delete(p);
    if (err != ESP_OK) {
        s_map_started_us = 0;
        return gw_http_send_error(req, 503, "MQTT not connected");
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "started", true);
    return gw_http_send_json(req, json);
}

/* Operations: action -> Zigbee2MQTT request, answered with the response of Zigbee2MQTT. */
static esp_err_t request_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *action = str(body, "action"), *device = str(body, "device"), *to = str(body, "to");
    cJSON *p = cJSON_CreateObject();
    const char *path = NULL;
    bool wait = true;
    if (action && strcmp(action, "permit_join") == 0) {
        cJSON *t = cJSON_GetObjectItem(body, "time");
        int time_s = cJSON_IsNumber(t) ? t->valueint : 254;
        path = "permit_join";
        cJSON_AddNumberToObject(p, "time", time_s < 0 ? 0 : time_s > 254 ? 254 : time_s);
        cJSON_AddBoolToObject(p, "value", time_s > 0); /* Zigbee2MQTT 1.x */
        if (device) {
            cJSON_AddStringToObject(p, "device", device);
        }
    } else if (action && device && strcmp(action, "rename") == 0 && to && to[0]) {
        path = "device/rename";
        cJSON_AddStringToObject(p, "from", device);
        cJSON_AddStringToObject(p, "to", to);
        cJSON_AddBoolToObject(p, "homeassistant_rename", true);
    } else if (action && device && strcmp(action, "remove") == 0) {
        path = "device/remove";
        cJSON_AddStringToObject(p, "id", device);
        cJSON_AddBoolToObject(p, "force", cJSON_IsTrue(cJSON_GetObjectItem(body, "force")));
    } else if (action && device && strcmp(action, "interview") == 0) {
        path = "device/interview";
        cJSON_AddStringToObject(p, "id", device);
    } else if (action && device && strcmp(action, "ota_check") == 0) {
        path = "device/ota_update/check";
        cJSON_AddStringToObject(p, "id", device);
    } else if (action && device && strcmp(action, "identify") == 0) {
        /* a device command, not a bridge request: <base>/<device>/set, no answer */
        char topic[160];
        snprintf(topic, sizeof(topic), "%s/%s/set", s_base, device);
        cJSON_Delete(body); /* device points into it */
        cJSON_AddStringToObject(p, "identify", "identify");
        char *text = cJSON_PrintUnformatted(p);
        esp_err_t err = s_base[0] && text ? gw_mqtt_publish_to(topic, text) : ESP_ERR_INVALID_STATE;
        cJSON_free(text);
        cJSON_Delete(p);
        if (err != ESP_OK) {
            return gw_http_send_error(req, 503, "MQTT not connected or no Zigbee2MQTT");
        }
        cJSON *json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "status", "sent");
        return gw_http_send_json(req, json);
    } else if (action && device && strcmp(action, "ota_update") == 0) {
        path = "device/ota_update/update";
        cJSON_AddStringToObject(p, "id", device);
        wait = false; /* takes minutes, the device state shows the progress */
    }
    cJSON_Delete(body);
    if (!path) {
        cJSON_Delete(p);
        return gw_http_send_error(req, 400, "unknown action or missing device");
    }
    if (!s_base[0]) {
        cJSON_Delete(p);
        return gw_http_send_error(req, 409, "no Zigbee2MQTT found on the MQTT broker");
    }
    if (xSemaphoreTake(s_req_lock, 0) != pdTRUE) {
        cJSON_Delete(p);
        return gw_http_send_error(req, 409, "another operation is running");
    }
    char tr[24];
    snprintf(tr, sizeof(tr), "p4gw-%lu", (unsigned long)++s_seq);
    cJSON_AddStringToObject(p, "transaction", tr);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_req_transaction, wait ? tr : "", sizeof(s_req_transaction));
    xSemaphoreGive(s_lock);
    xSemaphoreTake(s_req_done, 0);
    esp_err_t err = publish(path, p);
    cJSON_Delete(p);
    cJSON *json = NULL;
    if (err != ESP_OK) {
        xSemaphoreGive(s_req_lock);
        return gw_http_send_error(req, 503, "MQTT not connected");
    }
    if (!wait) {
        json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "status", "started");
    } else if (xSemaphoreTake(s_req_done, REQUEST_WAIT) == pdTRUE) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        json = cJSON_Parse(s_req_response);
        free(s_req_response);
        s_req_response = NULL;
        xSemaphoreGive(s_lock);
    }
    if (!json) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_req_transaction[0] = '\0';
        xSemaphoreGive(s_lock);
        json = cJSON_CreateObject();
        cJSON_AddStringToObject(json, "status", "sent");
        cJSON_AddStringToObject(json, "error", "no answer from Zigbee2MQTT yet");
    }
    xSemaphoreGive(s_req_lock);
    return gw_http_send_json(req, json);
}

static esp_err_t base_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *base = str(body, "base");
    if (!base || !base[0] || strlen(base) >= sizeof(s_base) || strchr(base, '+') || strchr(base, '#')) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "base: a Zigbee2MQTT base topic");
    }
    gw_config_set_str(KEY_BASE, base);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(base, s_base) != 0) {
        if (s_base[0]) {
            char filter[80];
            snprintf(filter, sizeof(filter), "%s/#", s_base);
            gw_mqtt_unsubscribe(filter);
        }
        strlcpy(s_base, base, sizeof(s_base));
        s_base_subscribed = false;
        cJSON_Delete(s_devices);
        s_devices = NULL;
        cJSON_Delete(s_map);
        s_map = NULL;
        cJSON_Delete(s_states);
        s_states = cJSON_CreateObject();
        subscribe_base();
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(body);
    return net_get(req);
}

void zigbee_net_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_req_lock = xSemaphoreCreateMutex();
    s_req_done = xSemaphoreCreateBinary();
    s_instances = cJSON_CreateArray();
    s_states = cJSON_CreateObject();
    const gw_mqtt_listener_t listener = {.on_connect = on_connect, .on_message = on_message};
    gw_mqtt_set_listener(&listener);
    gw_http_register("/api/zigbee/net", HTTP_GET, net_get);
    gw_http_register("/api/zigbee/net/map", HTTP_POST, map_post);
    gw_http_register("/api/zigbee/net/request", HTTP_POST, request_post);
    gw_http_register("/api/zigbee/net/base", HTTP_POST, base_post);
}
