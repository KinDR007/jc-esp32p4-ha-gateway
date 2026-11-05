#include "sensors.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "gw_i2c.h"
#include "sensor_drv.h"

/*
 * I2C sensors on header JP1 (docs/sensors.md). The bus is shared with the ES8311 codec, which the
 * sound component only talks to at its start; every transfer here goes through the driver's bus lock.
 *
 * A scan probes every address, identifies new chips (sensor_drv.c) and adds those it recognised
 * without doubt; the rest wait in the scan result for the user to pick a type. One task reads the
 * configured sensors; values reach /api/sensors, the MQTT state and the integration.
 */

static const char *TAG = "sensors";

#define KEY_CFG "i2c_sens" /* str JSON {"interval": s, "sensors": [{addr, type, chip, name, expose, enabled}]} */
#define MAX_SENSORS 16
#define MAX_FOUND 32
#define MAX_CANDS 4
#define SENSOR_NAME_MAX 32 /* characters (UTF-8) */
#define BUS_HZ 100000
#define XFER_MS 100
#define DEFAULT_INTERVAL_S 30
#define FAILS_TO_RESTART 3 /* failed reads in a row: init again after RETRY_S */
#define RETRY_S 60

typedef struct {
    uint8_t bus; /* 0 the board's bus, 1 the second bus on JP1 (gpio_ctl) */
    uint8_t addr;
    const sdrv_t *drv;
    char chip[16]; /* the exact chip found (BME688 for the bme680 driver) */
    char name[SENSOR_NAME_MAX * 3 + 1];
    bool expose, enabled;
    /* runtime */
    i2c_master_dev_handle_t dev;
    void *state;
    bool ready;
    int64_t retry_us, next_us, last_us;
    sval_t val;
    char error[64];
    uint32_t reads, errors;
    uint8_t fails;
} sensor_t;

typedef struct {
    uint8_t bus;
    uint8_t addr;
    uint8_t ncand;
    sdrv_cand_t cand[MAX_CANDS];
} found_t;

static sensor_t *s_sens; /* PSRAM, MAX_SENSORS */
static int s_count;
static int s_interval = DEFAULT_INTERVAL_S;
static found_t *s_found; /* last scan, PSRAM, MAX_FOUND */
static int s_nfound = -1; /* -1: no scan yet */
static int64_t s_scan_time, s_scan_uptime_us;
static SemaphoreHandle_t s_lock;
static uint32_t s_published[2][128]; /* per bus and address: quantities with an MQTT discovery config out */
static i2c_master_bus_handle_t s_bus2; /* second bus on free pins of JP1, NULL while not set up */
static int s_bus2_sda = -1, s_bus2_scl = -1;
static uint32_t s_bus2_hz = BUS_HZ;

static i2c_master_bus_handle_t bus_handle(int bus)
{
    return bus ? s_bus2 : gw_i2c_bus();
}

/* ---------------------------------------------------------------- bus */

static int bus_xfer(void *ctx, uint8_t addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    i2c_master_dev_handle_t dev = ctx;
    esp_err_t err;
    if (wl && rl) {
        err = i2c_master_transmit_receive(dev, w, wl, r, rl, XFER_MS);
    } else if (wl) {
        err = i2c_master_transmit(dev, w, wl, XFER_MS);
    } else {
        err = i2c_master_receive(dev, r, rl, XFER_MS);
    }
    return err == ESP_OK ? 0 : -1;
}

static void bus_delay(void *ctx, int ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static i2c_master_dev_handle_t dev_add(int bus, uint8_t addr)
{
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = bus ? s_bus2_hz : BUS_HZ,
    };
    i2c_master_dev_handle_t dev = NULL;
    if (!bus_handle(bus) || i2c_master_bus_add_device(bus_handle(bus), &cfg, &dev) != ESP_OK) {
        return NULL;
    }
    return dev;
}

static sbus_t sbus(i2c_master_dev_handle_t dev, uint8_t addr)
{
    return (sbus_t){.ctx = dev, .addr = addr, .xfer = bus_xfer, .delay_ms = bus_delay};
}

/* ---------------------------------------------------------------- configuration */

