#include "sound_events.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"
#include "sound_model.h"

static const char *TAG = "sound_events";

#define KEY_CONFIG "snd_events"
#define MAX_EVENTS 32
#define WORDS ((SOUND_CLASSES + 31) / 32)
#define SUPPRESS_SCORE 0.5f
#define HYSTERESIS 0.1f
#define LOG_SIZE 64
#define UNASSIGNED_SCORE 0.6f
#define UNASSIGNED_REPEAT_US (60 * 1000000LL)
#define MAX_BODY (32 * 1024)

extern const char presets_start[] asm("_binary_sound_presets_json_start");
extern const char presets_end[] asm("_binary_sound_presets_json_end");
extern const char classes_start[] asm("_binary_sound_classes_json_start");
extern const char classes_end[] asm("_binary_sound_classes_json_end");

typedef struct {
    char id[24];
    char name[48];
    char name_cs[96]; /* optional Czech name: web UI and an HA attribute only */
    char device_class[16];
    bool enabled;
    bool continuous;
    float threshold;
    uint8_t windows;
    uint16_t hold_s;
    uint16_t cooldown_s;
    uint32_t classes[WORDS];
    uint32_t suppress[WORDS];
    /* runtime */
    uint8_t hits;
    bool active;
    int64_t start_us, last_hit_us, end_us, last_event_us;
    float peak;
    int peak_class;
    uint32_t count;
    int log_slot;
} sevent_t;

typedef struct {
    int64_t at_us;
    char id[24];
    int cls;
    float score;
    float level_db;
    float duration_s; /* <0 while a continuous event is still on */
} log_entry_t;

static SemaphoreHandle_t s_lock;
static const char *(*s_class_name)(int);
static char s_preset[24];
static uint32_t s_exclude[WORDS];
static sevent_t *s_ev;
static int s_count;
static log_entry_t *s_log;
static int s_log_next;
static float *s_scores;
static int64_t s_scores_us;
static int64_t *s_unassigned_us;

static bool bit(const uint32_t *set, int i)
{
    return set[i / 32] & (1u << (i % 32));
}

static void set_bit(uint32_t *set, int i)
{
    set[i / 32] |= 1u << (i % 32);
}

/* ---------------------------------------------------------------- config <-> JSON */

static bool parse_classes(cJSON *arr, uint32_t *set)
{
    memset(set, 0, WORDS * sizeof(uint32_t));
    if (!arr) {
        return true;
    }
    if (!cJSON_IsArray(arr)) {
        return false;
    }
    cJSON *v;
    cJSON_ArrayForEach(v, arr)
    {
        if (!cJSON_IsNumber(v) || v->valueint < 0 || v->valueint >= SOUND_CLASSES) {
            return false;
        }
        set_bit(set, v->valueint);
    }
    return true;
}

static cJSON *classes_json(const uint32_t *set)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < SOUND_CLASSES; i++) {
        if (bit(set, i)) {
            cJSON_AddItemToArray(arr, cJSON_CreateNumber(i));
        }
    }
    return arr;
}

static bool valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n >= sizeof(((sevent_t *)0)->id)) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = id[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
    }
    return true;
}

static double num(cJSON *obj, const char *key, double def, double lo, double hi)
{
    cJSON *v = cJSON_GetObjectItem(obj, key);
    double d = cJSON_IsNumber(v) ? v->valuedouble : def;
    return d < lo ? lo : d > hi ? hi : d;
}

/* Czech name of the preset event with the same id and English name (configs saved before name_cs existed). */
static void preset_name_cs(cJSON **presets, const sevent_t *x, char *out, size_t len)
{
    if (!*presets) {
        *presets = cJSON_Parse(presets_start);
    }
    cJSON *p, *e;
    cJSON_ArrayForEach(p, *presets)
    {
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(p, "events"))
        {
            const char *id = GW_JSON_STR(e, "id"), *name = GW_JSON_STR(e, "name"), *cs = GW_JSON_STR(e, "name_cs");
            if (id && name && cs && strcmp(id, x->id) == 0 && strcmp(name, x->name) == 0) {
                strlcpy(out, cs, len);
                return;
            }
        }
    }
}

