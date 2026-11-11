#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "gw_auth_priv.h"

static const char *TAG = "gw_report";

/* Periodic Telegram reports (docs/notifications.md). Each user sets up to MAX_REPORTS, each with devices of
 * the report sources (Modbus devices, I2C sensors, GPIO pins: their components register) and a period
 * aligned to the clock. A task samples the devices of the active reports every SAMPLE_S: minimum, maximum
 * and average of measured values, and the counters at the start of the period (kept in NVS, so their
 * difference survives a restart). At the end of the period the report goes to the user's chat. */
#define MAX_SOURCES 4
#define MAX_USERS 16
#define MAX_REPORTS 4  /* per user */
#define MAX_DEVS 8     /* per report */
#define MAX_VALUES 64  /* per device */
#define MAX_STATS 256  /* per report */
#define SAMPLE_S 60
#define PERIOD_MIN 15
#define PERIOD_MAX 10080
#define NAME_CHARS 40
#define KEY_CFG "rep%d"     /* str JSON {"user": name, "reports": [...]} of user n */
#define KEY_BASE "repb%d_%d" /* blob base_head_t + base_t[], counters at the start of the period */
#define TEXT_MAX 16384
#define PART_MAX 4000       /* Telegram takes 4096 characters */

typedef struct {
    char id[12];
    char name_en[24], name_cs[24];
    char page[24];
    int (*devices)(gw_rdev_t *out, int max);
    int (*values)(const char *dev, bool czech, gw_rv_t *out, int max);
} source_t;

typedef struct {
    uint32_t h; /* of "<device>/<key>" */
    uint8_t kind;
    bool has_base;
    uint32_t n;
    double min, max, sum, base;
} stat_t;

typedef struct {
    bool used, enabled;
    uint8_t id;
    char name[NAME_CHARS * 3 + 1];
    char devs[MAX_DEVS][32]; /* "<source>:<device id>" */
    int ndev;
    uint16_t period_min, at_min; /* at: minutes after local midnight */
    uint8_t weekday;             /* 0 Monday .. 6, periods of whole weeks */
    /* runtime */
    time_t start, end;   /* the period, 0 before the clock is set */
    time_t sampled_from; /* minimum, maximum and average since (a restart within the period) */
    bool base_dirty;
    int nst;
    stat_t *st; /* MAX_STATS, PSRAM */
} report_t;

typedef struct {
    int64_t start;
    uint16_t n;
} base_head_t;

typedef struct {
    uint32_t h;
    double base;
} base_t;

static source_t s_src[MAX_SOURCES];
static int s_nsrc;
static report_t (*s_rep)[MAX_REPORTS]; /* PSRAM, MAX_USERS */
static char s_owner[MAX_USERS][33];    /* the user the stored reports belong to (a slot may be reused) */
static SemaphoreHandle_t s_lock; /* taken after gw_auth_lock where both are needed */
static TaskHandle_t s_task;

static const uint16_t s_presets[] = {15, 30, 60, 360, 720, 1440, 10080};

/* ---------------------------------------------------------------- helpers */

static uint32_t hash(const char *a, const char *b)
{
    uint32_t h = 2166136261u;
    for (const char *s = a; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    h = (h ^ '/') * 16777619u;
    for (const char *s = b; *s; s++) {
        h = (h ^ (uint8_t)*s) * 16777619u;
    }
    return h;
}

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xc0) != 0x80;
    }
    return n;
}

/* text for Telegram's HTML parse mode */
static size_t put_html(char *out, size_t len, const char *in)
{
    size_t n = 0;
    for (; *in && n + 6 < len; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : NULL;
        if (rep) {
            n += snprintf(out + n, len - n, "%s", rep);
        } else {
            out[n++] = *in;
        }
    }
    if (len) {
        out[n < len ? n : len - 1] = '\0';
    }
    return n;
}