static void release(sensor_t *s)
{
    if (s->dev) {
        i2c_master_bus_rm_device(s->dev);
    }
    free(s->state);
    s->dev = NULL;
    s->state = NULL;
    s->ready = false;
}

static sensor_t *find(int bus, uint8_t addr)
{
    for (int i = 0; i < s_count; i++) {
        if (s_sens[i].bus == bus && s_sens[i].addr == addr) {
            return &s_sens[i];
        }
    }
    return NULL;
}

static void save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "interval", s_interval);
    cJSON *list = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; i < s_count; i++) {
        sensor_t *s = &s_sens[i];
        cJSON *j = cJSON_CreateObject();
        if (s->bus) {
            cJSON_AddNumberToObject(j, "bus", 2);
        }
        cJSON_AddNumberToObject(j, "addr", s->addr);
        cJSON_AddStringToObject(j, "type", s->drv->id);
        cJSON_AddStringToObject(j, "chip", s->chip);
        cJSON_AddStringToObject(j, "name", s->name);
        cJSON_AddBoolToObject(j, "expose", s->expose);
        cJSON_AddBoolToObject(j, "enabled", s->enabled);
        cJSON_AddItemToArray(list, j);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        gw_config_set_str(KEY_CFG, text);
        cJSON_free(text);
    }
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xc0) != 0x80;
    }
    return n;
}

/* One configured sensor from JSON, checked; NULL when fine, else why not. Unknown fields stay out. */
static const char *parse_sensor(const cJSON *j, sensor_t *out)
{
    const cJSON *addr = cJSON_GetObjectItem(j, "addr");
    const cJSON *bus = cJSON_GetObjectItem(j, "bus");
    const char *type = GW_JSON_STR(j, "type");
    const char *name = GW_JSON_STR(j, "name");
    const char *chip = GW_JSON_STR(j, "chip");
    const cJSON *expose = cJSON_GetObjectItem(j, "expose");
    const cJSON *enabled = cJSON_GetObjectItem(j, "enabled");
    if (!cJSON_IsNumber(addr) || addr->valueint < 0x08 || addr->valueint > 0x77) {
        return "addr: 7-bit address 0x08-0x77";
    }
    if (bus && (!cJSON_IsNumber(bus) || (bus->valueint != 1 && bus->valueint != 2))) {
        return "bus: 1 (the board's) or 2 (on JP1, System › GPIO)";
    }
    if (addr->valueint == GW_I2C_CODEC_ADDR && !(bus && bus->valueint == 2)) {
        return "addr 0x18 is the audio codec of the board";
    }
    const sdrv_t *drv = type ? sdrv_find(type) : NULL;
    if (!drv) {
        return "type: unknown sensor type";
    }
    if (!sdrv_has_addr(drv, (uint8_t)addr->valueint)) {
        return "addr: the sensor type cannot have this address";
    }
    if (name && (utf8_chars(name) > SENSOR_NAME_MAX || strlen(name) >= sizeof(out->name) || strpbrk(name, "\"\\{}"))) {
        return "name: up to 32 characters, no quotes, braces or backslash";
    }
    if ((expose && !cJSON_IsBool(expose)) || (enabled && !cJSON_IsBool(enabled))) {
        return "expose, enabled: true or false";
    }
    memset(out, 0, sizeof(*out));
    out->bus = bus && bus->valueint == 2;
    out->addr = (uint8_t)addr->valueint;
    out->drv = drv;
    snprintf(out->chip, sizeof(out->chip), "%s", chip && chip[0] && strlen(chip) < sizeof(out->chip) ? chip : drv->label);
    snprintf(out->name, sizeof(out->name), "%s", name && name[0] ? name : out->chip);
    out->expose = !expose || cJSON_IsTrue(expose);
    out->enabled = !enabled || cJSON_IsTrue(enabled);
    return NULL;
}