/* Parses into fresh buffers; nothing changes on error. */
static esp_err_t parse_config(cJSON *root, sevent_t *ev, int *count, uint32_t *exclude, char *preset, char *err,
                              size_t err_len)
{
    cJSON *events = cJSON_GetObjectItem(root, "events");
    if (!cJSON_IsArray(events) || cJSON_GetArraySize(events) > MAX_EVENTS) {
        snprintf(err, err_len, "events: array of at most %d", MAX_EVENTS);
        return ESP_ERR_INVALID_ARG;
    }
    if (!parse_classes(cJSON_GetObjectItem(root, "exclude"), exclude)) {
        snprintf(err, err_len, "exclude: class indexes 0-%d", SOUND_CLASSES - 1);
        return ESP_ERR_INVALID_ARG;
    }
    const char *p = GW_JSON_STR(root, "preset");
    strlcpy(preset, p ? p : "custom", 24);
    int n = 0;
    cJSON *e, *presets = NULL;
    esp_err_t ret = ESP_OK;
    cJSON_ArrayForEach(e, events)
    {
        sevent_t *x = &ev[n];
        memset(x, 0, sizeof(*x));
        const char *id = GW_JSON_STR(e, "id"), *name = GW_JSON_STR(e, "name"), *kind = GW_JSON_STR(e, "kind");
        if (!valid_id(id)) {
            snprintf(err, err_len, "event %d: id must be a-z, 0-9, _ (max 23)", n);
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        for (int j = 0; j < n && ret == ESP_OK; j++) {
            if (strcmp(ev[j].id, id) == 0) {
                snprintf(err, err_len, "duplicate event id %s", id);
                ret = ESP_ERR_INVALID_ARG;
            }
        }
        if (ret != ESP_OK) {
            break;
        }
        strlcpy(x->id, id, sizeof(x->id));
        strlcpy(x->name, name && name[0] ? name : id, sizeof(x->name));
        cJSON *cs = cJSON_GetObjectItem(e, "name_cs");
        if (cJSON_IsString(cs)) {
            strlcpy(x->name_cs, cs->valuestring, sizeof(x->name_cs));
        } else {
            preset_name_cs(&presets, x, x->name_cs, sizeof(x->name_cs));
        }
        const char *dc = GW_JSON_STR(e, "device_class");
        strlcpy(x->device_class, dc ? dc : "", sizeof(x->device_class));
        cJSON *en = cJSON_GetObjectItem(e, "enabled");
        x->enabled = !en || cJSON_IsTrue(en);
        x->continuous = kind && strcmp(kind, "continuous") == 0;
        x->threshold = num(e, "threshold", 0.5, 0.05, 1.0);
        x->windows = num(e, "windows", 1, 1, 20);
        x->hold_s = num(e, "hold_s", 3, 1, 600);
        x->cooldown_s = num(e, "cooldown_s", 0, 0, 3600);
        if (!parse_classes(cJSON_GetObjectItem(e, "classes"), x->classes) ||
            !parse_classes(cJSON_GetObjectItem(e, "suppress"), x->suppress)) {
            snprintf(err, err_len, "event %s: class indexes 0-%d", id, SOUND_CLASSES - 1);
            ret = ESP_ERR_INVALID_ARG;
            break;
        }
        x->log_slot = -1;
        n++;
    }
    cJSON_Delete(presets);
    *count = n;
    return ret;
}

static cJSON *config_json(bool runtime)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "preset", s_preset);
    cJSON_AddItemToObject(root, "exclude", classes_json(s_exclude));
    cJSON *events = cJSON_AddArrayToObject(root, "events");
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < s_count; i++) {
        sevent_t *x = &s_ev[i];
        cJSON *e = cJSON_CreateObject();
        cJSON_AddStringToObject(e, "id", x->id);
        cJSON_AddStringToObject(e, "name", x->name);
        if (x->name_cs[0]) {
            cJSON_AddStringToObject(e, "name_cs", x->name_cs);
        }
        cJSON_AddStringToObject(e, "kind", x->continuous ? "continuous" : "impulse");
        if (x->device_class[0]) {
            cJSON_AddStringToObject(e, "device_class", x->device_class);
        }
        cJSON_AddBoolToObject(e, "enabled", x->enabled);
        cJSON_AddNumberToObject(e, "threshold", round((double)(x->threshold) * 100) / 100.0);
        cJSON_AddNumberToObject(e, "windows", x->windows);
        cJSON_AddNumberToObject(e, "hold_s", x->hold_s);
        cJSON_AddNumberToObject(e, "cooldown_s", x->cooldown_s);
        cJSON_AddItemToObject(e, "classes", classes_json(x->classes));
        cJSON_AddItemToObject(e, "suppress", classes_json(x->suppress));
        if (runtime) {
            cJSON_AddBoolToObject(e, "active", x->active);
            cJSON_AddNumberToObject(e, "count", x->count);
            if (x->last_event_us) {
                cJSON_AddNumberToObject(e, "last_ago_s", (now - x->last_event_us) / 1000000);
            }
        }
        cJSON_AddItemToArray(events, e);
    }
    return root;
}

