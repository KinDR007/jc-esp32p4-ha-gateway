#include "modbus.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "mb_bus.h"
#include "mb_template.h"
#include "sdkconfig.h"

/*
 * Modbus devices on RS485 (connector J4) and Modbus TCP (docs/modbus.md). One task owns both transports:
 * it applies configuration changes, runs writes and manual requests, a bus search, and polls the devices.
 * Each device follows a template (mb_template.c); values go to /api/modbus, to the push WebSocket and
 * MQTT (topic modbus/<id>), and Home Assistant shows every device as its own device.
 */

static const char *TAG = "modbus";

#define KEY_CFG "mb_cfg"          /* str JSON {"bus": {...}, "devices": [...]} */
#define KEY_TPL "mbt%d"           /* blob, compact JSON of user template n */
#define MAX_DEVICES 16
#define MAX_USER_TPL 16
#define MAX_TPL 48
#define USER_TPL_MAX_BYTES 16384
#define NAME_CHARS 32
#define OFFLINE_AFTER 3           /* polls without any answer */
#define OFFLINE_RETRY_S 30
#define EXPOSE_INTEGRATION 1
#define EXPOSE_MQTT 2

extern const char templates_json_start[] asm("_binary_templates_json_start");

typedef struct {
    mb_template_t t;
    uint32_t rev; /* CRC of its JSON: the integration refetches a changed template */
    int slot;     /* NVS slot of a user template, -1 built in */
} tpl_t;

typedef struct {
    /* configuration */
    char id[8];
    char name[NAME_CHARS * 3 + 1];
    char tpl[32];
    bool tcp;
    char host[64];
    uint16_t port;
    uint8_t slave;
    uint16_t poll_s;  /* 0 = the template's */
    uint8_t expose;   /* EXPOSE_* bits */
    bool enabled;
    /* runtime, owned by the task; values copied under s_lock */
    const tpl_t *t;
    int nent;        /* entities the arrays below hold (the template may change before they follow) */
    mb_tcp_t conn;
    uint32_t cycle;
    int64_t next_us, last_ok_us;
    uint8_t fails;
    bool online;
    char error[64];
    uint32_t polls, errors;
    double *val;
    uint8_t *vstate; /* VAL_* */
    bool *single;
    char **text;     /* str entities */
} mbdev_t;

enum { VAL_NONE, VAL_OK, VAL_UNSUPPORTED };

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static tpl_t *s_tpl;          /* PSRAM, MAX_TPL */
static int s_ntpl;
static mbdev_t *s_dev;          /* PSRAM, MAX_DEVICES */
static int s_ndev;
static mb_serial_t s_serial = {9600, 'N', 1};
static uint16_t s_timeout_ms = 500;
static bool s_cfg_dirty;      /* the task re-creates the devices */
static char *s_cfg_pending;   /* new configuration JSON for the task */

/* ---------------------------------------------------------------- helpers */

static uint32_t crc32(const char *s)
{
    uint32_t c = 0xffffffff;
    for (; *s; s++) {
        c ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) {
            c = c & 1 ? (c >> 1) ^ 0xedb88320 : c >> 1;
        }
    }
    return ~c;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xc0) != 0x80;
    }
    return n;
}

static void *ps_calloc(size_t n, size_t size)
{
    return heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM);
}

static const tpl_t *find_tpl(const char *id)
{
    for (int i = 0; i < s_ntpl; i++) {
        if (strcmp(s_tpl[i].t.id, id) == 0) {
            return &s_tpl[i];
        }
    }
    return NULL;
}