static const source_t *find_source(const char *dev, const char **id)
{
    const char *colon = strchr(dev, ':');
    for (int i = 0; colon && i < s_nsrc; i++) {
        if (strlen(s_src[i].id) == (size_t)(colon - dev) && strncmp(s_src[i].id, dev, colon - dev) == 0) {
            *id = colon + 1;
            return &s_src[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- periods */

static time_t local_time(int year, int mon, int mday, int minutes)
{
    struct tm t = {.tm_year = year, .tm_mon = mon, .tm_mday = mday, .tm_min = minutes, .tm_isdst = -1};
    return mktime(&t); /* normalises days and minutes past the end */
}

static long floor_div(long a, long b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

/* The period of r holding now: [start, end), aligned to the local clock. Whole days from at_min of a day
 * (weeks from the weekday), parts of a day from at_min + k * period, other periods in steps from at_min of
 * Monday 2024-01-01. */
static void period_of(const report_t *r, time_t now, time_t *start, time_t *end)
{
    struct tm lt;
    localtime_r(&now, &lt);
    int p = r->period_min;
    if (p % 1440 == 0) {
        long days = p / 1440;
        long today = lround(difftime(local_time(lt.tm_year, lt.tm_mon, lt.tm_mday, 720), local_time(124, 0, 1, 720)) / 86400);
        long ref = p == 10080 ? r->weekday : 0;
        long d0 = ref + floor_div(today - ref, days) * days;
        time_t s = local_time(124, 0, 1 + d0, r->at_min);
        if (s > now) {
            d0 -= days;
            s = local_time(124, 0, 1 + d0, r->at_min);
        }
        *start = s;
        *end = local_time(124, 0, 1 + d0 + days, r->at_min);
    } else if (1440 % p == 0) {
        int m = lt.tm_hour * 60 + lt.tm_min;
        int from = m - ((m - r->at_min) % p + p) % p;
        *start = local_time(lt.tm_year, lt.tm_mon, lt.tm_mday, from);
        *end = local_time(lt.tm_year, lt.tm_mon, lt.tm_mday, from + p);
    } else {
        time_t base = local_time(124, 0, 1, r->at_min);
        long k = floor_div((long)(now - base), (long)p * 60);
        *start = base + (time_t)k * p * 60;
        *end = *start + (time_t)p * 60;
    }
}

/* ---------------------------------------------------------------- counters at the start of a period */

static void base_key(int u, int k, char *key, size_t len)
{
    snprintf(key, len, KEY_BASE, u, s_rep[u][k].id);
}

static void save_bases(int u, int k)
{
    report_t *r = &s_rep[u][k];
    size_t len = sizeof(base_head_t) + MAX_STATS * sizeof(base_t);
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return;
    }
    base_head_t *head = (base_head_t *)buf;
    base_t *b = (base_t *)(buf + sizeof(*head));
    head->start = r->start;
    head->n = 0;
    for (int i = 0; i < r->nst; i++) {
        if (r->st[i].has_base) {
            b[head->n].h = r->st[i].h;
            b[head->n++].base = r->st[i].base;
        }
    }
    char key[16];
    base_key(u, k, key, sizeof(key));
    gw_config_set_blob(key, buf, sizeof(*head) + head->n * sizeof(base_t));
    free(buf);
    r->base_dirty = false;
}

/* the counters stored for this period, after a restart */
static bool load_bases(int u, int k)
{
    report_t *r = &s_rep[u][k];
    char key[16];
    size_t len = 0;
    base_key(u, k, key, sizeof(key));
    if (gw_config_blob_size(key, &len) != ESP_OK || len < sizeof(base_head_t) || len > sizeof(base_head_t) + MAX_STATS * sizeof(base_t)) {
        return false;
    }
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    bool ok = buf && gw_config_get_blob(key, buf, len);
    const base_head_t *head = (const base_head_t *)buf;
    ok = ok && head->start == r->start && len == sizeof(*head) + head->n * sizeof(base_t);
    for (int i = 0; ok && i < head->n && r->nst < MAX_STATS; i++) {
        const base_t *b = (const base_t *)(buf + sizeof(*head)) + i;
        stat_t *s = &r->st[r->nst++];
        memset(s, 0, sizeof(*s));
        s->h = b->h;
        s->kind = GW_RV_TOTAL;
        s->has_base = true;
        s->base = b->base;
    }
    free(buf);
    return ok;
}

/* ---------------------------------------------------------------- sampling */

static stat_t *find_stat(report_t *r, uint32_t h, bool add)
{
    for (int i = 0; i < r->nst; i++) {
        if (r->st[i].h == h) {
            return &r->st[i];
        }
    }
    if (!add || r->nst == MAX_STATS) {
        return NULL;
    }
    stat_t *s = &r->st[r->nst++];
    memset(s, 0, sizeof(*s));
    s->h = h;
    return s;
}

/* a new period of report k of user u starts (or the first one after a restart) */
static void begin(int u, int k, time_t now)
{
    report_t *r = &s_rep[u][k];
    period_of(r, now, &r->start, &r->end);
    r->nst = 0;
    r->sampled_from = r->start;
    if (load_bases(u, k)) {
        r->sampled_from = now; /* the gateway restarted within the period */
    }
}

static void sample(report_t *r, gw_rv_t *v)
{
    for (int d = 0; d < r->ndev; d++) {
        const char *id;
        const source_t *src = find_source(r->devs[d], &id);
        int n = src ? src->values(id, false, v, MAX_VALUES) : -1;
        for (int i = 0; i < n; i++) {
            if (!v[i].num || v[i].kind == GW_RV_STATE) {
                continue;
            }
            stat_t *s = find_stat(r, hash(r->devs[d], v[i].key), true);
            if (!s) {
                break;
            }
            s->kind = v[i].kind;
            if (v[i].kind == GW_RV_TOTAL && !s->has_base) {
                s->base = v[i].value;
                s->has_base = true;
                r->base_dirty = true;
            } else if (v[i].kind == GW_RV_MEASURE) {
                s->min = s->n ? fmin(s->min, v[i].value) : v[i].value;
                s->max = s->n ? fmax(s->max, v[i].value) : v[i].value;
                s->sum += v[i].value;
                s->n++;
            }
        }
    }
}

/* ---------------------------------------------------------------- text */

static void fmt_time(time_t t, char *out, size_t len)
{
    struct tm lt;
    localtime_r(&t, &lt);
    strftime(out, len, "%d.%m. %H:%M", &lt);
}

static void fmt_num(double v, int prec, char *out, size_t len)
{
    snprintf(out, len, "%.*f", prec < 0 ? 0 : prec, v);
}

/* The report as Telegram HTML into text (TEXT_MAX); only the devices user may read. */
static void render(int u, const report_t *r, bool czech, time_t now, char *text, gw_rv_t *v, gw_rdev_t *devs)
{
    size_t n = 0;
    char a[24], b[24], esc[300];
    fmt_time(r->start, a, sizeof(a));
    fmt_time(now < r->end ? now : r->end, b, sizeof(b));
    put_html(esc, sizeof(esc), r->name);
    n += snprintf(text + n, TEXT_MAX - n, "📊 <b>%s</b> · %s\n%s – %s", esc, gw_state()->hostname, a, b);
    if (r->sampled_from > r->start) {
        fmt_time(r->sampled_from, a, sizeof(a));
        n += snprintf(text + n, TEXT_MAX - n, czech ? "\n<i>min, max a průměr od %s (restart brány)</i>" : "\n<i>min, max and average since %s (gateway restart)</i>", a);
    }
    for (int d = 0; d < r->ndev && n < TEXT_MAX - 512; d++) {
        const char *id;
        const source_t *src = find_source(r->devs[d], &id);
        if (!src || gw_auth_user_level(u, src->page) < 1) {
            continue;
        }
        int nd = src->devices(devs, 32);
        const char *name = id;
        for (int k = 0; k < nd; k++) {
            if (strcmp(devs[k].id, id) == 0) {
                name = devs[k].name;
            }
        }
        put_html(esc, sizeof(esc), name);
        n += snprintf(text + n, TEXT_MAX - n, "\n\n<b>%s</b>", esc);
        int nv = src->values(id, czech, v, MAX_VALUES);
        if (nv < 0) {
            n += snprintf(text + n, TEXT_MAX - n, czech ? ": nedostupné" : ": unavailable");
            continue;
        }
        for (int i = 0; i < nv && n < TEXT_MAX - 256; i++) {
            const stat_t *s = NULL;
            for (int k = 0; k < r->nst && !s; k++) {
                s = r->st[k].h == hash(r->devs[d], v[i].key) ? &r->st[k] : NULL;
            }
            char val[48], unit[24];
            if (v[i].num) {
                fmt_num(v[i].value, v[i].precision, val, sizeof(val));
            } else {
                put_html(val, sizeof(val), v[i].text);
            }
            put_html(esc, sizeof(esc), v[i].name);
            snprintf(unit, sizeof(unit), "%s%s", v[i].unit[0] && strcmp(v[i].unit, "%") ? " " : "", v[i].unit);
            n += snprintf(text + n, TEXT_MAX - n, "\n• %s: %s%s", esc, val, unit);
            if (s && s->kind == GW_RV_MEASURE && s->n && v[i].num) {
                char lo[24], hi[24], avg[24];
                fmt_num(s->min, v[i].precision, lo, sizeof(lo));
                fmt_num(s->max, v[i].precision, hi, sizeof(hi));
                fmt_num(s->sum / s->n, v[i].precision, avg, sizeof(avg));
                n += snprintf(text + n, TEXT_MAX - n, " <i>(min %s · max %s · ⌀ %s)</i>", lo, hi, avg);
            } else if (s && s->kind == GW_RV_TOTAL && s->has_base && v[i].num) {
                fmt_num(v[i].value - s->base, v[i].precision, val, sizeof(val));
                n += snprintf(text + n, TEXT_MAX - n, czech ? " <i>(+%s%s za období)</i>" : " <i>(+%s%s in the period)</i>", val, unit);
            }
        }
    }
    if (n >= TEXT_MAX - 512) {
        snprintf(text + n, TEXT_MAX - n, "\n…");
    }
}

/* Splits text at line ends into parts of at most PART_MAX bytes: calls fn per part. */
static void split(char *text, void (*fn)(const char *part, void *ctx), void *ctx)
{
    char *p = text;
    while (*p) {
        size_t len = strlen(p);
        if (len <= PART_MAX) {
            fn(p, ctx);
            return;
        }
        char *cut = p + PART_MAX;
        while (cut > p && *cut != '\n') {
            cut--;
        }
        if (cut == p) {
            cut = p + PART_MAX;
            while (cut > p && ((unsigned char)*cut & 0xc0) == 0x80) {
                cut--; /* not inside a UTF-8 character */
            }
        }
        char c = *cut;
        *cut = '\0';
        fn(p, ctx);
        *cut = c;
        p = c == '\n' ? cut + 1 : cut;
    }
}

static void send_part(const char *part, void *ctx)
{
    int *u = ctx;
    if (!gw_telegram_send_user(*u, part)) {
        ESP_LOGW(TAG, "report to user %d not sent: not paired or the bot is off", *u);
    }
}

static void add_part(const char *part, void *ctx)
{
    cJSON_AddItemToArray((cJSON *)ctx, cJSON_CreateString(part));
}

static bool user_czech(int u)
{
    return strcmp(gw_auth_user_tg(u)->lang, "en") != 0;
}

/* ---------------------------------------------------------------- task */

static void task(void *arg)
{
    char *text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    gw_rv_t *v = heap_caps_malloc(MAX_VALUES * sizeof(gw_rv_t), MALLOC_CAP_SPIRAM);
    gw_rdev_t *devs = heap_caps_malloc(32 * sizeof(gw_rdev_t), MALLOC_CAP_SPIRAM);
    if (!text || !v || !devs) {
        ESP_LOGE(TAG, "no memory");
        vTaskDelete(NULL);
        return;
    }
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAMPLE_S * 1000));
        time_t now = time(NULL);
        if (now < 1700000000) {
            continue; /* no clock yet */
        }
        for (int u = 0; u < MAX_USERS; u++) {
            for (int k = 0; k < MAX_REPORTS; k++) {
                gw_auth_lock(); /* always before s_lock */
                xSemaphoreTake(s_lock, portMAX_DELAY);
                report_t *r = &s_rep[u][k];
                bool due = false;
                /* a deleted user's reports stop; a new user in the slot never gets them */
                bool owner = gw_auth_user_valid(u) && strcmp(gw_auth_user_name(u), s_owner[u]) == 0;
                if (r->used && r->enabled && r->st && owner) {
                    if (!r->start) {
                        begin(u, k, now);
                    }
                    if (now >= r->end) {
                        render(u, r, user_czech(u), r->end, text, v, devs);
                        due = true;
                        r->nst = 0;
                        period_of(r, now, &r->start, &r->end);
                        r->sampled_from = r->start;
                    }
                    sample(r, v);
                    if (r->base_dirty) {
                        save_bases(u, k);
                    }
                }
                xSemaphoreGive(s_lock);
                gw_auth_unlock();
                if (due) {
                    split(text, send_part, &u);
                }
            }
        }
    }
}

/* ---------------------------------------------------------------- configuration */

static void cfg_json(int u, cJSON *list)
{
    for (int k = 0; k < MAX_REPORTS; k++) {
        const report_t *r = &s_rep[u][k];
        if (!r->used) {
            continue;
        }
        cJSON *j = cJSON_CreateObject();
        cJSON_AddNumberToObject(j, "id", r->id);
        cJSON_AddStringToObject(j, "name", r->name);
        cJSON *d = cJSON_AddArrayToObject(j, "devices");
        for (int i = 0; i < r->ndev; i++) {
            cJSON_AddItemToArray(d, cJSON_CreateString(r->devs[i]));
        }
        cJSON_AddNumberToObject(j, "period_min", r->period_min);
        cJSON_AddNumberToObject(j, "at", r->at_min);
        cJSON_AddNumberToObject(j, "weekday", r->weekday);
        cJSON_AddBoolToObject(j, "enabled", r->enabled);
        cJSON_AddItemToArray(list, j);
    }
}

static void save_user(int u)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "user", s_owner[u]);
    cfg_json(u, cJSON_AddArrayToObject(root, "reports"));
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    char key[16];
    snprintf(key, sizeof(key), KEY_CFG, u);
    if (text) {
        gw_config_set_str(key, text);
        cJSON_free(text);
    }
}