static esp_err_t save_config(void)
{
    cJSON *json = config_json(false);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = gw_config_set_blob(KEY_CONFIG, text, strlen(text) + 1);
    cJSON_free(text);
    return err;
}

/* ---------------------------------------------------------------- MQTT */

static void object_id(char *out, size_t len, const char *id, const char *suffix)
{
    snprintf(out, len, "snd_%s%s", id, suffix);
}

static void publish_state(const sevent_t *x)
{
    char t[64];
    snprintf(t, sizeof(t), "sound/%s/state", x->id);
    gw_mqtt_publish(t, x->active ? "ON" : "OFF", true);
}

static void publish_discovery(const sevent_t *x, bool remove)
{
    char oid[48], buf[128];
    const char *base = gw_mqtt_base_topic();

    object_id(oid, sizeof(oid), x->id, "");
    cJSON *ev = NULL, *bin = NULL, *trig = NULL;
    if (!remove) {
        ev = cJSON_CreateObject();
        cJSON_AddStringToObject(ev, "name", x->name);
        snprintf(buf, sizeof(buf), "%s/sound/%s", base, x->id);
        cJSON_AddStringToObject(ev, "state_topic", buf);
        cJSON *types = cJSON_AddArrayToObject(ev, "event_types");
        cJSON_AddItemToArray(types, cJSON_CreateString("detected"));
        if (x->device_class[0]) {
            cJSON_AddStringToObject(ev, "device_class", x->device_class);
        }
        cJSON_AddStringToObject(ev, "icon", "mdi:waveform");

        bin = cJSON_CreateObject();
        snprintf(buf, sizeof(buf), "%s active", x->name);
        cJSON_AddStringToObject(bin, "name", buf);
        snprintf(buf, sizeof(buf), "%s/sound/%s/state", base, x->id);
        cJSON_AddStringToObject(bin, "state_topic", buf);
        cJSON_AddStringToObject(bin, "device_class", "sound");

        trig = cJSON_CreateObject();
        cJSON_AddStringToObject(trig, "automation_type", "trigger");
        snprintf(buf, sizeof(buf), "%s/sound/%s", base, x->id);
        cJSON_AddStringToObject(trig, "topic", buf);
        cJSON_AddStringToObject(trig, "type", "sound");
        cJSON_AddStringToObject(trig, "subtype", x->name);
    }
    gw_mqtt_discovery("event", oid, ev);
    object_id(oid, sizeof(oid), x->id, "_active");
    gw_mqtt_discovery("binary_sensor", oid, bin);
    object_id(oid, sizeof(oid), x->id, "");
    gw_mqtt_discovery("device_automation", oid, trig);
    if (!remove) {
        publish_state(x);
    }
}

void sound_events_mqtt_discovery(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        publish_discovery(&s_ev[i], !s_ev[i].enabled);
    }
    xSemaphoreGive(s_lock);
}

static void publish_event(const sevent_t *x, float level_db)
{
    char t[64];
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "event_type", "detected");
    cJSON_AddStringToObject(json, "class", s_class_name(x->peak_class));
    cJSON_AddNumberToObject(json, "score", round((double)(x->peak) * 100) / 100.0);
    cJSON_AddNumberToObject(json, "level_db", round((double)(level_db) * 10) / 10.0);
    if (x->name_cs[0]) {
        cJSON_AddStringToObject(json, "name_cs", x->name_cs);
    }
    char *payload = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    snprintf(t, sizeof(t), "sound/%s", x->id);
    gw_mqtt_publish(t, payload, false);
    cJSON_free(payload);
}