static mbdev_t *find_dev(const char *id)
{
    for (int i = 0; i < s_ndev; i++) {
        if (strcmp(s_dev[i].id, id) == 0) {
            return &s_dev[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- templates */

static bool add_tpl(const cJSON *j, int slot, bool builtin, char *err, size_t err_len)
{
    if (s_ntpl == MAX_TPL) {
        snprintf(err, err_len, "too many templates");
        return false;
    }
    tpl_t *t = &s_tpl[s_ntpl];
    if (!mb_template_parse(j, &t->t, err, err_len)) {
        return false;
    }
    t->t.builtin = builtin;
    t->slot = slot;
    char *text = cJSON_PrintUnformatted(j);
    t->rev = text ? crc32(text) : 0;
    cJSON_free(text);
    s_ntpl++;
    return true;
}

static void load_templates(void)
{
    char err[160];
    cJSON *list = cJSON_Parse(templates_json_start);
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        if (!add_tpl(j, -1, true, err, sizeof(err))) {
            ESP_LOGE(TAG, "built-in template: %s", err);
        }
    }
    cJSON_Delete(list);
    for (int slot = 0; slot < MAX_USER_TPL; slot++) {
        char key[16];
        size_t len = 0;
        snprintf(key, sizeof(key), KEY_TPL, slot);
        if (gw_config_blob_size(key, &len) != ESP_OK || !len || len > USER_TPL_MAX_BYTES) {
            continue;
        }
        char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
        if (text && gw_config_get_blob(key, text, len)) {
            text[len] = '\0';
            cJSON *tj = cJSON_Parse(text);
            const char *id = tj ? GW_JSON_STR(tj, "id") : NULL;
            if (id && find_tpl(id)) {
                ESP_LOGW(TAG, "user template %s: id taken", id);
            } else if (!add_tpl(tj, slot, false, err, sizeof(err))) {
                ESP_LOGW(TAG, "user template in %s: %s", key, err);
            }
            cJSON_Delete(tj);
        }
        free(text);
    }
    ESP_LOGI(TAG, "%d templates", s_ntpl);
}

/* ---------------------------------------------------------------- configuration */

static void free_runtime(mbdev_t *d)
{
    mb_tcp_close(&d->conn);
    if (d->text) {
        for (int i = 0; i < d->nent; i++) {
            free(d->text[i]);
        }
    }
    free(d->val);
    free(d->vstate);
    free(d->single);
    free(d->text);
    d->val = NULL, d->vstate = NULL, d->single = NULL, d->text = NULL;
}

static bool alloc_runtime(mbdev_t *d)
{
    int n = d->t ? d->t->t.nent : 0;
    d->nent = n;
    mb_tcp_init(&d->conn);
    if (!n) {
        return true;
    }
    d->val = ps_calloc(n, sizeof(double));
    d->vstate = ps_calloc(n, 1);
    d->single = ps_calloc(n, sizeof(bool));
    d->text = ps_calloc(n, sizeof(char *));
    return d->val && d->vstate && d->single && d->text;
}

static const char *parse_expose(const char *s, uint8_t *out)
{
    if (!s || strcmp(s, "integration") == 0) {
        *out = EXPOSE_INTEGRATION;
    } else if (strcmp(s, "mqtt") == 0) {
        *out = EXPOSE_MQTT;
    } else if (strcmp(s, "both") == 0) {
        *out = EXPOSE_INTEGRATION | EXPOSE_MQTT;
    } else if (strcmp(s, "none") == 0) {
        *out = 0;
    } else {
        return "expose: integration, mqtt, both or none";
    }
    return NULL;
}

static const char *expose_name(uint8_t e)
{
    return e == 3 ? "both" : e == EXPOSE_MQTT ? "mqtt" : e == EXPOSE_INTEGRATION ? "integration" : "none";
}

/* One device from JSON, checked (templates must exist). NULL when fine, else why not. */
static const char *parse_device(const cJSON *j, mbdev_t *d)
{
    memset(d, 0, sizeof(*d));
    const char *id = GW_JSON_STR(j, "id"), *name = GW_JSON_STR(j, "name"), *tpl = GW_JSON_STR(j, "template");
    const char *transport = GW_JSON_STR(j, "transport"), *host = GW_JSON_STR(j, "host");
    const cJSON *slave = cJSON_GetObjectItem(j, "slave"), *port = cJSON_GetObjectItem(j, "port"), *poll = cJSON_GetObjectItem(j, "poll_s");
    const cJSON *enabled = cJSON_GetObjectItem(j, "enabled");
    if (id && (strlen(id) >= sizeof(d->id) || !id[0] || strspn(id, "abcdefghijklmnopqrstuvwxyz0123456789") != strlen(id))) {
        return "id: a-z, 0-9, up to 7 characters";
    }
    if (!tpl || !(d->t = find_tpl(tpl))) {
        return "template: unknown";
    }
    if (name && (utf8_chars(name) > NAME_CHARS || strlen(name) >= sizeof(d->name) || strpbrk(name, "\"\\{}"))) {
        return "name: up to 32 characters, no quotes, braces or backslash";
    }
    d->tcp = transport && strcmp(transport, "tcp") == 0;
    if (transport && !d->tcp && strcmp(transport, "rtu") != 0) {
        return "transport: rtu or tcp";
    }
    if (d->tcp && (!host || !host[0] || strlen(host) >= sizeof(d->host) || strpbrk(host, " /"))) {
        return "host: the address of the Modbus TCP device";
    }
    int p = cJSON_IsNumber(port) ? port->valueint : 502;
    if (port && (!cJSON_IsNumber(port) || p < 1 || p > 65535)) {
        return "port: 1-65535";
    }
    int s = cJSON_IsNumber(slave) ? slave->valueint : d->t->t.slave;
    if ((slave && !cJSON_IsNumber(slave)) || s < (d->tcp ? 0 : 1) || s > (d->tcp ? 255 : 247)) {
        return "slave: 1-247 (TCP: unit 0-255)";
    }
    int ps = cJSON_IsNumber(poll) ? poll->valueint : 0;
    if (poll && (!cJSON_IsNumber(poll) || (ps != 0 && (ps < 1 || ps > 3600)))) {
        return "poll_s: 0 (the template's) or 1-3600";
    }
    if (enabled && !cJSON_IsBool(enabled)) {
        return "enabled: true or false";
    }
    const char *why = parse_expose(GW_JSON_STR(j, "expose"), &d->expose);
    if (why) {
        return why;
    }
    if (id) {
        strcpy(d->id, id);
    }
    strcpy(d->tpl, tpl);
    snprintf(d->name, sizeof(d->name), "%s", name && name[0] ? name : d->t->t.model);
    if (d->tcp) {
        strcpy(d->host, host);
    }
    d->port = (uint16_t)p;
    d->slave = (uint8_t)s;
    d->poll_s = (uint16_t)ps;
    d->enabled = !enabled || cJSON_IsTrue(enabled);
    return NULL;
}

static const char *parse_bus(const cJSON *b, mb_serial_t *ser, uint16_t *timeout)
{
    const cJSON *baud = cJSON_GetObjectItem(b, "baud"), *stop = cJSON_GetObjectItem(b, "stop"), *to = cJSON_GetObjectItem(b, "timeout_ms");
    const char *parity = GW_JSON_STR(b, "parity");
    static const int bauds[] = {1200, 2400, 4800, 9600, 19200, 38400, 57600, 115200};
    if (baud) {
        bool ok = false;
        for (size_t i = 0; cJSON_IsNumber(baud) && i < sizeof(bauds) / sizeof(bauds[0]); i++) {
            ok |= baud->valueint == bauds[i];
        }
        if (!ok) {
            return "bus.baud: 1200, 2400, 4800, 9600, 19200, 38400, 57600 or 115200";
        }
        ser->baud = baud->valueint;
    }
    if (parity) {
        if (strlen(parity) != 1 || !strchr("NEO", parity[0])) {
            return "bus.parity: N, E or O";
        }
        ser->parity = parity[0];
    }
    if (stop) {
        if (!cJSON_IsNumber(stop) || (stop->valueint != 1 && stop->valueint != 2)) {
            return "bus.stop: 1 or 2";
        }
        ser->stop = (uint8_t)stop->valueint;
    }
    if (to) {
        if (!cJSON_IsNumber(to) || to->valueint < 50 || to->valueint > 5000) {
            return "bus.timeout_ms: 50-5000";
        }
        *timeout = (uint16_t)to->valueint;
    }
    return NULL;
}

static cJSON *config_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *bus = cJSON_AddObjectToObject(root, "bus");
    cJSON_AddNumberToObject(bus, "baud", s_serial.baud);
    char p[2] = {s_serial.parity, 0};
    cJSON_AddStringToObject(bus, "parity", p);
    cJSON_AddNumberToObject(bus, "stop", s_serial.stop);
    cJSON_AddNumberToObject(bus, "timeout_ms", s_timeout_ms);
    cJSON *list = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < s_ndev; i++) {
        const mbdev_t *d = &s_dev[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", d->id);
        cJSON_AddStringToObject(j, "name", d->name);
        cJSON_AddStringToObject(j, "template", d->tpl);
        cJSON_AddStringToObject(j, "transport", d->tcp ? "tcp" : "rtu");
        cJSON_AddNumberToObject(j, "slave", d->slave);
        if (d->tcp) {
            cJSON_AddStringToObject(j, "host", d->host);
            cJSON_AddNumberToObject(j, "port", d->port);
        }
        cJSON_AddNumberToObject(j, "poll_s", d->poll_s);
        cJSON_AddStringToObject(j, "expose", expose_name(d->expose));
        cJSON_AddBoolToObject(j, "enabled", d->enabled);
        cJSON_AddItemToArray(list, j);
    }
    return root;
}

/* Applies a configuration (checked by the caller). Task or start only, under s_lock. Devices whose
 * transport and template stay keep their values. */
static void apply_config(const cJSON *root)
{
    const cJSON *bus = cJSON_GetObjectItem(root, "bus");
    if (bus) {
        mb_serial_t ser = s_serial;
        parse_bus(bus, &ser, &s_timeout_ms);
        if (memcmp(&ser, &s_serial, sizeof(ser)) != 0 || !mb_rtu_ready()) {
            s_serial = ser;
            mb_rtu_open(&s_serial);
        }
    }
    const cJSON *list = cJSON_GetObjectItem(root, "devices");
    if (!list) {
        return;
    }
    mbdev_t *next = ps_calloc(MAX_DEVICES, sizeof(mbdev_t));
    int n = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        if (!next || n == MAX_DEVICES || parse_device(j, &next[n])) {
            continue;
        }
        mbdev_t *d = &next[n];
        mbdev_t *old = d->id[0] ? find_dev(d->id) : NULL;
        if (!d->id[0]) {
            /* a new device: the lowest free number */
            for (int k = 1; !d->id[0]; k++) {
                char id[8];
                snprintf(id, sizeof(id), "d%d", k % 1000000);
                bool used = find_dev(id) != NULL;
                for (int m = 0; m < n && !used; m++) {
                    used = strcmp(next[m].id, id) == 0;
                }
                if (!used) {
                    strcpy(d->id, id);
                }
            }
        }
        if (old && old->t == d->t && old->tcp == d->tcp && !strcmp(old->host, d->host) && old->port == d->port && old->slave == d->slave &&
            old->enabled && d->enabled) {
            /* same device: keep its runtime */
            memcpy(&d->t, &old->t, sizeof(*d) - offsetof(mbdev_t, t));
            old->val = NULL, old->vstate = NULL, old->single = NULL, old->text = NULL;
            old->conn.sock = -1;
            d->next_us = 0;
        } else if (!alloc_runtime(d)) {
            free_runtime(d);
            continue;
        }
        n++;
    }
    for (int i = 0; i < s_ndev; i++) {
        free_runtime(&s_dev[i]);
    }
    if (next) {
        memcpy(s_dev, next, sizeof(mbdev_t) * MAX_DEVICES);
        free(next);
        s_ndev = n;
    }
}

static void save_config(void)
{
    cJSON *j = config_json();
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(KEY_CFG, text);
        cJSON_free(text);
    }
}

/* ---------------------------------------------------------------- requests */

static int request(mbdev_t *d, const mb_req_t *req, uint8_t *data, size_t *len)
{
    const mb_template_t *t = &d->t->t;
    uint16_t timeout = t->timeout_ms > s_timeout_ms ? t->timeout_ms : s_timeout_ms;
    return d->tcp ? mb_tcp_request(&d->conn, d->host, d->port, d->slave, req, timeout, data, len)
                  : mb_rtu_request(d->slave, req, timeout, t->gap_ms, data, len);
}

static uint8_t read_fc(mb_space_t space)
{
    static const uint8_t fc[] = {MB_FC_READ_COILS, MB_FC_READ_DISCRETE, MB_FC_READ_INPUT, MB_FC_READ_HOLDING};
    return fc[space];
}