/* Takes the new list (checked): sensors with the same address and type keep their runtime. */
static void apply(sensor_t *list, int n)
{
    for (int i = 0; i < s_count; i++) {
        sensor_t *old = &s_sens[i];
        bool kept = false;
        for (int k = 0; k < n; k++) {
            sensor_t *nw = &list[k];
            if (nw->bus == old->bus && nw->addr == old->addr && nw->drv == old->drv && old->enabled && nw->enabled) {
                nw->dev = old->dev, nw->state = old->state, nw->ready = old->ready;
                nw->retry_us = old->retry_us, nw->next_us = old->next_us, nw->last_us = old->last_us;
                nw->val = old->val, nw->reads = old->reads, nw->errors = old->errors, nw->fails = old->fails;
                memcpy(nw->error, old->error, sizeof(nw->error));
                kept = true;
            }
        }
        if (!kept) {
            release(old);
        }
    }
    memcpy(s_sens, list, n * sizeof(sensor_t));
    s_count = n;
}

static void load(void)
{
    char *text = heap_caps_malloc(4000, MALLOC_CAP_SPIRAM);
    cJSON *root = text && gw_config_get_str(KEY_CFG, text, 4000) ? cJSON_Parse(text) : NULL;
    free(text);
    if (!root) {
        return;
    }
    const cJSON *interval = cJSON_GetObjectItem(root, "interval");
    if (cJSON_IsNumber(interval) && interval->valueint >= 5 && interval->valueint <= 3600) {
        s_interval = interval->valueint;
    }
    const cJSON *j;
    cJSON_ArrayForEach(j, cJSON_GetObjectItem(root, "sensors"))
    {
        if (s_count < MAX_SENSORS && !parse_sensor(j, &s_sens[s_count]) && !find(s_sens[s_count].bus, s_sens[s_count].addr)) {
            s_count++;
        }
    }
    cJSON_Delete(root);
}

/* ---------------------------------------------------------------- reading */

static bool fresh(const sensor_t *s, int64_t now)
{
    int period = s->drv->read_s ? s->drv->read_s : s_interval;
    return s->enabled && s->val.have && now - s->last_us < (int64_t)(3 * (period > s_interval ? period : s_interval) + 10) * 1000000;
}

static void fail(sensor_t *s, int64_t now, const char *what, int err)
{
    s->errors++;
    snprintf(s->error, sizeof(s->error), "%s: %s", what, err == SDRV_EDATA ? "invalid data (CRC or range)" : "no answer");
    if (++s->fails >= FAILS_TO_RESTART || !s->ready) {
        ESP_LOGW(TAG, "%s0x%02x %s: %s, again in %d s", s->bus ? "bus 2 " : "", s->addr, s->chip, s->error, RETRY_S);
        s->ready = false;
        s->val.have = 0;
        s->retry_us = now + (int64_t)RETRY_S * 1000000;
    }
}

static void service(sensor_t *s, int64_t now)
{
    if (!s->enabled) {
        return;
    }
    if (!s->ready) {
        if (now < s->retry_us) {
            return;
        }
        if (!s->dev && !(s->dev = dev_add(s->bus, s->addr))) {
            fail(s, now, "bus", SDRV_EBUS);
            return;
        }
        free(s->state);
        s->state = s->drv->state_size ? heap_caps_calloc(1, s->drv->state_size, MALLOC_CAP_SPIRAM) : NULL;
        sbus_t b = sbus(s->dev, s->addr);
        int err = s->drv->init(&b, s->state);
        if (err != SDRV_OK) {
            fail(s, now, "start", err);
            return;
        }
        s->ready = true;
        s->fails = 0;
        s->next_us = now;
        ESP_LOGI(TAG, "%s0x%02x %s started", s->bus ? "bus 2 " : "", s->addr, s->chip);
    }
    if (now < s->next_us) {
        return;
    }
    int period = s->drv->read_s ? s->drv->read_s : s_interval;
    s->next_us = now + (int64_t)period * 1000000;
    sbus_t b = sbus(s->dev, s->addr);
    sval_t v = {0};
    int err = s->drv->read(&b, s->state, &v);
    if (err == SDRV_BUSY) {
        return;
    }
    if (err != SDRV_OK) {
        fail(s, now, "read", err);
        return;
    }
    /* merge: a chip may deliver its quantities in turns (LTR390) */
    for (int q = 0; q < SQ_COUNT; q++) {
        if (v.have & (1u << q)) {
            s->val.v[q] = v.v[q];
        }
    }
    s->val.have |= v.have;
    s->last_us = now;
    s->reads++;
    s->fails = 0;
    s->error[0] = '\0';
}