/* ---------------------------------------------------------------- detection */

static int log_add(const char *id, int cls, float score, float level_db, float duration_s)
{
    int slot = s_log_next;
    s_log[slot] = (log_entry_t){.at_us = esp_timer_get_time(), .cls = cls, .score = score, .level_db = level_db,
                                .duration_s = duration_s};
    strlcpy(s_log[slot].id, id, sizeof(s_log[slot].id));
    s_log_next = (s_log_next + 1) % LOG_SIZE;
    return slot;
}

void sound_events_update(const float *scores, float level_db, int64_t now)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (scores) {
        memcpy(s_scores, scores, SOUND_CLASSES * sizeof(float));
        s_scores_us = now;
    }
    for (int i = 0; i < s_count; i++) {
        sevent_t *x = &s_ev[i];
        if (!x->enabled) {
            continue;
        }
        float best = 0;
        int best_class = -1;
        bool suppressed = false;
        for (int c = 0; scores && c < SOUND_CLASSES; c++) {
            if (bit(x->suppress, c) && scores[c] >= SUPPRESS_SCORE) {
                suppressed = true;
            }
            if (bit(x->classes, c) && !bit(s_exclude, c) && scores[c] > best) {
                best = scores[c];
                best_class = c;
            }
        }
        float threshold = x->active ? x->threshold - HYSTERESIS : x->threshold;
        bool hit = !suppressed && best_class >= 0 && best >= threshold;
        if (hit) {
            x->hits = x->hits < 255 ? x->hits + 1 : 255;
            x->last_hit_us = now;
            if (!x->active || best > x->peak) {
                x->peak = best;
                x->peak_class = best_class;
            }
            if (!x->active && x->hits >= x->windows) {
                x->active = true;
                x->start_us = now;
                publish_state(x);
                bool cooled = !x->last_event_us || now - x->end_us >= (int64_t)x->cooldown_s * 1000000;
                if (cooled) {
                    x->last_event_us = now;
                    x->count++;
                    publish_event(x, level_db);
                    x->log_slot = log_add(x->id, x->peak_class, x->peak, level_db, x->continuous ? -1 : 0);
                    ESP_LOGI(TAG, "%s (%s %.2f)", x->id, s_class_name(x->peak_class), x->peak);
                } else {
                    x->log_slot = -1;
                }
            }
        } else {
            x->hits = 0;
            if (x->active && now - x->last_hit_us >= (int64_t)x->hold_s * 1000000) {
                x->active = false;
                x->end_us = now;
                publish_state(x);
                if (x->log_slot >= 0 && strcmp(s_log[x->log_slot].id, x->id) == 0) {
                    s_log[x->log_slot].duration_s = (x->last_hit_us - x->start_us) / 1e6f + 0.96f;
                    s_log[x->log_slot].score = x->peak;
                }
                x->peak = 0;
            }
        }
    }
    /* Strong classes that no event covers: shown in the UI log to help personalising. */
    for (int c = 0; scores && c < SOUND_CLASSES; c++) {
        if (scores[c] < UNASSIGNED_SCORE || bit(s_exclude, c) || now - s_unassigned_us[c] < UNASSIGNED_REPEAT_US) {
            continue;
        }
        bool covered = false;
        for (int i = 0; i < s_count && !covered; i++) {
            covered = s_ev[i].enabled && bit(s_ev[i].classes, c);
        }
        if (!covered) {
            s_unassigned_us[c] = now;
            log_add("", c, scores[c], level_db, 0);
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- API */

static esp_err_t events_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *json = config_json(true);
    xSemaphoreGive(s_lock);
    cJSON *presets = cJSON_Parse(presets_start);
    cJSON *list = cJSON_AddArrayToObject(json, "presets");
    cJSON *p;
    cJSON_ArrayForEach(p, presets)
    {
        cJSON *item = cJSON_CreateObject();
        cJSON_AddStringToObject(item, "key", p->string);
        cJSON_AddStringToObject(item, "name", GW_JSON_STR(p, "name") ?: p->string);
        cJSON_AddItemToArray(list, item);
    }
    cJSON_Delete(presets);
    return gw_http_send_json(req, json);
}

/* Replaces the configuration; events that disappeared or got disabled are removed from HA. */
static esp_err_t apply_config(cJSON *root, char *err, size_t err_len)
{
    sevent_t *ev = heap_caps_calloc(MAX_EVENTS, sizeof(sevent_t), MALLOC_CAP_SPIRAM);
    uint32_t exclude[WORDS];
    char preset[24];
    int count = 0;
    if (!ev) {
        snprintf(err, err_len, "out of memory");
        return ESP_ERR_NO_MEM;
    }
    esp_err_t e = parse_config(root, ev, &count, exclude, preset, err, err_len);
    if (e != ESP_OK) {
        free(ev);
        return e;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        bool kept = false;
        for (int j = 0; j < count; j++) {
            if (strcmp(s_ev[i].id, ev[j].id) == 0) {
                kept = ev[j].enabled;
                /* Keep counters and state of events that stay. */
                ev[j].active = s_ev[i].active && ev[j].enabled;
                ev[j].count = s_ev[i].count;
                ev[j].last_event_us = s_ev[i].last_event_us;
                ev[j].end_us = s_ev[i].end_us;
            }
        }
        if (!kept) {
            publish_discovery(&s_ev[i], true);
        }
    }
    memcpy(s_ev, ev, MAX_EVENTS * sizeof(sevent_t));
    s_count = count;
    memcpy(s_exclude, exclude, sizeof(exclude));
    strlcpy(s_preset, preset, sizeof(s_preset));
    for (int i = 0; i < s_count; i++) {
        publish_discovery(&s_ev[i], !s_ev[i].enabled);
    }
    e = save_config();
    xSemaphoreGive(s_lock);
    free(ev);
    ESP_LOGI(TAG, "%d events (%s)", count, preset);
    return e;
}

typedef struct {
    char *buf;
    size_t len;
} body_t;

static esp_err_t body_sink(void *arg, uint32_t offset, const void *data, size_t len)
{
    body_t *b = arg;
    memcpy(b->buf + offset, data, len);
    b->len = offset + len;
    return ESP_OK;
}

static esp_err_t events_put(httpd_req_t *req)
{
    if (req->content_len == 0 || req->content_len > MAX_BODY) {
        return gw_http_send_error(req, 413, "configuration JSON up to 32 kB expected");
    }
    body_t body = {.buf = heap_caps_malloc(req->content_len + 1, MALLOC_CAP_SPIRAM)};
    if (!body.buf || gw_http_receive(req, body_sink, &body) != ESP_OK) {
        free(body.buf);
        return gw_http_send_error(req, 400, "body read failed");
    }
    body.buf[body.len] = '\0';
    cJSON *root = cJSON_Parse(body.buf);
    free(body.buf);
    if (!root) {
        return gw_http_send_error(req, 400, "invalid JSON");
    }
    char err[96] = "";
    esp_err_t e = apply_config(root, err, sizeof(err));
    cJSON_Delete(root);
    if (e != ESP_OK) {
        return gw_http_send_error(req, e == ESP_ERR_INVALID_ARG ? 400 : 500, err[0] ? err : esp_err_to_name(e));
    }
    return events_get(req);
}

static esp_err_t apply_preset(const char *key, char *err, size_t err_len)
{
    cJSON *presets = cJSON_Parse(presets_start);
    cJSON *p = cJSON_GetObjectItem(presets, key ? key : "");
    esp_err_t e = ESP_ERR_NOT_FOUND;
    if (p) {
        cJSON_AddStringToObject(p, "preset", key);
        e = apply_config(p, err, err_len);
    } else {
        snprintf(err, err_len, "unknown preset");
    }
    cJSON_Delete(presets);
    return e;
}

static esp_err_t preset_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char err[96] = "";
    esp_err_t e = apply_preset(GW_JSON_STR(body, "preset"), err, sizeof(err));
    cJSON_Delete(body);
    if (e != ESP_OK) {
        return gw_http_send_error(req, 400, err);
    }
    return events_get(req);
}