/* Values of the entities of a block from the data read; under s_lock. */
static void store_block(mbdev_t *d, const uint16_t *idx, const mb_block_t *b, const uint8_t *data)
{
    const mb_template_t *t = &d->t->t;
    for (int k = 0; k < b->n; k++) {
        int i = idx[b->first + k];
        const mb_entity_t *e = &t->ent[i];
        int off = e->addr - b->addr;
        double raw;
        bool ok;
        if (e->space == MB_SPACE_COIL || e->space == MB_SPACE_DISCRETE) {
            raw = (data[off / 8] >> (off % 8)) & 1;
            ok = true;
        } else if (e->type == MB_T_STR) {
            char s[129];
            int n = 0;
            for (int r = 0; r < e->count; r++) {
                for (int c = 0; c < 2; c++) {
                    char ch = (char)data[2 * (off + r) + c];
                    if (ch >= 32 && ch < 127) {
                        s[n++] = ch;
                    }
                }
            }
            while (n && s[n - 1] == ' ') {
                n--;
            }
            s[n] = '\0';
            free(d->text[i]);
            d->text[i] = strdup(s);
            d->vstate[i] = VAL_OK;
            continue;
        } else {
            ok = mb_decode(data + 2 * off, e->type, e->order, e->bit, &raw);
        }
        if (ok && e->has_nan && raw == e->nan_raw) {
            ok = false;
        }
        d->val[i] = ok ? (e->nopts ? raw : mb_entity_value(e, raw)) : 0;
        d->vstate[i] = ok ? VAL_OK : VAL_NONE;
    }
}

static cJSON *values_json(const mbdev_t *d);

/* One poll of a device: the entities due in this cycle, block by block. */
static void poll_device(mbdev_t *d, int64_t now)
{
    const mb_template_t *t = &d->t->t;
    if (d->nent != t->nent) {
        d->next_us = now + 1000000; /* its template was replaced: wait for the new runtime */
        return;
    }
    uint16_t idx[MB_ENTITIES_MAX];
    mb_block_t blocks[MB_ENTITIES_MAX];
    static uint8_t data[2 * MB_READ_REGS_MAX];
    int n = 0;
    for (int i = 0; i < t->nent; i++) {
        const mb_entity_t *e = &t->ent[i];
        bool due = e->every ? d->cycle % e->every == 0 : d->vstate[i] != VAL_OK;
        if (!e->space_none && d->vstate[i] != VAL_UNSUPPORTED && due) {
            idx[n++] = (uint16_t)i;
        }
    }
    int nb = mb_plan(t, idx, n, d->single, blocks, MB_ENTITIES_MAX);
    int answered = 0, last_err = MB_OK;
    for (int k = 0; k < nb; k++) {
        const mb_block_t *b = &blocks[k];
        mb_req_t req = {.fc = read_fc(b->space), .addr = b->addr, .count = b->count};
        size_t len = sizeof(data);
        int err = request(d, &req, data, &len);
        if (err == MB_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            store_block(d, idx, b, data);
            xSemaphoreGive(s_lock);
            answered++;
            continue;
        }
        last_err = err;
        if (err > 0) {
            answered++; /* the device is there, it refuses this read */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            for (int m = 0; m < b->n; m++) {
                int i = idx[b->first + m];
                if (b->n > 1) {
                    d->single[i] = true; /* read the entities one by one from now on */
                } else if (err == 2 || err == 1) {
                    d->vstate[i] = VAL_UNSUPPORTED;
                    ESP_LOGW(TAG, "%s %s: %s, left out", d->id, t->ent[i].key, mb_err_name(err));
                }
            }
            xSemaphoreGive(s_lock);
        } else if (err == MB_ERR_TIMEOUT || err == MB_ERR_IO) {
            break; /* no device: no point in the other blocks */
        }
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    d->polls++;
    d->cycle++;
    if (answered) {
        d->fails = 0;
        d->online = true;
        d->last_ok_us = now;
        if (last_err == MB_OK) {
            d->error[0] = '\0';
        } else {
            snprintf(d->error, sizeof(d->error), "%s", mb_err_name(last_err));
        }
    } else if (nb) {
        d->errors++;
        snprintf(d->error, sizeof(d->error), "%s", mb_err_name(last_err));
        if (++d->fails >= OFFLINE_AFTER && d->online) {
            ESP_LOGW(TAG, "%s (%s) offline: %s", d->id, d->name, d->error);
            d->online = false;
            memset(d->vstate, 0, t->nent); /* also those read once: read them again when it is back */
            memset(d->single, 0, t->nent * sizeof(bool));
        }
    }
    int poll_s = d->poll_s ? d->poll_s : t->poll_s;
    d->next_us = now + (int64_t)(d->online || d->fails < OFFLINE_AFTER ? poll_s : OFFLINE_RETRY_S) * 1000000;
    cJSON *v = d->expose ? values_json(d) : NULL;
    bool online = d->online;
    char topic[32];
    snprintf(topic, sizeof(topic), "modbus/%s", d->id);
    xSemaphoreGive(s_lock);
    if (v) {
        char *text = cJSON_PrintUnformatted(v);
        cJSON_Delete(v);
        if (text) {
            gw_mqtt_publish(topic, text, false); /* also the push WebSocket of the integration */
            cJSON_free(text);
        }
        strlcat(topic, "/availability", sizeof(topic));
        gw_mqtt_publish(topic, online ? "online" : "offline", true);
    }
}

/* value of an entity for the API, MQTT and Telegram: number, bool, label or text; under s_lock */
static cJSON *value_item(const mbdev_t *d, int i)
{
    const mb_entity_t *e = &d->t->t.ent[i];
    if (d->vstate[i] != VAL_OK) {
        return NULL;
    }
    if (e->type == MB_T_STR) {
        return cJSON_CreateString(d->text[i] ? d->text[i] : "");
    }
    if (e->nopts) {
        const char *label = mb_entity_label(e, d->val[i]);
        if (label) {
            return cJSON_CreateString(label);
        }
        return cJSON_CreateNumber(d->val[i]);
    }
    if (e->type == MB_T_BOOL) {
        return cJSON_CreateBool(d->val[i] != 0);
    }
    return cJSON_CreateNumber(d->val[i]);
}

static cJSON *values_json(const mbdev_t *d)
{
    cJSON *v = cJSON_CreateObject();
    for (int i = 0; d->t && i < d->nent && i < d->t->t.nent; i++) {
        cJSON *item = value_item(d, i);
        if (item) {
            cJSON_AddItemToObject(v, d->t->t.ent[i].key, item);
        }
    }
    return v;
}

/* ---------------------------------------------------------------- writes */

/* Writes value (text: a number, a label of the entity's map, ON/OFF, PRESS) to an entity. Task only. */
static int write_entity(mbdev_t *d, const char *key, const char *value, char *err, size_t err_len)
{
    const mb_entity_t *e = mb_template_entity(&d->t->t, key);
    if (d->nent != d->t->t.nent) {
        snprintf(err, err_len, "device %s is being reconfigured", d->id);
        return MB_ERR_ARG;
    }
    if (!e || !e->write) {
        snprintf(err, err_len, "%s: not writable", key);
        return MB_ERR_ARG;
    }
    double v = 0;
    char *end;
    switch (e->write) {
    case MB_W_SWITCH:
        v = !strcasecmp(value, "ON") || !strcmp(value, "1") || !strcasecmp(value, "true") ? e->on : e->off;
        break;
    case MB_W_BUTTON:
        v = e->press;
        break;
    case MB_W_SELECT: {
        bool found = false;
        for (int k = 0; k < e->nopts && !found; k++) {
            if (strcmp(e->opts[k].label, value) == 0) {
                v = e->opts[k].raw;
                found = true;
            }
        }
        if (!found) {
            v = strtod(value, &end);
            found = *end == '\0' && mb_entity_label(e, v);
        }
        if (!found) {
            snprintf(err, err_len, "%s: not an option", value);
            return MB_ERR_ARG;
        }
        break;
    }
    default:
        v = strtod(value, &end);
        if (*end || !value[0] || v < e->min || v > e->max) {
            snprintf(err, err_len, "%s: a number %g to %g", key, e->min, e->max);
            return MB_ERR_ARG;
        }
        v = (v - e->offset) / e->scale; /* back to the raw value */
        break;
    }
    mb_req_t req = {0};
    uint16_t regs[4];
    int nregs = 0;
    if (e->space_none) {
        req.fc = e->write_fc;
    } else if (e->space == MB_SPACE_COIL) {
        req = (mb_req_t){.fc = MB_FC_WRITE_COIL, .addr = e->addr, .value = v != 0 ? 0xff00 : 0x0000};
    } else if (e->bit >= 0) {
        /* one bit of a holding register: read, change, write */
        uint8_t cur[2];
        size_t len = 2;
        mb_req_t rd = {.fc = MB_FC_READ_HOLDING, .addr = e->addr, .count = 1};
        int r = request(d, &rd, cur, &len);
        if (r != MB_OK) {
            snprintf(err, err_len, "%s", mb_err_name(r));
            return r;
        }
        uint16_t w = (uint16_t)(cur[0] << 8 | cur[1]);
        w = v != 0 ? w | (1u << e->bit) : w & ~(1u << e->bit);
        req = (mb_req_t){.fc = e->write_fc == 16 ? MB_FC_WRITE_REGS : MB_FC_WRITE_REG, .addr = e->addr, .count = 1, .value = w};
        regs[0] = w;
        req.regs = regs;
    } else {
        if (!mb_encode(v, e->type, e->order, regs, &nregs)) {
            snprintf(err, err_len, "%s: %g does not fit %s", key, v, mb_type_name(e->type));
            return MB_ERR_ARG;
        }
        bool multiple = nregs > 1 || e->write_fc == 16;
        req = (mb_req_t){.fc = multiple ? MB_FC_WRITE_REGS : MB_FC_WRITE_REG, .addr = e->addr, .count = (uint16_t)nregs,
                         .regs = regs, .value = regs[0]};
    }
    int r = request(d, &req, NULL, NULL);
    if (r != MB_OK) {
        snprintf(err, err_len, "%s", mb_err_name(r));
    } else {
        ESP_LOGI(TAG, "%s %s = %s", d->id, key, value);
        d->next_us = 0; /* read the new state soon */
        if (!e->space_none && e->every == 0) {
            d->vstate[e - d->t->t.ent] = VAL_NONE;
        }
    }
    return r;
}

/* writes from MQTT, queued by the MQTT task */
typedef struct {
    char dev[8], key[MB_KEY_LEN], value[48];
} queued_write_t;
static QueueHandle_t s_wq;

/* one synchronous job of the API (write, manual request), run by the task */
typedef enum { JOB_NONE, JOB_WRITE, JOB_RAW } job_kind_t;
static struct {
    SemaphoreHandle_t lock, done;
    volatile bool busy;
    job_kind_t kind;
    char dev[8], key[MB_KEY_LEN], value[48];
    /* raw */
    mbdev_t raw;          /* a temporary device: transport and address */
    mb_template_t raw_tpl;
    tpl_t raw_t;
    mb_req_t req;
    uint16_t regs[123];
    uint8_t data[2 * MB_READ_REGS_MAX];
    size_t len;
    int result;
    char err[96];
} s_job;

/* ---------------------------------------------------------------- bus search */

static struct {
    bool running, stop;
    bool tcp;
    char host[64];
    uint16_t port;
    uint32_t bauds[8];
    int nbauds, baud_i;
    int from, to, next;
    const tpl_t *t;
    int found_n;
    struct {
        uint8_t slave;
        uint32_t baud;
    } found[32];
    int64_t started;
} s_probe;

static mb_tcp_t s_probe_conn = {.sock = -1};

/* one address of the search: reads the first entity of the template (or holding register 0) */
static void probe_step(void)
{
    if (s_probe.next > s_probe.to) {
        s_probe.next = s_probe.from;
        if (s_probe.tcp || ++s_probe.baud_i >= s_probe.nbauds) {
            s_probe.running = false;
            mb_tcp_close(&s_probe_conn);
            mb_rtu_open(&s_serial); /* the bus settings again */
            ESP_LOGI(TAG, "search done: %d found", s_probe.found_n);
            return;
        }
    }
    if (!s_probe.tcp && s_probe.next == s_probe.from) {
        mb_serial_t ser = s_serial;
        ser.baud = s_probe.bauds[s_probe.baud_i];
        if (s_probe.t) {
            ser.parity = s_probe.t->t.parity;
            ser.stop = s_probe.t->t.stop;
        }
        mb_rtu_open(&ser);
    }
    mb_req_t req = {.fc = MB_FC_READ_HOLDING, .addr = 0, .count = 1};
    if (s_probe.t) {
        for (int i = 0; i < s_probe.t->t.nent; i++) {
            const mb_entity_t *e = &s_probe.t->t.ent[i];
            if (!e->space_none) {
                req = (mb_req_t){.fc = read_fc(e->space), .addr = e->addr, .count = (uint16_t)mb_entity_regs(e)};
                break;
            }
        }
    }
    uint8_t data[64];
    size_t len = sizeof(data);
    int slave = s_probe.next++;
    mb_tcp_t *conn = &s_probe_conn;
    int err = s_probe.tcp ? mb_tcp_request(conn, s_probe.host, s_probe.port, (uint8_t)slave, &req, 500, data, &len)
                          : mb_rtu_request((uint8_t)slave, &req, 150, 20, data, &len);
    /* an exception is an answer too: something has this address */
    if ((err == MB_OK || err > 0) && s_probe.found_n < 32) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_probe.found[s_probe.found_n].slave = (uint8_t)slave;
        s_probe.found[s_probe.found_n].baud = s_probe.tcp ? 0 : s_probe.bauds[s_probe.baud_i];
        s_probe.found_n++;
        xSemaphoreGive(s_lock);
    }
    if (s_probe.tcp && err == MB_ERR_IO && conn->sock < 0) {
        s_probe.running = false; /* the host does not take connections */
    }
}