static void task(void *arg)
{
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        for (int i = 0;; i++) {
            /* one sensor per lock: the API waits for at most one read */
            xSemaphoreTake(s_lock, portMAX_DELAY);
            if (i >= s_count) {
                xSemaphoreGive(s_lock);
                break;
            }
            service(&s_sens[i], esp_timer_get_time());
            xSemaphoreGive(s_lock);
        }
    }
}

/* ---------------------------------------------------------------- scan */

/* Probes the buses, identifies the chips not configured yet and adds those recognised without doubt.
 * Under s_lock. Returns how many were added (their addresses in added). */
static int scan(uint8_t *added, int max_added)
{
    int n_added = 0;
    s_nfound = 0;
    s_scan_uptime_us = esp_timer_get_time();
    time_t t = time(NULL);
    s_scan_time = t > 1700000000 ? (int64_t)t : 0;
    /* identification reads registers of chips that may not have them: NACKs are expected */
    esp_log_level_t level = esp_log_level_get("i2c.master");
    esp_log_level_set("i2c.master", ESP_LOG_NONE);
    for (int bus = 0; bus < 2; bus++) {
        for (uint8_t addr = 0x08; bus_handle(bus) && addr <= 0x77 && s_nfound < MAX_FOUND; addr++) {
            if (i2c_master_probe(bus_handle(bus), addr, 20) != ESP_OK) {
                continue;
            }
            found_t *f = &s_found[s_nfound++];
            memset(f, 0, sizeof(*f));
            f->bus = bus;
            f->addr = addr;
            if ((!bus && addr == GW_I2C_CODEC_ADDR) || find(bus, addr)) {
                continue; /* the codec, or a sensor already running: not disturbed */
            }
            i2c_master_dev_handle_t dev = dev_add(bus, addr);
            if (!dev) {
                continue;
            }
            sbus_t b = sbus(dev, addr);
            f->ncand = (uint8_t)sdrv_identify_all(&b, f->cand, MAX_CANDS);
            i2c_master_bus_rm_device(dev);
            const sdrv_cand_t *pick = sdrv_pick(f->cand, f->ncand);
            if (pick && s_count < MAX_SENSORS) {
                sensor_t *s = &s_sens[s_count++];
                memset(s, 0, sizeof(*s));
                s->bus = bus;
                s->addr = addr;
                s->drv = pick->drv;
                snprintf(s->chip, sizeof(s->chip), "%s", pick->label);
                snprintf(s->name, sizeof(s->name), "%s", pick->label);
                s->expose = true;
                s->enabled = true;
                if (n_added < max_added) {
                    added[n_added++] = addr;
                }
            }
        }
    }
    esp_log_level_set("i2c.master", level);
    ESP_LOGI(TAG, "scan: %d devices, %d sensors added", s_nfound, n_added);
    if (n_added) {
        save();
        gw_mqtt_rediscover();
    }
    return n_added;
}

/* ---------------------------------------------------------------- JSON */

static double rounded(float v, int precision)
{
    double f = pow(10, precision);
    return round((double)v * f) / f;
}

static void add_values(cJSON *obj, const sensor_t *s)
{
    for (int q = 0; q < SQ_COUNT; q++) {
        if (s->val.have & (1u << q)) {
            cJSON_AddNumberToObject(obj, sdrv_quantity[q].key, rounded(s->val.v[q], sdrv_quantity[q].precision));
        }
    }
}

static const char *state_name(const sensor_t *s, int64_t now)
{
    return !s->enabled ? "off" : !s->ready && s->error[0] ? "error" : !s->ready || !s->val.have ? "starting"
                                                                                                  : fresh(s, now) ? "ok" : "stale";
}

static cJSON *quantities_json(uint32_t mask)
{
    cJSON *a = cJSON_CreateArray();
    for (int q = 0; q < SQ_COUNT; q++) {
        if (mask & (1u << q)) {
            cJSON_AddItemToArray(a, cJSON_CreateString(sdrv_quantity[q].key));
        }
    }
    return a;
}