static esp_err_t classes_get(httpd_req_t *req)
{
    /* [[index, name, category, group], ...] from AudioSet (CC BY-SA 4.0). */
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, classes_start, classes_end - classes_start - 1);
}

static esp_err_t scores_get(httpd_req_t *req)
{
    char *buf = heap_caps_malloc(SOUND_CLASSES * 4 + 64, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return gw_http_send_error(req, 500, "out of memory");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = snprintf(buf, 64, "{\"age_s\":%.1f,\"scores\":[",
                     s_scores_us ? (esp_timer_get_time() - s_scores_us) / 1e6 : -1.0);
    for (int i = 0; i < SOUND_CLASSES; i++) {
        n += sprintf(buf + n, i ? ",%d" : "%d", (int)lroundf(s_scores[i] * 100));
    }
    xSemaphoreGive(s_lock);
    n += sprintf(buf + n, "]}");
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t log_get(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int k = 1; k <= LOG_SIZE; k++) {
        const log_entry_t *l = &s_log[(s_log_next - k + LOG_SIZE) % LOG_SIZE];
        if (!l->at_us) {
            break;
        }
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "ago_s", (now - l->at_us) / 1000000);
        cJSON_AddStringToObject(e, "event", l->id);
        cJSON_AddNumberToObject(e, "class", l->cls);
        cJSON_AddStringToObject(e, "class_name", s_class_name(l->cls));
        cJSON_AddNumberToObject(e, "score", round((double)(l->score) * 100) / 100.0);
        cJSON_AddNumberToObject(e, "level_db", round((double)(l->level_db) * 10) / 10.0);
        if (l->duration_s >= 0) {
            cJSON_AddNumberToObject(e, "duration_s", round((double)(l->duration_s) * 10) / 10.0);
        } else {
            cJSON_AddBoolToObject(e, "ongoing", true);
        }
        cJSON_AddItemToArray(arr, e);
    }
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, arr);
}