/* ---------------------------------------------------------------- task */

static void run_job(void)
{
    s_job.err[0] = '\0';
    if (s_job.kind == JOB_WRITE) {
        mbdev_t *d = find_dev(s_job.dev);
        if (!d || !d->enabled) {
            snprintf(s_job.err, sizeof(s_job.err), "device %s: not configured or off", s_job.dev);
            s_job.result = MB_ERR_ARG;
        } else {
            s_job.result = write_entity(d, s_job.key, s_job.value, s_job.err, sizeof(s_job.err));
        }
    } else if (s_job.kind == JOB_RAW) {
        s_job.len = sizeof(s_job.data);
        mb_tcp_init(&s_job.raw.conn);
        s_job.result = request(&s_job.raw, &s_job.req, s_job.data, &s_job.len);
        mb_tcp_close(&s_job.raw.conn);
        if (s_job.result != MB_OK) {
            snprintf(s_job.err, sizeof(s_job.err), "%s", mb_err_name(s_job.result));
        }
    }
    s_job.kind = JOB_NONE;
    s_job.busy = false;
    xSemaphoreGive(s_job.done);
}

static void task(void *arg)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(s_probe.running ? 1 : 200));
        if (s_cfg_dirty) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            cJSON *j = cJSON_Parse(s_cfg_pending);
            free(s_cfg_pending);
            s_cfg_pending = NULL;
            s_cfg_dirty = false;
            if (j) {
                apply_config(j);
                cJSON_Delete(j);
            }
            xSemaphoreGive(s_lock);
            save_config();
            gw_mqtt_rediscover();
        }
        if (s_job.busy) {
            run_job();
        }
        queued_write_t w;
        while (xQueueReceive(s_wq, &w, 0) == pdTRUE) {
            mbdev_t *d = find_dev(w.dev);
            char err[96];
            if (d && d->enabled && write_entity(d, w.key, w.value, err, sizeof(err)) != MB_OK) {
                ESP_LOGW(TAG, "write %s %s: %s", w.dev, w.key, err);
            }
        }
        if (s_probe.running) {
            if (s_probe.stop) {
                s_probe.running = false;
                mb_tcp_close(&s_probe_conn);
                mb_rtu_open(&s_serial);
            } else {
                probe_step();
            }
            if (!s_probe.tcp) {
                continue; /* the RS485 bus belongs to the search meanwhile */
            }
        }
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < s_ndev; i++) {
            mbdev_t *d = &s_dev[i];
            if (d->enabled && d->t && now >= d->next_us) {
                poll_device(d, now);
                break; /* one device per round: jobs and writes in between */
            }
        }
    }
}

/* ---------------------------------------------------------------- HTTP */