/* One report from JSON for user u (rights checked); NULL when fine, else why not. */
static const char *parse_report(int u, const cJSON *j, report_t *r)
{
    static char why[96];
    memset(r, 0, sizeof(*r));
    const char *name = GW_JSON_STR(j, "name");
    const cJSON *devs = cJSON_GetObjectItem(j, "devices"), *period = cJSON_GetObjectItem(j, "period_min"), *at = cJSON_GetObjectItem(j, "at"),
                *wd = cJSON_GetObjectItem(j, "weekday"), *en = cJSON_GetObjectItem(j, "enabled"), *id = cJSON_GetObjectItem(j, "id");
    if (!name || !name[0] || utf8_chars(name) > NAME_CHARS || strlen(name) >= sizeof(r->name)) {
        return "name: 1-40 characters";
    }
    strcpy(r->name, name);
    if (!cJSON_IsArray(devs) || cJSON_GetArraySize(devs) < 1 || cJSON_GetArraySize(devs) > MAX_DEVS) {
        return "devices: 1-8";
    }
    const cJSON *d;
    gw_rdev_t *list = heap_caps_malloc(32 * sizeof(gw_rdev_t), MALLOC_CAP_SPIRAM);
    cJSON_ArrayForEach(d, devs)
    {
        const char *dev_id;
        const source_t *src = cJSON_IsString(d) && strlen(d->valuestring) < sizeof(r->devs[0]) ? find_source(d->valuestring, &dev_id) : NULL;
        bool found = false;
        int n = src && list ? src->devices(list, 32) : 0;
        for (int k = 0; k < n && !found; k++) {
            found = strcmp(list[k].id, dev_id) == 0;
        }
        if (!found || gw_auth_user_level(u, src->page) < 1) {
            snprintf(why, sizeof(why), "devices: %.40s is no device you may read", cJSON_IsString(d) ? d->valuestring : "?");
            free(list);
            return why;
        }
        strcpy(r->devs[r->ndev++], d->valuestring);
    }
    free(list);
    if (!cJSON_IsNumber(period) || period->valueint < PERIOD_MIN || period->valueint > PERIOD_MAX) {
        return "period_min: 15-10080";
    }
    r->period_min = (uint16_t)period->valueint;
    if (at && (!cJSON_IsNumber(at) || at->valueint < 0 || at->valueint > 1439)) {
        return "at: 0-1439 (minutes after midnight)";
    }
    r->at_min = at ? (uint16_t)at->valueint : 7 * 60;
    if (wd && (!cJSON_IsNumber(wd) || wd->valueint < 0 || wd->valueint > 6)) {
        return "weekday: 0 (Monday) - 6";
    }
    r->weekday = wd ? (uint8_t)wd->valueint : 0;
    if (en && !cJSON_IsBool(en)) {
        return "enabled: true or false";
    }
    r->enabled = !en || cJSON_IsTrue(en);
    if (id && (!cJSON_IsNumber(id) || id->valueint < 1 || id->valueint > 255)) {
        return "id: 1-255";
    }
    r->id = id ? (uint8_t)id->valueint : 0;
    r->used = true;
    return NULL;
}