esp_err_t sound_events_init(const char *(*class_name)(int index))
{
    s_class_name = class_name;
    s_lock = xSemaphoreCreateMutex();
    s_ev = heap_caps_calloc(MAX_EVENTS, sizeof(sevent_t), MALLOC_CAP_SPIRAM);
    s_log = heap_caps_calloc(LOG_SIZE, sizeof(log_entry_t), MALLOC_CAP_SPIRAM);
    s_scores = heap_caps_calloc(SOUND_CLASSES, sizeof(float), MALLOC_CAP_SPIRAM);
    s_unassigned_us = heap_caps_calloc(SOUND_CLASSES, sizeof(int64_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_lock && s_ev && s_log && s_scores && s_unassigned_us, ESP_ERR_NO_MEM, TAG, "no mem");
    for (int c = 0; c < SOUND_CLASSES; c++) {
        s_unassigned_us[c] = -UNASSIGNED_REPEAT_US;
    }

    char err[96] = "";
    size_t len = 0;
    esp_err_t e = ESP_ERR_NOT_FOUND;
    char *text = NULL;
    if (gw_config_blob_size(KEY_CONFIG, &len) == ESP_OK && len > 1 && (text = heap_caps_malloc(len, MALLOC_CAP_SPIRAM)) &&
        gw_config_get_blob(KEY_CONFIG, text, len)) {
        cJSON *root = cJSON_Parse(text);
        e = root ? apply_config(root, err, sizeof(err)) : ESP_ERR_INVALID_ARG;
        cJSON_Delete(root);
    }
    free(text);
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "no valid saved events (%s), using the household preset", err[0] ? err : esp_err_to_name(e));
        apply_preset("household", err, sizeof(err));
    }

    gw_http_register("/api/sound/events", HTTP_GET, events_get);
    gw_http_register("/api/sound/events", HTTP_PUT, events_put);
    gw_http_register("/api/sound/events/preset", HTTP_POST, preset_post);
    gw_http_register("/api/sound/classes", HTTP_GET, classes_get);
    gw_http_register("/api/sound/scores", HTTP_GET, scores_get);
    gw_http_register("/api/sound/log", HTTP_GET, log_get);
    return ESP_OK;
}