static cJSON *status_json(void)
{
    int64_t now = esp_timer_get_time();
    cJSON *root = config_json();
    cJSON *bus = cJSON_GetObjectItem(root, "bus");
    cJSON_AddBoolToObject(bus, "ok", mb_rtu_ready());
    cJSON_AddNumberToObject(bus, "uart", CONFIG_GW_RS485_UART_PORT);
    cJSON_AddNumberToObject(bus, "tx", CONFIG_GW_RS485_TX);
    cJSON_AddNumberToObject(bus, "rx", CONFIG_GW_RS485_RX);
    mb_bus_stats_t st = mb_rtu_stats();
    cJSON *stats = cJSON_AddObjectToObject(bus, "stats");
    cJSON_AddNumberToObject(stats, "requests", st.requests);
    cJSON_AddNumberToObject(stats, "timeouts", st.timeouts);
    cJSON_AddNumberToObject(stats, "crc_errors", st.crc_errors);
    cJSON_AddNumberToObject(stats, "exceptions", st.exceptions);
    cJSON_AddNumberToObject(root, "max_devices", MAX_DEVICES);
    const cJSON *j;
    int i = 0;
    cJSON_ArrayForEach(j, cJSON_GetObjectItem(root, "devices"))
    {
        const mbdev_t *d = &s_dev[i++];
        cJSON *o = (cJSON *)j;
        cJSON_AddStringToObject(o, "manufacturer", d->t->t.manufacturer);
        cJSON_AddStringToObject(o, "model", d->t->t.model);
        cJSON_AddNumberToObject(o, "template_rev", d->t->rev);
        cJSON_AddStringToObject(o, "state", !d->enabled ? "off" : d->online ? (d->error[0] ? "partial" : "ok") : d->polls ? "offline" : "starting");
        cJSON_AddStringToObject(o, "error", d->error);
        cJSON_AddNumberToObject(o, "age_s", d->last_ok_us ? (double)((now - d->last_ok_us) / 1000000) : -1);
        cJSON_AddNumberToObject(o, "polls", d->polls);
        cJSON_AddNumberToObject(o, "errors", d->errors);
        cJSON_AddItemToObject(o, "values", d->enabled && d->online ? values_json(d) : cJSON_CreateObject());
    }
    return root;
}

static esp_err_t modbus_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *j = status_json();
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

/* {"bus": {...}, "devices": [...]}: both optional; devices replaces the list (devices without id are new) */
static esp_err_t modbus_put(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json_max(req, 8192);
    if (!body) {
        return ESP_OK;
    }
    char err[96] = "";
    mb_serial_t ser = s_serial;
    uint16_t to = s_timeout_ms;
    const cJSON *bus = cJSON_GetObjectItem(body, "bus"), *list = cJSON_GetObjectItem(body, "devices");
    const char *why = bus ? parse_bus(bus, &ser, &to) : NULL;
    if (why) {
        snprintf(err, sizeof(err), "%s", why);
    } else if (list && (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > MAX_DEVICES)) {
        snprintf(err, sizeof(err), "devices: a list of up to %d", MAX_DEVICES);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int k = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        mbdev_t d;
        why = err[0] ? NULL : parse_device(j, &d);
        if (why) {
            snprintf(err, sizeof(err), "devices[%d] %s", k, why);
        }
        k++;
    }
    /* the whole new configuration: what was not sent stays */
    cJSON *cfg = config_json();
    xSemaphoreGive(s_lock);
    if (err[0]) {
        cJSON_Delete(body);
        cJSON_Delete(cfg);
        return gw_http_send_error(req, 400, err);
    }
    if (bus) {
        cJSON_ReplaceItemInObject(cfg, "bus", cJSON_Duplicate(bus, true));
    }
    if (list) {
        cJSON_ReplaceItemInObject(cfg, "devices", cJSON_Duplicate(list, true));
    }
    cJSON_Delete(body);
    char *text = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    free(s_cfg_pending);
    s_cfg_pending = text ? strdup(text) : NULL;
    s_cfg_dirty = s_cfg_pending != NULL;
    xSemaphoreGive(s_lock);
    cJSON_free(text);
    xTaskNotifyGive(s_task);
    for (int i = 0; i < 40 && s_cfg_dirty; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    return modbus_get(req);
}

static cJSON *tpl_summary(const tpl_t *t)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", t->t.id);
    cJSON_AddStringToObject(o, "manufacturer", t->t.manufacturer);
    cJSON_AddStringToObject(o, "model", t->t.model);
    cJSON_AddStringToObject(o, "category", t->t.category);
    cJSON_AddStringToObject(o, "notes", t->t.notes);
    cJSON_AddBoolToObject(o, "builtin", t->t.builtin);
    cJSON_AddNumberToObject(o, "entities", t->t.nent);
    cJSON_AddNumberToObject(o, "rev", t->rev);
    cJSON_AddNumberToObject(o, "baud", t->t.baud);
    char p[4] = {'8', t->t.parity, (char)('0' + t->t.stop), 0};
    cJSON_AddStringToObject(o, "serial", p);
    cJSON_AddNumberToObject(o, "slave", t->t.slave);
    return o;
}

static esp_err_t templates_get(httpd_req_t *req)
{
    cJSON *list = cJSON_CreateArray();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ntpl; i++) {
        cJSON_AddItemToArray(list, tpl_summary(&s_tpl[i]));
    }
    xSemaphoreGive(s_lock);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "templates", list);
    cJSON_AddNumberToObject(root, "max_user", MAX_USER_TPL);
    return gw_http_send_json(req, root);
}

static bool query_id(httpd_req_t *req, char *id, size_t len)
{
    char q[64];
    return httpd_req_get_url_query_str(req, q, sizeof(q)) == ESP_OK && httpd_query_key_value(q, "id", id, len) == ESP_OK && id[0];
}

/* GET ?id=: the whole template (JSON as it is written) */
static esp_err_t template_get(httpd_req_t *req)
{
    char id[32];
    if (!query_id(req, id, sizeof(id))) {
        return gw_http_send_error(req, 400, "id: a template id");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const tpl_t *t = find_tpl(id);
    cJSON *j = t ? mb_template_json(&t->t) : NULL;
    if (j) {
        cJSON_AddBoolToObject(j, "builtin", t->t.builtin);
        cJSON_AddNumberToObject(j, "rev", t->rev);
    }
    xSemaphoreGive(s_lock);
    return j ? gw_http_send_json(req, j) : gw_http_send_error(req, 404, "no such template");
}

/* PUT: a user template (new or replacing the user template with its id) */
static esp_err_t template_put(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json_max(req, USER_TPL_MAX_BYTES);
    if (!body) {
        return ESP_OK;
    }
    cJSON_DeleteItemFromObject(body, "builtin");
    cJSON_DeleteItemFromObject(body, "rev");
    mb_template_t t;
    char err[160];
    if (!mb_template_parse(body, &t, err, sizeof(err))) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, err);
    }
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tpl_t *old = (tpl_t *)find_tpl(t.id);
    int slot = old ? old->slot : -1;
    const char *fail = old && old->t.builtin ? "a built-in template has this id: choose another" : NULL;
    for (int s = 0; !fail && slot < 0 && s < MAX_USER_TPL; s++) {
        bool used = false;
        for (int i = 0; i < s_ntpl; i++) {
            used |= s_tpl[i].slot == s;
        }
        if (!used) {
            slot = s;
        }
    }
    if (!fail && slot < 0) {
        fail = "no room for another user template";
    }
    char key[16];
    snprintf(key, sizeof(key), KEY_TPL, slot);
    if (!fail && (!text || gw_config_set_blob(key, text, strlen(text)) != ESP_OK)) {
        fail = "not stored (NVS full?)";
    }
    if (!fail) {
        /* the in-memory copy: replaced in place (devices keep their pointer) or appended */
        t.builtin = false;
        if (old) {
            mb_template_free(&old->t);
            old->t = t;
            old->rev = crc32(text);
        } else if (s_ntpl < MAX_TPL) {
            s_tpl[s_ntpl] = (tpl_t){.t = t, .rev = crc32(text), .slot = slot};
            s_ntpl++;
        }
        /* devices of the template start again with its new entities */
        for (int i = 0; i < s_ndev; i++) {
            if (old && s_dev[i].t == old) {
                s_dev[i].next_us = 0;
            }
        }
    } else {
        mb_template_free(&t);
    }
    cJSON *cfg = !fail && old ? config_json() : NULL;
    xSemaphoreGive(s_lock);
    cJSON_free(text);
    if (fail) {
        return gw_http_send_error(req, 409, fail);
    }
    if (cfg) {
        /* the entities of a replaced template changed: re-create its devices */
        char *c = cJSON_PrintUnformatted(cfg);
        cJSON_Delete(cfg);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_cfg_pending);
        s_cfg_pending = c ? strdup(c) : NULL;
        /* devices are kept only with the same template pointer and entity count: force new runtimes */
        for (int i = 0; i < s_ndev; i++) {
            if (s_dev[i].t == old) {
                s_dev[i].enabled = false;
            }
        }
        s_cfg_dirty = s_cfg_pending != NULL;
        xSemaphoreGive(s_lock);
        cJSON_free(c);
        xTaskNotifyGive(s_task);
    }
    gw_mqtt_rediscover();
    return template_get(req) == ESP_OK ? ESP_OK : ESP_FAIL;
}