static bool same_schedule(const report_t *a, const report_t *b)
{
    return a->period_min == b->period_min && a->at_min == b->at_min && a->weekday == b->weekday && a->ndev == b->ndev &&
           memcmp(a->devs, b->devs, sizeof(a->devs)) == 0;
}

/* Puts the reports of user u in place; one whose devices and schedule stay keeps its period and samples.
 * Under s_lock. */
static void apply_user(int u, report_t *next, int n)
{
    report_t *cur = s_rep[u];
    for (int k = 0; k < n; k++) {
        report_t *r = &next[k];
        report_t *old = NULL;
        for (int m = 0; m < MAX_REPORTS && r->id && !old; m++) {
            old = cur[m].used && cur[m].id == r->id ? &cur[m] : NULL;
        }
        for (uint8_t id = 1; !r->id; id++) {
            bool taken = false;
            for (int m = 0; m < n; m++) {
                taken |= next[m].id == id;
            }
            for (int m = 0; m < MAX_REPORTS; m++) {
                taken |= cur[m].used && cur[m].id == id; /* a deleted report's counters stay in NVS: not reused */
            }
            r->id = taken ? 0 : id;
        }
        if (old && same_schedule(old, r) && old->enabled == r->enabled) {
            r->start = old->start, r->end = old->end, r->sampled_from = old->sampled_from, r->nst = old->nst;
            r->st = old->st;
            old->st = NULL;
        }
    }
    for (int m = 0; m < MAX_REPORTS; m++) {
        if (cur[m].used) {
            char key[16];
            base_key(u, m, key, sizeof(key));
            bool kept = false;
            for (int k = 0; k < n; k++) {
                kept |= next[k].id == cur[m].id && next[k].st;
            }
            if (!kept) {
                gw_config_erase(key);
            }
        }
        free(cur[m].st);
        cur[m].st = NULL;
    }
    memset(cur, 0, sizeof(report_t) * MAX_REPORTS);
    for (int k = 0; k < n; k++) {
        cur[k] = next[k];
        if (!cur[k].st) {
            cur[k].st = heap_caps_calloc(MAX_STATS, sizeof(stat_t), MALLOC_CAP_SPIRAM);
        }
    }
}