static cJSON *status_json(void)
{
    int64_t now = esp_timer_get_time();
    cJSON *root = cJSON_CreateObject();
    cJSON *bus = cJSON_AddObjectToObject(root, "bus");
    cJSON_AddBoolToObject(bus, "ok", gw_i2c_bus() != NULL);
    cJSON_AddNumberToObject(bus, "sda", GW_I2C_SDA);
    cJSON_AddNumberToObject(bus, "scl", GW_I2C_SCL);
    cJSON_AddNumberToObject(bus, "khz", BUS_HZ / 1000);
    if (s_bus2) {
        cJSON *b2 = cJSON_AddObjectToObject(root, "bus2");
        cJSON_AddNumberToObject(b2, "sda", s_bus2_sda);
        cJSON_AddNumberToObject(b2, "scl", s_bus2_scl);
        cJSON_AddNumberToObject(b2, "khz", s_bus2_hz / 1000);
    }
    cJSON_AddNumberToObject(root, "interval_s", s_interval);
    cJSON_AddNumberToObject(root, "max_sensors", MAX_SENSORS);

    cJSON *list = cJSON_AddArrayToObject(root, "sensors");
    for (int i = 0; i < s_count; i++) {
        const sensor_t *s = &s_sens[i];
        cJSON *j = cJSON_CreateObject();
        char id[12];
        snprintf(id, sizeof(id), s->bus ? "i2c2_%02x" : "i2c_%02x", s->addr);
        cJSON_AddStringToObject(j, "id", id);
        cJSON_AddNumberToObject(j, "bus", s->bus + 1);
        cJSON_AddNumberToObject(j, "addr", s->addr);
        cJSON_AddStringToObject(j, "type", s->drv->id);
        cJSON_AddStringToObject(j, "chip", s->chip);
        cJSON_AddStringToObject(j, "name", s->name);
        cJSON_AddBoolToObject(j, "expose", s->expose);
        cJSON_AddBoolToObject(j, "enabled", s->enabled);
        cJSON_AddStringToObject(j, "state", state_name(s, now));
        cJSON_AddStringToObject(j, "error", s->error);
        cJSON_AddItemToObject(j, "quantities", quantities_json(s->drv->quantities));
        cJSON *values = cJSON_AddObjectToObject(j, "values");
        if (fresh(s, now)) {
            add_values(values, s);
            cJSON_AddNumberToObject(j, "age_s", round((double)(now - s->last_us) / 100000.0) / 10.0);
        }
        cJSON_AddNumberToObject(j, "reads", s->reads);
        cJSON_AddNumberToObject(j, "errors", s->errors);
        cJSON_AddItemToArray(list, j);
    }

    cJSON *scan = cJSON_AddObjectToObject(root, "scan");
    cJSON_AddBoolToObject(scan, "done", s_nfound >= 0);
    cJSON_AddNumberToObject(scan, "time", (double)s_scan_time);
    cJSON_AddNumberToObject(scan, "ago_s", s_nfound >= 0 ? (double)((now - s_scan_uptime_us) / 1000000) : -1);
    cJSON *devices = cJSON_AddArrayToObject(scan, "devices");
    for (int i = 0; i < s_nfound; i++) {
        const found_t *f = &s_found[i];
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNumberToObject(d, "bus", f->bus + 1);
        cJSON_AddNumberToObject(d, "addr", f->addr);
        cJSON_AddBoolToObject(d, "onboard", !f->bus && f->addr == GW_I2C_CODEC_ADDR);
        cJSON_AddBoolToObject(d, "configured", find(f->bus, f->addr) != NULL);
        const char *hint = sdrv_hint(f->addr);
        cJSON_AddStringToObject(d, "hint", hint ? hint : "");
        cJSON *cands = cJSON_AddArrayToObject(d, "candidates");
        for (int k = 0; k < f->ncand; k++) {
            cJSON *c = cJSON_CreateObject();
            cJSON_AddStringToObject(c, "type", f->cand[k].drv->id);
            cJSON_AddStringToObject(c, "chip", f->cand[k].label);
            cJSON_AddBoolToObject(c, "sure", f->cand[k].score == SDRV_SURE);
            cJSON_AddItemToArray(cands, c);
        }
        cJSON_AddItemToArray(devices, d);
    }

    cJSON *types = cJSON_AddArrayToObject(root, "types");
    for (size_t i = 0; i < sdrv_count; i++) {
        const sdrv_t *d = sdrv_list[i];
        cJSON *t = cJSON_CreateObject();
        cJSON_AddStringToObject(t, "type", d->id);
        cJSON_AddStringToObject(t, "label", d->label);
        cJSON_AddStringToObject(t, "vendor", d->vendor);
        cJSON_AddStringToObject(t, "note", d->note ? d->note : "");
        cJSON *addrs = cJSON_AddArrayToObject(t, "addrs");
        for (size_t k = 0; k < sizeof(d->addrs) && d->addrs[k]; k++) {
            cJSON_AddItemToArray(addrs, cJSON_CreateNumber(d->addrs[k]));
        }
        cJSON_AddItemToObject(t, "quantities", quantities_json(d->quantities));
        cJSON_AddItemToArray(types, t);
    }
    cJSON *qs = cJSON_AddArrayToObject(root, "quantities");
    for (int q = 0; q < SQ_COUNT; q++) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "key", sdrv_quantity[q].key);
        cJSON_AddStringToObject(j, "name", sdrv_quantity[q].name);
        if (sdrv_quantity[q].unit) {
            cJSON_AddStringToObject(j, "unit", sdrv_quantity[q].unit);
        }
        if (sdrv_quantity[q].device_class) {
            cJSON_AddStringToObject(j, "device_class", sdrv_quantity[q].device_class);
        }
        cJSON_AddNumberToObject(j, "precision", sdrv_quantity[q].precision);
        cJSON_AddItemToArray(qs, j);
    }
    return root;
}