static esp_err_t template_delete(httpd_req_t *req)
{
    char id[32];
    if (!query_id(req, id, sizeof(id))) {
        return gw_http_send_error(req, 400, "id: a template id");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    tpl_t *t = (tpl_t *)find_tpl(id);
    const char *fail = !t ? "no such template" : t->t.builtin ? "built-in templates cannot be deleted" : NULL;
    for (int i = 0; !fail && i < s_ndev; i++) {
        if (s_dev[i].t == t) {
            fail = "a device uses it";
        }
    }
    if (!fail) {
        char key[16];
        snprintf(key, sizeof(key), KEY_TPL, t->slot);
        gw_config_erase(key);
        mb_template_free(&t->t);
        int i = (int)(t - s_tpl);
        memmove(&s_tpl[i], &s_tpl[i + 1], (s_ntpl - i - 1) * sizeof(tpl_t));
        s_ntpl--;
        /* devices point into s_tpl: re-point after the move */
        for (int k = 0; k < s_ndev; k++) {
            if (s_dev[k].t > t) {
                s_dev[k].t--;
            }
        }
    }
    xSemaphoreGive(s_lock);
    if (fail) {
        return gw_http_send_error(req, !t ? 404 : 409, fail);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "deleted", id);
    return gw_http_send_json(req, j);
}

/* runs a job in the task and waits; false: timed out (the task is busy, e.g. with a dead TCP device) */
static bool run_and_wait(uint32_t ms)
{
    s_job.busy = true;
    xTaskNotifyGive(s_task);
    return xSemaphoreTake(s_job.done, pdMS_TO_TICKS(ms)) == pdTRUE;
}

static bool job_begin(void)
{
    if (xSemaphoreTake(s_job.lock, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return false;
    }
    /* an earlier job that timed out may still run */
    for (int i = 0; i < 100 && s_job.busy; i++) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_job.busy) {
        xSemaphoreGive(s_job.lock);
        return false;
    }
    xSemaphoreTake(s_job.done, 0);
    return true;
}

/* {"device": "d1", "key": "...", "value": "..." | number | bool} */
static esp_err_t write_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *dev = GW_JSON_STR(body, "device"), *key = GW_JSON_STR(body, "key");
    const cJSON *value = cJSON_GetObjectItem(body, "value");
    char v[48] = "PRESS";
    if (cJSON_IsNumber(value)) {
        snprintf(v, sizeof(v), "%.10g", value->valuedouble);
    } else if (cJSON_IsBool(value)) {
        strcpy(v, cJSON_IsTrue(value) ? "ON" : "OFF");
    } else if (cJSON_IsString(value) && strlen(value->valuestring) < sizeof(v)) {
        strcpy(v, value->valuestring);
    } else if (value) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "value: a number, true/false or a text up to 47 characters");
    }
    if (!dev || !key || strlen(dev) >= sizeof(s_job.dev) || strlen(key) >= sizeof(s_job.key)) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "device and key required");
    }
    if (!job_begin()) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 503, "Modbus busy");
    }
    s_job.kind = JOB_WRITE;
    strcpy(s_job.dev, dev);
    strcpy(s_job.key, key);
    strcpy(s_job.value, v);
    cJSON_Delete(body);
    bool done = run_and_wait(8000);
    int result = s_job.result;
    char err[96];
    strlcpy(err, s_job.err, sizeof(err));
    xSemaphoreGive(s_job.lock);
    if (!done) {
        return gw_http_send_error(req, 503, "Modbus busy (no answer within 8 s)");
    }
    if (result != MB_OK) {
        return gw_http_send_error(req, result == MB_ERR_ARG ? 400 : 409, err);
    }
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "written", true);
    return gw_http_send_json(req, j);
}

/* Manual request: {"transport": "rtu"|"tcp", "host", "port", "slave", "fc", "addr", "count", "values": [..]} */
static esp_err_t raw_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *transport = GW_JSON_STR(body, "transport"), *host = GW_JSON_STR(body, "host");
    const cJSON *slave = cJSON_GetObjectItem(body, "slave"), *fc = cJSON_GetObjectItem(body, "fc"), *addr = cJSON_GetObjectItem(body, "addr");
    const cJSON *count = cJSON_GetObjectItem(body, "count"), *values = cJSON_GetObjectItem(body, "values"), *port = cJSON_GetObjectItem(body, "port");
    bool tcp = transport && strcmp(transport, "tcp") == 0;
    const char *why = NULL;
    int f = cJSON_IsNumber(fc) ? fc->valueint : 0;
    int n = cJSON_IsNumber(count) ? count->valueint : 1;
    if (!cJSON_IsNumber(slave) || slave->valueint < 0 || slave->valueint > 255) {
        why = "slave: 1-247 (TCP unit 0-255)";
    } else if (tcp && (!host || !host[0] || strlen(host) >= 64)) {
        why = "host required for tcp";
    } else if (!(f >= 1 && f <= 6) && f != 15 && f != 16) {
        why = "fc: 1, 2, 3, 4, 5, 6, 15 or 16";
    } else if (!cJSON_IsNumber(addr) || addr->valueint < 0 || addr->valueint > 65535) {
        why = "addr: 0-65535";
    } else if ((f == 3 || f == 4) && (n < 1 || n > MB_READ_REGS_MAX)) {
        why = "count: 1-125 registers";
    } else if ((f == 1 || f == 2) && (n < 1 || n > 2000)) {
        why = "count: 1-2000 bits";
    } else if ((f == 5 || f == 6 || f == 15 || f == 16) && (!cJSON_IsArray(values) || cJSON_GetArraySize(values) < 1 ||
                                                          cJSON_GetArraySize(values) > (f == 5 || f == 6 ? 1 : 123))) {
        why = "values: the value(s) to write (16-bit numbers; coils 0/1)";
    }
    if (why) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, why);
    }
    if (!job_begin()) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 503, "Modbus busy");
    }
    memset(&s_job.raw, 0, sizeof(s_job.raw));
    memset(&s_job.raw_tpl, 0, sizeof(s_job.raw_tpl));
    s_job.raw_tpl.timeout_ms = 1000;
    s_job.raw_tpl.gap_ms = 20;
    s_job.raw_t.t = s_job.raw_tpl;
    s_job.raw.t = &s_job.raw_t;
    s_job.raw.tcp = tcp;
    if (tcp) {
        strcpy(s_job.raw.host, host);
    }
    s_job.raw.port = (uint16_t)(cJSON_IsNumber(port) ? port->valueint : 502);
    s_job.raw.slave = (uint8_t)slave->valueint;
    s_job.req = (mb_req_t){.fc = (uint8_t)f, .addr = (uint16_t)addr->valueint, .count = (uint16_t)n};
    int k = 0;
    const cJSON *v;
    static uint8_t bits[16];
    memset(bits, 0, sizeof(bits));
    cJSON_ArrayForEach(v, values)
    {
        int x = cJSON_IsNumber(v) ? v->valueint : 0;
        if (k < 123) {
            s_job.regs[k] = (uint16_t)x;
        }
        if (k < 128 && x) {
            bits[k / 8] |= 1 << (k % 8);
        }
        k++;
    }
    if (f == 5) {
        s_job.req.value = s_job.regs[0] ? 0xff00 : 0;
    } else if (f == 6) {
        s_job.req.value = s_job.regs[0];
    } else if (f == 15 || f == 16) {
        s_job.req.count = (uint16_t)k;
        s_job.req.regs = s_job.regs;
        s_job.req.bits = bits;
    }
    s_job.kind = JOB_RAW;
    cJSON_Delete(body);
    bool done = run_and_wait(8000);
    cJSON *j = cJSON_CreateObject();
    if (!done) {
        xSemaphoreGive(s_job.lock);
        cJSON_Delete(j);
        return gw_http_send_error(req, 503, "Modbus busy (no answer within 8 s)");
    }
    cJSON_AddBoolToObject(j, "ok", s_job.result == MB_OK);
    cJSON_AddNumberToObject(j, "result", s_job.result);
    cJSON_AddStringToObject(j, "error", s_job.err);
    if (s_job.result == MB_OK && f <= 4) {
        cJSON *a = cJSON_AddArrayToObject(j, f <= 2 ? "bits" : "registers");
        if (f <= 2) {
            for (int i = 0; i < n; i++) {
                cJSON_AddItemToArray(a, cJSON_CreateNumber((s_job.data[i / 8] >> (i % 8)) & 1));
            }
        } else {
            for (size_t i = 0; i + 1 < s_job.len + 1 && i / 2 < (size_t)n; i += 2) {
                cJSON_AddItemToArray(a, cJSON_CreateNumber(s_job.data[i] << 8 | s_job.data[i + 1]));
            }
        }
    }
    xSemaphoreGive(s_job.lock);
    return gw_http_send_json(req, j);
}