static void load_user(int u)
{
    char key[16];
    snprintf(key, sizeof(key), KEY_CFG, u);
    char *text = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    cJSON *root = text && gw_config_get_str(key, text, 4096) ? cJSON_Parse(text) : NULL;
    free(text);
    const char *owner = GW_JSON_STR(root, "user");
    report_t next[MAX_REPORTS];
    int n = 0;
    gw_auth_lock();
    bool same = owner && gw_auth_user_valid(u) && strcmp(gw_auth_user_name(u), owner) == 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, (same ? cJSON_GetObjectItem(root, "reports") : NULL))
    {
        if (n < MAX_REPORTS && !parse_report(u, j, &next[n])) {
            n++;
        }
    }
    gw_auth_unlock();
    if (same) {
        strlcpy(s_owner[u], owner, sizeof(s_owner[u]));
        apply_user(u, next, n);
    } else if (root) {
        /* the user is gone or the slot has another user now: their reports and counters go */
        cJSON_ArrayForEach(j, cJSON_GetObjectItem(root, "reports"))
        {
            const cJSON *id = cJSON_GetObjectItem(j, "id");
            char bkey[16];
            snprintf(bkey, sizeof(bkey), KEY_BASE, u, cJSON_IsNumber(id) ? id->valueint : 0);
            gw_config_erase(bkey);
        }
        gw_config_erase(key);
    }
    cJSON_Delete(root);
}