/* ---------------------------------------------------------------- HTTP */

static esp_err_t sensors_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *json = status_json();
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, json);
}

/* {"interval_s": n, "sensors": [{addr, type, name, expose, enabled, chip}]}: both optional; sensors
 * replaces the whole list */
static esp_err_t sensors_put(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char err[96] = "";
    const cJSON *interval = cJSON_GetObjectItem(body, "interval_s");
    const cJSON *list = cJSON_GetObjectItem(body, "sensors");
    sensor_t *parsed = NULL;
    int n = 0;
    if (interval && (!cJSON_IsNumber(interval) || interval->valueint < 5 || interval->valueint > 3600)) {
        strlcpy(err, "interval_s: 5-3600", sizeof(err));
    } else if (list && !cJSON_IsArray(list)) {
        strlcpy(err, "sensors: list", sizeof(err));
    } else if (list && cJSON_GetArraySize(list) > MAX_SENSORS) {
        snprintf(err, sizeof(err), "sensors: at most %d", MAX_SENSORS);
    } else if (list) {
        parsed = heap_caps_calloc(MAX_SENSORS, sizeof(sensor_t), MALLOC_CAP_SPIRAM);
        const cJSON *j;
        cJSON_ArrayForEach(j, list)
        {
            const char *why = parsed ? parse_sensor(j, &parsed[n]) : "out of memory";
            for (int k = 0; !why && k < n; k++) {
                if (parsed[k].bus == parsed[n].bus && parsed[k].addr == parsed[n].addr) {
                    why = "addr: used twice on the bus";
                }
            }
            if (why) {
                snprintf(err, sizeof(err), "sensors[%d] %s", n, why);
                break;
            }
            n++;
        }
    }
    cJSON_Delete(body);
    if (err[0]) {
        free(parsed);
        return gw_http_send_error(req, 400, err);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (interval) {
        s_interval = interval->valueint;
        for (int i = 0; i < s_count; i++) {
            s_sens[i].next_us = 0; /* the new interval from now on */
        }
    }
    if (parsed) {
        apply(parsed, n);
    }
    save();
    xSemaphoreGive(s_lock);
    free(parsed);
    gw_mqtt_rediscover();
    return sensors_get(req);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    uint8_t added[MAX_SENSORS];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = scan(added, MAX_SENSORS);
    cJSON *json = status_json();
    xSemaphoreGive(s_lock);
    cJSON *a = cJSON_AddArrayToObject(json, "added");
    for (int i = 0; i < n; i++) {
        cJSON_AddItemToArray(a, cJSON_CreateNumber(added[i]));
    }
    return gw_http_send_json(req, json);
}

/* ---------------------------------------------------------------- MQTT */

static void object_id(char *out, size_t len, int bus, uint8_t addr, int q)
{
    snprintf(out, len, bus ? "i2c2_%02x_%s" : "i2c_%02x_%s", addr, sdrv_quantity[q].key);
}

/* Runs in the MQTT task (connect) and in the publish task (gw_mqtt_rediscover), possibly both at once:
 * s_lock is never held while publishing, the client's own lock would deadlock with it. */
static void mqtt_discovery(void)
{
    typedef struct {
        uint8_t bus, addr;
        uint32_t quantities;
        char name[SENSOR_NAME_MAX * 3 + 1];
    } entry_t;
    entry_t *list = heap_caps_calloc(MAX_SENSORS, sizeof(entry_t), MALLOC_CAP_SPIRAM);
    uint32_t (*gone)[128] = heap_caps_calloc(2, sizeof(*gone), MALLOC_CAP_SPIRAM);
    char oid[40], buf[160], tpl[96];
    int n = 0;
    if (!list || !gone) {
        free(list);
        free(gone);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const sensor_t *s = &s_sens[i];
        if (s->expose && s->enabled) {
            list[n].bus = s->bus;
            list[n].addr = s->addr;
            list[n].quantities = s->drv->quantities;
            memcpy(list[n].name, s->name, sizeof(list[n].name));
            gone[s->bus][s->addr] = s->drv->quantities; /* wanted, for the moment */
            n++;
        }
    }
    for (int bus = 0; bus < 2; bus++) {
        for (int addr = 0; addr < 128; addr++) {
            uint32_t want = gone[bus][addr];
            gone[bus][addr] = s_published[bus][addr] & ~want;
            s_published[bus][addr] = want;
        }
    }
    xSemaphoreGive(s_lock);

    for (int bus = 0; bus < 2; bus++) {
        for (int addr = 0; addr < 128; addr++) {
            for (int q = 0; q < SQ_COUNT; q++) {
                if (gone[bus][addr] & (1u << q)) {
                    object_id(oid, sizeof(oid), bus, addr, q);
                    gw_mqtt_discovery(GW_MQTT_SENSORS, "sensor", oid, NULL); /* removed */
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        for (int q = 0; q < SQ_COUNT; q++) {
            if (!(list[i].quantities & (1u << q))) {
                continue;
            }
            const sdrv_quantity_t *qd = &sdrv_quantity[q];
            object_id(oid, sizeof(oid), list[i].bus, list[i].addr, q);
            cJSON *cfg = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%s %s", list[i].name, qd->name);
            cJSON_AddStringToObject(cfg, "name", buf);
            snprintf(buf, sizeof(buf), "%s/state", gw_mqtt_base_topic());
            cJSON_AddStringToObject(cfg, "state_topic", buf);
            snprintf(tpl, sizeof(tpl), "{{ value_json.%s }}", oid);
            cJSON_AddStringToObject(cfg, "value_template", tpl);
            /* unavailable while the gateway is offline or the sensor gives no value */
            cJSON *avail = cJSON_AddArrayToObject(cfg, "availability");
            cJSON *a = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%s/availability", gw_mqtt_base_topic());
            cJSON_AddStringToObject(a, "topic", buf);
            cJSON_AddItemToArray(avail, a);
            a = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "%s/state", gw_mqtt_base_topic());
            cJSON_AddStringToObject(a, "topic", buf);
            snprintf(tpl, sizeof(tpl), "{{ 'online' if value_json.%s is defined else 'offline' }}", oid);
            cJSON_AddStringToObject(a, "value_template", tpl);
            cJSON_AddItemToArray(avail, a);
            cJSON_AddStringToObject(cfg, "availability_mode", "all");
            if (qd->unit) {
                cJSON_AddStringToObject(cfg, "unit_of_measurement", qd->unit);
            }
            if (qd->device_class) {
                cJSON_AddStringToObject(cfg, "device_class", qd->device_class);
            }
            cJSON_AddStringToObject(cfg, "state_class", "measurement");
            cJSON_AddNumberToObject(cfg, "suggested_display_precision", qd->precision);
            gw_mqtt_discovery(GW_MQTT_SENSORS, "sensor", oid, cfg);
        }
    }
    free(list);
    free(gone);
}

static void mqtt_state(cJSON *state)
{
    char oid[40];
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        const sensor_t *s = &s_sens[i];
        if (!s->expose || !fresh(s, now)) {
            continue;
        }
        for (int q = 0; q < SQ_COUNT; q++) {
            if (s->val.have & (1u << q)) {
                object_id(oid, sizeof(oid), s->bus, s->addr, q);
                cJSON_AddNumberToObject(state, oid, rounded(s->val.v[q], sdrv_quantity[q].precision));
            }
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- Telegram */

static void bot_sensors(const char *args, bool cz, char *out, size_t len)
{
    int64_t now = esp_timer_get_time();
    size_t used = snprintf(out, len, cz ? "<b>Senzory</b>" : "<b>Sensors</b>");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_count) {
        snprintf(out + used, len - used, cz ? "\nŽádný senzor (Systém › Hardware)." : "\nNo sensor (System › Hardware).");
    }
    for (int i = 0; i < s_count && used < len; i++) {
        const sensor_t *s = &s_sens[i];
        used += snprintf(out + used, len - used, "\n%s:", s->name);
        if (!fresh(s, now)) {
            used += snprintf(out + used, len - used, " %s", cz ? "bez hodnoty" : "no value");
            continue;
        }
        for (int q = 0; q < SQ_COUNT && used < len; q++) {
            if (s->val.have & (1u << q)) {
                const sdrv_quantity_t *qd = &sdrv_quantity[q];
                used += snprintf(out + used, len - used, " %.*f%s%s", qd->precision, rounded(s->val.v[q], qd->precision),
                                 qd->unit && strcmp(qd->unit, "%") ? " " : "", qd->unit ? qd->unit : " AQI");
            }
        }
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- second bus */

void sensors_set_bus2(i2c_master_bus_handle_t bus, int sda, int scl, uint32_t hz)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    /* the devices of the old bus go first: the bus is deleted after this */
    for (int i = 0; i < s_count; i++) {
        if (s_sens[i].bus) {
            release(&s_sens[i]);
            s_sens[i].retry_us = 0;
        }
    }
    s_bus2 = bus;
    s_bus2_sda = sda;
    s_bus2_scl = scl;
    s_bus2_hz = hz ? hz : BUS_HZ;
    uint8_t added[MAX_SENSORS];
    if (bus) {
        scan(added, MAX_SENSORS);
    }
    xSemaphoreGive(s_lock);
    if (bus) {
        ESP_LOGI(TAG, "second bus on SDA GPIO%d, SCL GPIO%d", sda, scl);
    } else {
        ESP_LOGI(TAG, "second bus gone");
    }
}

/* ---------------------------------------------------------------- start */

esp_err_t sensors_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_sens = heap_caps_calloc(MAX_SENSORS, sizeof(sensor_t), MALLOC_CAP_SPIRAM);
    s_found = heap_caps_calloc(MAX_FOUND, sizeof(found_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_sens || !s_found) {
        return ESP_ERR_NO_MEM;
    }
    gw_psram_note("sensors", MAX_SENSORS * sizeof(sensor_t) + MAX_FOUND * sizeof(found_t));
    load();
    gw_http_register("/api/sensors", HTTP_GET, sensors_get);
    gw_http_register("/api/sensors", HTTP_PUT, sensors_put);
    gw_http_register("/api/sensors/scan", HTTP_POST, scan_post);
    gw_bot_command("sensors", "values of the I2C sensors", "hodnoty I2C senzorů", "system/hw", 1, false, bot_sensors);
    gw_mqtt_set_extension(mqtt_discovery, mqtt_state);
    if (!gw_i2c_bus()) {
        ESP_LOGE(TAG, "no I2C bus, sensors off");
        return ESP_OK;
    }
    /* a scan at every start: sensors connected while the gateway was off are added */
    uint8_t added[MAX_SENSORS];
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan(added, MAX_SENSORS);
    xSemaphoreGive(s_lock);
    return xTaskCreate(task, "sensors", 4096, NULL, 2, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