/* POST: starts a search {"transport", "host", "port", "template", "bauds": [...], "from", "to"} or stops it {"stop": true} */
static esp_err_t probe_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "stop"))) {
        s_probe.stop = true;
        cJSON_Delete(body);
        xTaskNotifyGive(s_task);
        return gw_http_send_json(req, cJSON_CreateObject());
    }
    const char *transport = GW_JSON_STR(body, "transport"), *host = GW_JSON_STR(body, "host"), *tpl = GW_JSON_STR(body, "template");
    const cJSON *from = cJSON_GetObjectItem(body, "from"), *to = cJSON_GetObjectItem(body, "to"), *bauds = cJSON_GetObjectItem(body, "bauds");
    const cJSON *port = cJSON_GetObjectItem(body, "port");
    bool tcp = transport && strcmp(transport, "tcp") == 0;
    int a = cJSON_IsNumber(from) ? from->valueint : 1, b = cJSON_IsNumber(to) ? to->valueint : 247;
    const char *why = NULL;
    if (s_probe.running) {
        why = "a search is running";
    } else if (a < (tcp ? 0 : 1) || b > (tcp ? 255 : 247) || a > b) {
        why = "from, to: 1-247 (TCP 0-255)";
    } else if (tcp && (!host || !host[0] || strlen(host) >= sizeof(s_probe.host))) {
        why = "host required for tcp";
    } else if (tpl && !find_tpl(tpl)) {
        why = "template: unknown";
    } else if (!tcp && bauds && (!cJSON_IsArray(bauds) || cJSON_GetArraySize(bauds) < 1 || cJSON_GetArraySize(bauds) > 8)) {
        why = "bauds: a list of 1-8 rates";
    }
    if (why) {
        cJSON_Delete(body);
        return gw_http_send_error(req, why == NULL ? 400 : s_probe.running ? 409 : 400, why);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_probe, 0, sizeof(s_probe));
    s_probe.tcp = tcp;
    if (tcp) {
        strcpy(s_probe.host, host);
    }
    s_probe.port = (uint16_t)(cJSON_IsNumber(port) ? port->valueint : 502);
    s_probe.t = tpl ? find_tpl(tpl) : NULL;
    const cJSON *v;
    cJSON_ArrayForEach(v, bauds)
    {
        if (cJSON_IsNumber(v) && v->valueint >= 1200 && v->valueint <= 115200) {
            s_probe.bauds[s_probe.nbauds++] = v->valueint;
        }
    }
    if (!s_probe.nbauds) {
        s_probe.bauds[s_probe.nbauds++] = s_probe.t ? s_probe.t->t.baud : s_serial.baud;
    }
    s_probe.from = a, s_probe.to = b, s_probe.next = a;
    s_probe.started = esp_timer_get_time();
    mb_tcp_close(&s_probe_conn);
    mb_tcp_init(&s_probe_conn);
    s_probe.running = true;
    xSemaphoreGive(s_lock);
    cJSON_Delete(body);
    xTaskNotifyGive(s_task);
    return gw_http_send_json(req, cJSON_CreateObject());
}

static esp_err_t probe_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(j, "running", s_probe.running);
    int total = (s_probe.to - s_probe.from + 1) * (s_probe.tcp ? 1 : (s_probe.nbauds ? s_probe.nbauds : 1));
    int done = s_probe.baud_i * (s_probe.to - s_probe.from + 1) + (s_probe.next - s_probe.from);
    cJSON_AddNumberToObject(j, "progress", s_probe.running && total > 0 ? (done * 100) / total : (s_probe.started ? 100 : 0));
    cJSON_AddNumberToObject(j, "baud", s_probe.running && !s_probe.tcp ? s_probe.bauds[s_probe.baud_i] : 0);
    cJSON *found = cJSON_AddArrayToObject(j, "found");
    for (int i = 0; i < s_probe.found_n; i++) {
        cJSON *f = cJSON_CreateObject();
        cJSON_AddNumberToObject(f, "slave", s_probe.found[i].slave);
        if (s_probe.found[i].baud) {
            cJSON_AddNumberToObject(f, "baud", s_probe.found[i].baud);
        }
        cJSON_AddItemToArray(found, f);
    }
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

/* ---------------------------------------------------------------- MQTT */

static void mqtt_command(const char *suffix, const char *payload)
{
    /* modbus/<device>/<key>/set */
    queued_write_t w = {0};
    if (sscanf(suffix, "modbus/%7[^/]/%23[^/]/set", w.dev, w.key) == 2) {
        strlcpy(w.value, payload, sizeof(w.value));
        if (xQueueSend(s_wq, &w, 0) != pdTRUE) {
            ESP_LOGW(TAG, "write queue full, %s %s dropped", w.dev, w.key);
        }
        xTaskNotifyGive(s_task);
    }
}

/* discovery entries sent per device slot, to remove them when a device or template changes */
static struct {
    char dev[8];
    int n;
    char (*keys)[MB_KEY_LEN];
    char (*components)[16];
} s_pub[MAX_DEVICES];