void gw_report_source(const char *id, const char *name_en, const char *name_cs, const char *page,
                      int (*devices)(gw_rdev_t *out, int max), int (*values)(const char *dev, bool czech, gw_rv_t *out, int max))
{
    if (s_nsrc == MAX_SOURCES) {
        return;
    }
    source_t *s = &s_src[s_nsrc++];
    strlcpy(s->id, id, sizeof(s->id));
    strlcpy(s->name_en, name_en, sizeof(s->name_en));
    strlcpy(s->name_cs, name_cs, sizeof(s->name_cs));
    strlcpy(s->page, page, sizeof(s->page));
    s->devices = devices;
    s->values = values;
}

/* ---------------------------------------------------------------- HTTP */

static int self_user(httpd_req_t *req)
{
    int u = gw_auth_current_user();
    if (u < 0 || u >= MAX_USERS) {
        gw_http_send_error(req, 409, "only for signed-in users");
        return -1;
    }
    return u;
}

static cJSON *reports_json(int u)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "reports");
    time_t now = time(NULL);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (strcmp(s_owner[u], gw_auth_user_name(u)) == 0) {
        cfg_json(u, list);
    }
    int i = 0;
    cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        while (!s_rep[u][i].used) {
            i++;
        }
        const report_t *r = &s_rep[u][i++];
        time_t start = r->start, end = r->end;
        if (!start && now > 1700000000) {
            period_of(r, now, &start, &end);
        }
        cJSON_AddNumberToObject(j, "period_start", (double)start);
        cJSON_AddNumberToObject(j, "period_end", (double)end);
        cJSON_AddNumberToObject(j, "next_s", end && r->enabled ? (double)(end - now) : -1);
    }
    xSemaphoreGive(s_lock);
    gw_rdev_t *devs = heap_caps_malloc(32 * sizeof(gw_rdev_t), MALLOC_CAP_SPIRAM);
    cJSON *srcs = cJSON_AddArrayToObject(root, "sources");
    for (int s = 0; devs && s < s_nsrc; s++) {
        if (gw_auth_user_level(u, s_src[s].page) < 1) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", s_src[s].id);
        cJSON_AddStringToObject(o, "name", s_src[s].name_en);
        cJSON_AddStringToObject(o, "name_cs", s_src[s].name_cs);
        cJSON *dl = cJSON_AddArrayToObject(o, "devices");
        int n = s_src[s].devices(devs, 32);
        for (int k = 0; k < n; k++) {
            cJSON *d = cJSON_CreateObject();
            char id[32];
            snprintf(id, sizeof(id), "%.11s:%.15s", s_src[s].id, devs[k].id);
            cJSON_AddStringToObject(d, "id", id);
            cJSON_AddStringToObject(d, "name", devs[k].name);
            cJSON_AddItemToArray(dl, d);
        }
        cJSON_AddItemToArray(srcs, o);
    }
    free(devs);
    cJSON *lim = cJSON_AddObjectToObject(root, "limits");
    cJSON_AddNumberToObject(lim, "reports", MAX_REPORTS);
    cJSON_AddNumberToObject(lim, "devices", MAX_DEVS);
    cJSON_AddNumberToObject(lim, "period_min", PERIOD_MIN);
    cJSON_AddNumberToObject(lim, "period_max", PERIOD_MAX);
    cJSON *pre = cJSON_AddArrayToObject(lim, "presets");
    for (size_t k = 0; k < sizeof(s_presets) / sizeof(s_presets[0]); k++) {
        cJSON_AddItemToArray(pre, cJSON_CreateNumber(s_presets[k]));
    }
    cJSON_AddBoolToObject(root, "paired", gw_auth_user_tg(u)->chat != 0);
    return root;
}

static esp_err_t reports_get(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    gw_auth_lock();
    cJSON *j = reports_json(u);
    gw_auth_unlock();
    return gw_http_send_json(req, j);
}

/* {"reports": [{id?, name, devices, period_min, at, weekday, enabled}]}: replaces the user's list */
static esp_err_t reports_put(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *list = cJSON_GetObjectItem(body, "reports");
    report_t *next = heap_caps_calloc(MAX_REPORTS, sizeof(report_t), MALLOC_CAP_SPIRAM);
    char err[128] = "";
    int n = 0;
    if (!next) {
        snprintf(err, sizeof(err), "out of memory");
    } else if (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > MAX_REPORTS) {
        snprintf(err, sizeof(err), "reports: a list of up to %d", MAX_REPORTS);
    }
    gw_auth_lock();
    const cJSON *j;
    cJSON_ArrayForEach(j, (err[0] ? NULL : list))
    {
        const char *why = parse_report(u, j, &next[n]);
        if (why) {
            snprintf(err, sizeof(err), "reports[%d] %s", n, why);
            break;
        }
        for (int m = 0; m < n && next[n].id; m++) {
            if (next[m].id == next[n].id) {
                snprintf(err, sizeof(err), "reports[%d] id: used twice", n);
            }
        }
        n++;
    }
    if (!err[0]) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (strcmp(s_owner[u], gw_auth_user_name(u)) != 0) {
            for (int k = 0; k < MAX_REPORTS; k++) {
                char key[16];
                if (s_rep[u][k].used) {
                    base_key(u, k, key, sizeof(key));
                    gw_config_erase(key);
                }
                free(s_rep[u][k].st);
            }
            memset(s_rep[u], 0, sizeof(report_t) * MAX_REPORTS); /* another user's leftovers */
            strlcpy(s_owner[u], gw_auth_user_name(u), sizeof(s_owner[u]));
        }
        apply_user(u, next, n);
        save_user(u);
        xSemaphoreGive(s_lock);
    }
    gw_auth_unlock();
    free(next);
    cJSON_Delete(body);
    if (err[0]) {
        return gw_http_send_error(req, 400, err);
    }
    xTaskNotifyGive(s_task);
    return reports_get(req);
}