static void mqtt_discovery(void)
{
    typedef struct {
        char id[8], name[NAME_CHARS * 3 + 1];
        const tpl_t *t;
        bool on;
    } snap_t;
    snap_t *snap = ps_calloc(MAX_DEVICES, sizeof(snap_t));
    if (!snap) {
        return;
    }
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_ndev; i++) {
        snap[n].on = s_dev[i].enabled && (s_dev[i].expose & EXPOSE_MQTT);
        strcpy(snap[n].id, s_dev[i].id);
        strcpy(snap[n].name, s_dev[i].name);
        snap[n].t = s_dev[i].t;
        n++;
    }
    xSemaphoreGive(s_lock);

    char oid[48], buf[160], tpl[128];
    const char *base = gw_mqtt_base_topic();
    const char *host = gw_state()->hostname;
    /* what was published before and is gone now */
    for (int s = 0; s < MAX_DEVICES; s++) {
        for (int k = 0; k < s_pub[s].n; k++) {
            bool keep = false;
            for (int i = 0; i < n && !keep; i++) {
                keep = snap[i].on && !strcmp(snap[i].id, s_pub[s].dev) && mb_template_entity(&snap[i].t->t, s_pub[s].keys[k]);
            }
            if (!keep) {
                snprintf(oid, sizeof(oid), "mb_%.7s_%.23s", s_pub[s].dev, s_pub[s].keys[k]);
                gw_mqtt_discovery(GW_MQTT_MODBUS, s_pub[s].components[k], oid, NULL);
            }
        }
        s_pub[s].n = 0;
    }
    for (int i = 0; i < n; i++) {
        if (!snap[i].on) {
            continue;
        }
        const mb_template_t *t = &snap[i].t->t;
        if (!s_pub[i].keys) {
            s_pub[i].keys = ps_calloc(MB_ENTITIES_MAX, MB_KEY_LEN);
            s_pub[i].components = ps_calloc(MB_ENTITIES_MAX, 16);
        }
        if (!s_pub[i].keys || !s_pub[i].components) {
            continue;
        }
        strcpy(s_pub[i].dev, snap[i].id);
        for (int k = 0; k < t->nent; k++) {
            const mb_entity_t *e = &t->ent[k];
            const char *component = e->write == MB_W_NUMBER   ? "number"
                                    : e->write == MB_W_SWITCH ? "switch"
                                    : e->write == MB_W_SELECT ? "select"
                                    : e->write == MB_W_BUTTON ? "button"
                                    : e->type == MB_T_BOOL && !e->nopts ? "binary_sensor"
                                                                         : "sensor";
            snprintf(oid, sizeof(oid), "mb_%.7s_%.23s", snap[i].id, e->key);
            cJSON *cfg = cJSON_CreateObject();
            cJSON_AddStringToObject(cfg, "name", e->name);
            snprintf(buf, sizeof(buf), "%s/modbus/%s", base, snap[i].id);
            if (e->write != MB_W_BUTTON) {
                cJSON_AddStringToObject(cfg, "state_topic", buf);
                if (!strcmp(component, "binary_sensor") || !strcmp(component, "switch")) {
                    snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json.%s else 'OFF' }}", e->key);
                    if (!strcmp(component, "switch")) {
                        snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json.%s == %g or value_json.%s == true else 'OFF' }}", e->key, e->on, e->key);
                    }
                } else {
                    snprintf(tpl, sizeof(tpl), "{{ value_json.%s }}", e->key);
                }
                cJSON_AddStringToObject(cfg, "value_template", tpl);
            }
            if (e->write) {
                snprintf(buf, sizeof(buf), "%s/modbus/%s/%s/set", base, snap[i].id, e->key);
                cJSON_AddStringToObject(cfg, "command_topic", buf);
            }
            if (e->write == MB_W_NUMBER) {
                cJSON_AddNumberToObject(cfg, "min", e->min);
                cJSON_AddNumberToObject(cfg, "max", e->max);
                cJSON_AddNumberToObject(cfg, "step", e->step > 0 ? e->step : pow(10, -e->precision));
                cJSON_AddStringToObject(cfg, "mode", "box");
            }
            if (e->write == MB_W_BUTTON) {
                cJSON_AddStringToObject(cfg, "payload_press", "PRESS");
            }
            if (e->nopts && (e->write == MB_W_SELECT || e->write == MB_W_NONE)) {
                cJSON *opts = cJSON_AddArrayToObject(cfg, "options");
                for (int m = 0; m < e->nopts; m++) {
                    cJSON_AddItemToArray(opts, cJSON_CreateString(e->opts[m].label));
                }
                if (e->write == MB_W_NONE) {
                    cJSON_AddStringToObject(cfg, "device_class", "enum");
                }
            }
            if (e->unit[0] && strcmp(component, "binary_sensor") && strcmp(component, "button") && strcmp(component, "select")) {
                cJSON_AddStringToObject(cfg, "unit_of_measurement", e->unit);
            }
            if (e->device_class[0] && !e->nopts) {
                cJSON_AddStringToObject(cfg, "device_class", e->device_class);
            }
            if (e->state_class[0] && !strcmp(component, "sensor") && !e->nopts) {
                cJSON_AddStringToObject(cfg, "state_class", e->state_class);
            }
            if (!strcmp(component, "sensor") && !e->nopts && e->type != MB_T_STR) {
                cJSON_AddNumberToObject(cfg, "suggested_display_precision", e->precision);
            }
            if (e->category[0]) {
                cJSON_AddStringToObject(cfg, "entity_category", e->category);
            }
            cJSON *avail = cJSON_AddArrayToObject(cfg, "availability");
            cJSON *a = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%s/availability", base);
            cJSON_AddStringToObject(a, "topic", buf);
            cJSON_AddItemToArray(avail, a);
            a = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%s/modbus/%s/availability", base, snap[i].id);
            cJSON_AddStringToObject(a, "topic", buf);
            cJSON_AddItemToArray(avail, a);
            cJSON_AddStringToObject(cfg, "availability_mode", "all");
            cJSON *dev = cJSON_AddObjectToObject(cfg, "device");
            cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
            snprintf(buf, sizeof(buf), "%s_mb_%s", host, snap[i].id);
            cJSON_AddItemToArray(ids, cJSON_CreateString(buf));
            cJSON_AddStringToObject(dev, "name", snap[i].name);
            cJSON_AddStringToObject(dev, "manufacturer", t->manufacturer);
            cJSON_AddStringToObject(dev, "model", t->model);
            gw_mqtt_discovery(GW_MQTT_MODBUS, component, oid, cfg);
            int m = s_pub[i].n++;
            strcpy(s_pub[i].keys[m], e->key);
            strcpy(s_pub[i].components[m], component);
        }
    }
    free(snap);
}

/* ---------------------------------------------------------------- Telegram */

static void bot_modbus(const char *args, bool cz, char *out, size_t len)
{
    size_t used = snprintf(out, len, "<b>Modbus</b>");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_ndev) {
        snprintf(out + used, len - used, cz ? "\nŽádné zařízení (Systém › RS485)." : "\nNo device (System › RS485).");
    }
    for (int i = 0; i < s_ndev && used < len; i++) {
        const mbdev_t *d = &s_dev[i];
        used += snprintf(out + used, len - used, "\n<b>%s</b>: %s", d->name,
                         !d->enabled ? (cz ? "vypnuto" : "off") : d->online ? "" : (cz ? "nedostupné" : "offline"));
        /* the measurements (not the configuration or diagnostics), at most 6 */
        for (int k = 0, shown = 0; d->online && k < d->nent && k < d->t->t.nent && shown < 6 && used < len; k++) {
            const mb_entity_t *e = &d->t->t.ent[k];
            if (e->category[0] || e->write || d->vstate[k] != VAL_OK || e->type == MB_T_STR) {
                continue;
            }
            const char *label = e->nopts ? mb_entity_label(e, d->val[k]) : NULL;
            const char *name = cz && e->name_cs[0] ? e->name_cs : e->name;
            if (label) {
                used += snprintf(out + used, len - used, "\n  %s: %s", name, label);
            } else {
                used += snprintf(out + used, len - used, "\n  %s: %.*f %s", name, e->precision, d->val[k], e->unit);
            }
            shown++;
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- start */

esp_err_t modbus_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_job.lock = xSemaphoreCreateMutex();
    s_job.done = xSemaphoreCreateBinary();
    s_wq = xQueueCreate(8, sizeof(queued_write_t));
    s_tpl = ps_calloc(MAX_TPL, sizeof(tpl_t));
    s_dev = ps_calloc(MAX_DEVICES, sizeof(mbdev_t));
    if (!s_lock || !s_job.lock || !s_job.done || !s_wq || !s_tpl || !s_dev) {
        return ESP_ERR_NO_MEM;
    }
    load_templates();
    gw_psram_note("modbus", MAX_TPL * sizeof(tpl_t) + MAX_DEVICES * sizeof(mbdev_t));
    char *text = heap_caps_malloc(4000, MALLOC_CAP_SPIRAM);
    cJSON *cfg = text && gw_config_get_str(KEY_CFG, text, 4000) ? cJSON_Parse(text) : NULL;
    free(text);
    if (cfg) {
        apply_config(cfg);
        cJSON_Delete(cfg);
    }
    if (!mb_rtu_ready()) {
        mb_rtu_open(&s_serial);
    }
    gw_http_register("/api/modbus", HTTP_GET, modbus_get);
    gw_http_register("/api/modbus", HTTP_PUT, modbus_put);
    gw_http_register("/api/modbus/templates", HTTP_GET, templates_get);
    gw_http_register("/api/modbus/template", HTTP_GET, template_get);
    gw_http_register("/api/modbus/template", HTTP_PUT, template_put);
    gw_http_register("/api/modbus/template", HTTP_DELETE, template_delete);
    gw_http_register("/api/modbus/write", HTTP_POST, write_post);
    gw_http_register("/api/modbus/raw", HTTP_POST, raw_post);
    gw_http_register("/api/modbus/probe", HTTP_GET, probe_get);
    gw_http_register("/api/modbus/probe", HTTP_POST, probe_post);
    gw_bot_command("modbus", "values of the Modbus devices", "hodnoty zařízení Modbus", "system/rs485", 1, false, bot_modbus);
    gw_mqtt_set_extension(mqtt_discovery, NULL);
    gw_mqtt_add_command("modbus/+/+/set", mqtt_command);
    ESP_LOGI(TAG, "%d devices", s_ndev);
    return xTaskCreate(task, "modbus", 6144, NULL, 3, &s_task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