/* {"id": n, "preview": bool}: the report so far; sent to the user's chat unless preview */
static esp_err_t send_post(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *id = cJSON_GetObjectItem(body, "id");
    bool preview = cJSON_IsTrue(cJSON_GetObjectItem(body, "preview"));
    cJSON_Delete(body);
    char *text = heap_caps_malloc(TEXT_MAX, MALLOC_CAP_SPIRAM);
    gw_rv_t *v = heap_caps_malloc(MAX_VALUES * sizeof(gw_rv_t), MALLOC_CAP_SPIRAM);
    gw_rdev_t *devs = heap_caps_malloc(32 * sizeof(gw_rdev_t), MALLOC_CAP_SPIRAM);
    int status = 200;
    const char *err = NULL;
    time_t now = time(NULL);
    if (!text || !v || !devs) {
        status = 500, err = "out of memory";
    } else if (now < 1700000000) {
        status = 409, err = "the clock is not set yet";
    } else {
        gw_auth_lock();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        report_t *r = NULL;
        for (int k = 0; k < MAX_REPORTS && cJSON_IsNumber(id) && !r; k++) {
            if (s_rep[u][k].used && s_rep[u][k].id == id->valueint && strcmp(s_owner[u], gw_auth_user_name(u)) == 0) {
                r = &s_rep[u][k];
                if (!r->start) {
                    begin(u, k, now);
                }
            }
        }
        if (r && r->st) {
            render(u, r, user_czech(u), now, text, v, devs);
        } else {
            status = 404, err = "no such report";
        }
        bool paired = gw_auth_user_tg(u)->chat != 0;
        xSemaphoreGive(s_lock);
        gw_auth_unlock();
        if (!err && !preview && !paired) {
            status = 409, err = "not paired with Telegram";
        }
    }
    cJSON *j = NULL;
    if (!err) {
        j = cJSON_CreateObject();
        cJSON *parts = cJSON_AddArrayToObject(j, "parts");
        split(text, add_part, parts);
        if (!preview) {
            split(text, send_part, &u);
        }
        cJSON_AddBoolToObject(j, "sent", !preview);
    }
    free(text);
    free(v);
    free(devs);
    return err ? gw_http_send_error(req, status, err) : gw_http_send_json(req, j);
}

/* ---------------------------------------------------------------- start */

void gw_report_start(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_rep = heap_caps_calloc(MAX_USERS, sizeof(*s_rep), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_rep) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    gw_psram_note("reports", MAX_USERS * sizeof(*s_rep));
    for (int u = 0; u < MAX_USERS && u < gw_auth_users_max(); u++) {
        load_user(u);
    }
    gw_http_register("/api/auth/telegram/reports", HTTP_GET, reports_get);
    gw_http_register("/api/auth/telegram/reports", HTTP_PUT, reports_put);
    gw_http_register("/api/auth/telegram/reports/send", HTTP_POST, send_post);
    xTaskCreate(task, "report", 6144, NULL, 2, &s_task);
}
