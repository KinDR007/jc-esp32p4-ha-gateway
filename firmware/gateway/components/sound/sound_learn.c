#include "sound_learn.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_rom_crc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "gw.h"
#include "sound_model.h"

static const char *TAG = "sound_learn";

#define PART_SUBTYPE 0x43
#define SLOT_SIZE 0x9000 /* header, embedding, audio; whole sectors */
#define SLOT_MAGIC 0x53444e53u /* "SNDS" */
#define SLOT_VERSION 1
#define EMB_OFFSET 0x100
#define AUDIO_OFFSET (EMB_OFFSET + SOUND_EMBEDDING * sizeof(float))
#define MAX_SLOTS 64
#define MAX_PROFILES 16
#define BACKGROUND "_background"
#define SAMPLE_WAIT_US (4 * 1000000LL) /* a sample is the loudest window within this time */
#define BACKGROUND_WINDOWS 5          /* every second window: no overlap, about 5 s */
#define TONE_BLOCK 512
#define TONE_MIN_HZ 150
#define TONE_MAX_HZ 7500
#define TONE_STEP_HZ 5
#define TONE_COARSE_HZ 40 /* below the 31 Hz block resolution x 2 */

_Static_assert(AUDIO_OFFSET + LEARN_AUDIO * sizeof(int16_t) <= SLOT_SIZE, "slot too small");

enum { KIND_SAMPLE = 0, KIND_BACKGROUND = 1 };
enum { TYPE_EMBEDDING = 0, TYPE_TONE = 1 };

typedef struct {
    uint32_t magic;
    uint8_t version, kind, type, reserved;
    char profile[24];
    uint32_t saved_unix;
    float level_db;
    float tone_hz;    /* dominant frequency of this sample (tone type) */
    float tone_ratio; /* share of its energy at tone_hz */
    uint32_t crc;     /* of embedding and audio */
} slot_hdr_t;

typedef struct {
    char id[24];
    uint8_t type;
    int samples;
    float *centroid; /* unit vector, PSRAM */
    float tone_hz, tone_ratio;
    float self_min; /* lowest score of a sample against the others */
    float bg_max;   /* highest score of a background recording */
    float score;
    int64_t score_us;
} profile_t;

static const esp_partition_t *s_part;
static int s_slot_count;
static slot_hdr_t s_slots[MAX_SLOTS]; /* magic != SLOT_MAGIC: free */
static profile_t s_prof[MAX_PROFILES];
static int s_prof_count;
static SemaphoreHandle_t s_lock;
static sound_learn_hooks_t s_hooks;

#define UPLOAD_MAX_S 10
/* an uploaded WAV, handed to the inference task (the model runs only there) */
static struct {
    int16_t *audio;
    int n;
    bool test; /* only score it against the profiles */
    char profile[24];
    uint8_t kind, type;
    SemaphoreHandle_t done;
    esp_err_t err;
    char msg[96];
    cJSON *result;
    volatile bool pending;
    volatile bool abandoned; /* the request timed out: nobody takes the result */
} s_job;

/* recording in progress, driven by sound_learn_window() */
static struct {
    bool active;
    char profile[24];
    uint8_t kind, type;
    int64_t until_us;
    int windows, saved;
    float best_level;
    float *emb;
    int16_t *audio;
    char result[96];
    bool ok;
} s_rec;

/* ---------------------------------------------------------------- signal helpers */

static float cosine(const float *a, const float *b)
{
    double ab = 0, aa = 0, bb = 0;
    for (int i = 0; i < SOUND_EMBEDDING; i++) {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return aa > 0 && bb > 0 ? (float)(ab / sqrt(aa * bb)) : 0;
}

static void normalize(float *v)
{
    double n = 0;
    for (int i = 0; i < SOUND_EMBEDDING; i++) {
        n += v[i] * v[i];
    }
    float k = n > 0 ? (float)(1.0 / sqrt(n)) : 0;
    for (int i = 0; i < SOUND_EMBEDDING; i++) {
        v[i] *= k;
    }
}

/* Share of the energy at hz, Goertzel over blocks of TONE_BLOCK: 1 for a pure tone at hz. */
static float tone_ratio(const int16_t *a, int n, float hz)
{
    float k = 2.0f * cosf(2.0f * (float)M_PI * hz / MEL_SAMPLE_RATE);
    double tone = 0, energy = 0;
    for (int off = 0; off + TONE_BLOCK <= n; off += TONE_BLOCK) {
        float s1 = 0, s2 = 0;
        for (int i = 0; i < TONE_BLOCK; i++) {
            float x = a[off + i];
            float s0 = x + k * s1 - s2;
            s2 = s1;
            s1 = s0;
            energy += x * x;
        }
        tone += s1 * s1 + s2 * s2 - k * s1 * s2;
    }
    float r = energy > 0 ? (float)(2.0 * tone / (TONE_BLOCK * energy)) : 0;
    return r > 1 ? 1 : r;
}

/* Coarse search over the loudest 4096 samples, then a fine one around the best frequency. */
static float dominant_hz(const int16_t *a, int n, float *ratio)
{
    const int seg = 4096;
    int start = 0;
    double loudest = -1;
    for (int off = 0; off + seg <= n; off += seg / 4) {
        double e = 0;
        for (int i = 0; i < seg; i++) {
            e += (double)a[off + i] * a[off + i];
        }
        if (e > loudest) {
            loudest = e;
            start = off;
        }
    }
    float best = 0, best_hz = TONE_MIN_HZ;
    for (int hz = TONE_MIN_HZ; hz <= TONE_MAX_HZ; hz += TONE_COARSE_HZ) {
        float r = tone_ratio(a + start, seg < n ? seg : n, hz);
        if (r > best) {
            best = r;
            best_hz = hz;
        }
    }
    float centre = best_hz;
    best = 0;
    for (float hz = centre - TONE_COARSE_HZ; hz <= centre + TONE_COARSE_HZ; hz += TONE_STEP_HZ) {
        float r = hz >= TONE_MIN_HZ ? tone_ratio(a, n, hz) : 0;
        if (r > best) {
            best = r;
            best_hz = hz;
        }
    }
    *ratio = best;
    return best_hz;
}

/* ---------------------------------------------------------------- storage */

static uint32_t slot_addr(int slot)
{
    return (uint32_t)slot * SLOT_SIZE;
}

static bool slot_used(int slot)
{
    return s_slots[slot].magic == SLOT_MAGIC;
}

static esp_err_t read_emb(int slot, float *emb)
{
    return esp_partition_read(s_part, slot_addr(slot) + EMB_OFFSET, emb, SOUND_EMBEDDING * sizeof(float));
}

static esp_err_t read_audio(int slot, int16_t *audio)
{
    return esp_partition_read(s_part, slot_addr(slot) + AUDIO_OFFSET, audio, LEARN_AUDIO * sizeof(int16_t));
}

static int profile_find(const char *id)
{
    for (int i = 0; i < s_prof_count; i++) {
        if (strcmp(s_prof[i].id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* Score of a stored slot against a profile. */
static float slot_score(const profile_t *p, int slot, float *emb, int16_t *audio)
{
    if (p->type == TYPE_TONE) {
        if (read_audio(slot, audio) != ESP_OK || p->tone_ratio <= 0) {
            return 0;
        }
        float r = tone_ratio(audio, LEARN_AUDIO, p->tone_hz) / p->tone_ratio;
        return r > 1 ? 1 : r;
    }
    return read_emb(slot, emb) == ESP_OK ? cosine(emb, p->centroid) : 0;
}

/* Rebuilds the profiles (centroids, tone frequency, calibration) from the slot headers. Lock held. */
static void rebuild(void)
{
    float *emb = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
    int16_t *audio = heap_caps_malloc(LEARN_AUDIO * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    for (int i = 0; i < s_prof_count; i++) {
        free(s_prof[i].centroid);
    }
    memset(s_prof, 0, sizeof(s_prof));
    s_prof_count = 0;
    if (!emb || !audio) {
        free(emb);
        free(audio);
        return;
    }
    for (int slot = 0; slot < s_slot_count; slot++) {
        const slot_hdr_t *h = &s_slots[slot];
        if (!slot_used(slot) || h->kind != KIND_SAMPLE) {
            continue;
        }
        int i = profile_find(h->profile);
        if (i < 0) {
            if (s_prof_count == MAX_PROFILES) {
                continue;
            }
            i = s_prof_count++;
            strlcpy(s_prof[i].id, h->profile, sizeof(s_prof[i].id));
            s_prof[i].type = h->type;
            s_prof[i].centroid = heap_caps_calloc(SOUND_EMBEDDING, sizeof(float), MALLOC_CAP_SPIRAM);
        }
        profile_t *p = &s_prof[i];
        if (!p->centroid || read_emb(slot, emb) != ESP_OK) {
            continue;
        }
        normalize(emb);
        for (int k = 0; k < SOUND_EMBEDDING; k++) {
            p->centroid[k] += emb[k];
        }
        p->samples++;
        p->tone_hz += h->tone_hz;
        p->tone_ratio += h->tone_ratio;
    }
    for (int i = 0; i < s_prof_count; i++) {
        profile_t *p = &s_prof[i];
        p->tone_hz /= p->samples;
        p->tone_ratio /= p->samples;
        /* sum of the unit sample vectors, kept for leave-one-out below */
        float *sum = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
        if (sum) {
            memcpy(sum, p->centroid, SOUND_EMBEDDING * sizeof(float));
        }
        normalize(p->centroid);
        p->self_min = 1;
        for (int slot = 0; slot < s_slot_count; slot++) {
            const slot_hdr_t *h = &s_slots[slot];
            if (!slot_used(slot) || h->kind != KIND_SAMPLE || strcmp(h->profile, p->id) != 0) {
                continue;
            }
            float sc;
            if (p->type == TYPE_TONE) {
                sc = slot_score(p, slot, emb, audio);
            } else if (sum && p->samples > 1 && read_emb(slot, emb) == ESP_OK) {
                /* leave one out: this sample against the mean of the others */
                float *others = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
                normalize(emb);
                sc = 1;
                if (others) {
                    for (int k = 0; k < SOUND_EMBEDDING; k++) {
                        others[k] = sum[k] - emb[k];
                    }
                    sc = cosine(emb, others);
                }
                free(others);
            } else {
                sc = 1;
            }
            p->self_min = fminf(p->self_min, sc);
        }
        free(sum);
        p->bg_max = 0;
        for (int slot = 0; slot < s_slot_count; slot++) {
            if (slot_used(slot) && s_slots[slot].kind == KIND_BACKGROUND) {
                p->bg_max = fmaxf(p->bg_max, slot_score(p, slot, emb, audio));
            }
        }
        p->score = -1;
    }
    free(emb);
    free(audio);
}

static void scan(void)
{
    for (int slot = 0; slot < s_slot_count; slot++) {
        if (esp_partition_read(s_part, slot_addr(slot), &s_slots[slot], sizeof(slot_hdr_t)) != ESP_OK ||
            s_slots[slot].version != SLOT_VERSION) {
            s_slots[slot].magic = 0;
        }
        s_slots[slot].profile[sizeof(s_slots[slot].profile) - 1] = '\0';
    }
}

static int free_slot(void)
{
    for (int slot = 0; slot < s_slot_count; slot++) {
        if (!slot_used(slot)) {
            return slot;
        }
    }
    return -1;
}

static int free_slots(void)
{
    int n = 0;
    for (int slot = 0; slot < s_slot_count; slot++) {
        n += !slot_used(slot);
    }
    return n;
}

/* Writes one recording (s_rec.emb, s_rec.audio). Lock held. */
static esp_err_t save_slot(float level_db)
{
    int slot = free_slot();
    if (slot < 0) {
        return ESP_ERR_NO_MEM;
    }
    slot_hdr_t h = {.magic = SLOT_MAGIC, .version = SLOT_VERSION, .kind = s_rec.kind, .type = s_rec.type,
                    .level_db = level_db};
    strlcpy(h.profile, s_rec.kind == KIND_BACKGROUND ? BACKGROUND : s_rec.profile, sizeof(h.profile));
    time_t now = time(NULL);
    h.saved_unix = now > 1700000000 ? (uint32_t)now : 0;
    if (s_rec.kind == KIND_SAMPLE && s_rec.type == TYPE_TONE) {
        h.tone_hz = dominant_hz(s_rec.audio, LEARN_AUDIO, &h.tone_ratio);
    }
    h.crc = esp_rom_crc32_le(0, (const uint8_t *)s_rec.emb, SOUND_EMBEDDING * sizeof(float));
    h.crc = esp_rom_crc32_le(h.crc, (const uint8_t *)s_rec.audio, LEARN_AUDIO * sizeof(int16_t));
    uint32_t base = slot_addr(slot);
    /* data first, header last: an interrupted write leaves a free slot */
    esp_err_t err = esp_partition_erase_range(s_part, base, SLOT_SIZE);
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, base + EMB_OFFSET, s_rec.emb, SOUND_EMBEDDING * sizeof(float));
    }
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, base + AUDIO_OFFSET, s_rec.audio, LEARN_AUDIO * sizeof(int16_t));
    }
    if (err == ESP_OK) {
        err = esp_partition_write(s_part, base, &h, sizeof(h));
    }
    if (err == ESP_OK) {
        s_slots[slot] = h;
        ESP_LOGI(TAG, "slot %d: %s %s, %.1f dBFS%s", slot, h.kind == KIND_SAMPLE ? "sample of" : "background",
                 h.profile, level_db, h.tone_hz > 0 ? ", tone" : "");
    }
    return err;
}

static esp_err_t erase_slot(int slot)
{
    s_slots[slot].magic = 0;
    return esp_partition_erase_range(s_part, slot_addr(slot), SLOT_SIZE);
}

/* ---------------------------------------------------------------- runtime */

bool sound_learn_wants_all(void)
{
    return s_rec.active;
}

static void finish(bool ok, const char *msg)
{
    s_rec.active = false;
    s_rec.ok = ok;
    strlcpy(s_rec.result, msg, sizeof(s_rec.result));
    ESP_LOGI(TAG, "recording: %s", msg);
}

void sound_learn_window(const float *embedding, const int16_t *audio, float level_db, int64_t now)
{
    if (!s_lock) {
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rec.active && s_rec.kind == KIND_SAMPLE) {
        if (level_db > s_rec.best_level) {
            s_rec.best_level = level_db;
            memcpy(s_rec.emb, embedding, SOUND_EMBEDDING * sizeof(float));
            memcpy(s_rec.audio, audio, LEARN_AUDIO * sizeof(int16_t));
        }
        if (now >= s_rec.until_us && s_rec.best_level < s_hooks.gate_db()) {
            char msg[96];
            snprintf(msg, sizeof(msg), "nothing heard: loudest %.1f dBFS, below the %.0f dBFS gate", s_rec.best_level,
                     s_hooks.gate_db());
            finish(false, msg);
        } else if (now >= s_rec.until_us) {
            esp_err_t err = save_slot(s_rec.best_level);
            if (err == ESP_OK) {
                rebuild();
                char msg[64];
                snprintf(msg, sizeof(msg), "sample saved (%.1f dBFS)", s_rec.best_level);
                finish(true, msg);
            } else {
                finish(false, err == ESP_ERR_NO_MEM ? "no free sample slot, delete some" : esp_err_to_name(err));
            }
        }
    } else if (s_rec.active && s_rec.kind == KIND_BACKGROUND) {
        /* every second window: windows overlap by half */
        if (s_rec.windows++ % 2 == 0) {
            memcpy(s_rec.emb, embedding, SOUND_EMBEDDING * sizeof(float));
            memcpy(s_rec.audio, audio, LEARN_AUDIO * sizeof(int16_t));
            esp_err_t err = save_slot(level_db);
            if (err != ESP_OK) {
                rebuild();
                finish(false, err == ESP_ERR_NO_MEM ? "no free sample slot, delete some" : esp_err_to_name(err));
            } else if (++s_rec.saved == BACKGROUND_WINDOWS) {
                rebuild();
                finish(true, "background saved");
            }
        }
    }
    for (int i = 0; i < s_prof_count; i++) {
        profile_t *p = &s_prof[i];
        if (p->type == TYPE_TONE) {
            float r = p->tone_ratio > 0 ? tone_ratio(audio, LEARN_AUDIO, p->tone_hz) / p->tone_ratio : 0;
            p->score = r > 1 ? 1 : r;
        } else {
            p->score = p->centroid ? cosine(embedding, p->centroid) : 0;
        }
        p->score_us = now;
    }
    xSemaphoreGive(s_lock);
}

float sound_learn_score(const char *profile)
{
    if (!s_lock) {
        return -1;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int i = profile_find(profile);
    float score = i < 0 ? -1 : s_prof[i].score < 0 ? 0 : s_prof[i].score;
    xSemaphoreGive(s_lock);
    return score;
}

/* Threshold between the background and the samples; at the weakest sample when they overlap. */
static float suggested(const profile_t *p, bool *separable)
{
    bool has_bg = false;
    for (int slot = 0; slot < s_slot_count && !has_bg; slot++) {
        has_bg = slot_used(slot) && s_slots[slot].kind == KIND_BACKGROUND;
    }
    float t;
    *separable = !has_bg || p->bg_max < p->self_min;
    if (!has_bg) {
        t = p->self_min * 0.9f;
    } else if (*separable) {
        /* closer to the samples than to the background: other sounds of the home were not recorded */
        t = p->bg_max + 0.7f * (p->self_min - p->bg_max);
    } else {
        t = p->self_min;
    }
    /* YAMNet embeddings are not negative: unrelated sounds still reach a cosine of about 0.4-0.5 */
    float floor = p->type == TYPE_TONE ? 0.3f : 0.5f;
    t = t < floor ? floor : t > 0.98f ? 0.98f : t;
    return t;
}

/* ---------------------------------------------------------------- API */

static cJSON *slot_json(int slot)
{
    const slot_hdr_t *h = &s_slots[slot];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "slot", slot);
    cJSON_AddNumberToObject(j, "level_db", round((double)h->level_db * 10) / 10);
    cJSON_AddNumberToObject(j, "saved", h->saved_unix);
    if (h->tone_hz > 0) {
        cJSON_AddNumberToObject(j, "tone_hz", round((double)h->tone_hz));
    }
    return j;
}

static esp_err_t learn_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "slots_total", s_slot_count);
    cJSON_AddNumberToObject(json, "slots_free", free_slots());
    cJSON_AddBoolToObject(json, "ready", s_hooks.ready && s_hooks.ready());
    cJSON *rec = cJSON_AddObjectToObject(json, "recording");
    cJSON_AddBoolToObject(rec, "active", s_rec.active);
    if (s_rec.active) {
        cJSON_AddStringToObject(rec, "profile", s_rec.profile);
        cJSON_AddStringToObject(rec, "kind", s_rec.kind == KIND_SAMPLE ? "sample" : "background");
    }
    cJSON_AddStringToObject(rec, "result", s_rec.result);
    cJSON_AddBoolToObject(rec, "ok", s_rec.ok);
    int64_t now = esp_timer_get_time();
    cJSON *profiles = cJSON_AddArrayToObject(json, "profiles");
    for (int i = 0; i < s_prof_count; i++) {
        const profile_t *p = &s_prof[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", p->id);
        cJSON_AddStringToObject(j, "type", p->type == TYPE_TONE ? "tone" : "embedding");
        cJSON_AddNumberToObject(j, "samples", p->samples);
        if (p->type == TYPE_TONE) {
            cJSON_AddNumberToObject(j, "tone_hz", roundf(p->tone_hz));
        }
        cJSON_AddNumberToObject(j, "self_min", round((double)p->self_min * 100) / 100);
        cJSON_AddNumberToObject(j, "background_max", round((double)p->bg_max * 100) / 100);
        bool separable;
        cJSON_AddNumberToObject(j, "threshold", round((double)suggested(p, &separable) * 100) / 100);
        cJSON_AddBoolToObject(j, "separable", separable);
        if (p->score >= 0 && now - p->score_us < 5000000) {
            cJSON_AddNumberToObject(j, "score", round((double)p->score * 100) / 100);
        }
        cJSON *slots = cJSON_AddArrayToObject(j, "slots");
        for (int slot = 0; slot < s_slot_count; slot++) {
            if (slot_used(slot) && s_slots[slot].kind == KIND_SAMPLE && strcmp(s_slots[slot].profile, p->id) == 0) {
                cJSON_AddItemToArray(slots, slot_json(slot));
            }
        }
        cJSON_AddItemToArray(profiles, j);
    }
    cJSON *bg = cJSON_AddArrayToObject(json, "background");
    for (int slot = 0; slot < s_slot_count; slot++) {
        if (slot_used(slot) && s_slots[slot].kind == KIND_BACKGROUND) {
            cJSON_AddItemToArray(bg, slot_json(slot));
        }
    }
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, json);
}

static bool valid_id(const char *id)
{
    size_t n = id ? strlen(id) : 0;
    if (n == 0 || n >= sizeof(s_rec.profile)) {
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

static esp_err_t record_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *kind = GW_JSON_STR(body, "kind"), *profile = GW_JSON_STR(body, "profile"),
               *type = GW_JSON_STR(body, "type");
    bool background = kind && strcmp(kind, "background") == 0;
    uint8_t t = type && strcmp(type, "tone") == 0 ? TYPE_TONE : TYPE_EMBEDDING;
    const char *error = NULL;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int existing = background ? -1 : profile_find(profile ? profile : "");
    if (!s_hooks.ready || !s_hooks.ready()) {
        error = "sound recognition must run (microphone, model and recognition on)";
    } else if (s_rec.active) {
        error = "a recording is running";
    } else if (!background && !valid_id(profile)) {
        error = "profile: a-z, 0-9, _ (max 23)";
    } else if (existing >= 0 && s_prof[existing].type != t) {
        error = "profile exists with the other type";
    } else if (free_slots() < (background ? BACKGROUND_WINDOWS : 1)) {
        error = "no free sample slot, delete some";
    }
    if (!error) {
        memset(s_rec.result, 0, sizeof(s_rec.result));
        strlcpy(s_rec.profile, background ? BACKGROUND : profile, sizeof(s_rec.profile));
        s_rec.kind = background ? KIND_BACKGROUND : KIND_SAMPLE;
        s_rec.type = t;
        s_rec.until_us = esp_timer_get_time() + SAMPLE_WAIT_US;
        s_rec.best_level = -200;
        s_rec.windows = s_rec.saved = 0;
        s_rec.ok = false;
        s_rec.active = true;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(body);
    if (error) {
        return gw_http_send_error(req, 409, error);
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "seconds", background ? BACKGROUND_WINDOWS : SAMPLE_WAIT_US / 1000000);
    return gw_http_send_json(req, json);
}

static bool query_value(httpd_req_t *req, const char *key, char *out, size_t len)
{
    char query[64];
    return httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
           httpd_query_key_value(query, key, out, len) == ESP_OK;
}

/* ?slot=n, ?profile=id (all its samples) or ?profile=_background */
static esp_err_t learn_delete(httpd_req_t *req)
{
    char value[32];
    int deleted = 0;
    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rec.active) {
        xSemaphoreGive(s_lock);
        return gw_http_send_error(req, 409, "a recording is running");
    }
    if (query_value(req, "slot", value, sizeof(value))) {
        int slot = atoi(value);
        if (slot >= 0 && slot < s_slot_count && slot_used(slot)) {
            err = erase_slot(slot);
            deleted = 1;
        }
    } else if (query_value(req, "profile", value, sizeof(value))) {
        for (int slot = 0; slot < s_slot_count && err == ESP_OK; slot++) {
            if (slot_used(slot) && strcmp(s_slots[slot].profile, value) == 0) {
                err = erase_slot(slot);
                deleted++;
            }
        }
    }
    rebuild();
    xSemaphoreGive(s_lock);
    if (err != ESP_OK) {
        return gw_http_send_error(req, 500, esp_err_to_name(err));
    }
    if (!deleted) {
        return gw_http_send_error(req, 404, "no such sample");
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "deleted", deleted);
    return gw_http_send_json(req, json);
}

static esp_err_t sample_get(httpd_req_t *req)
{
    char value[8];
    int slot = query_value(req, "slot", value, sizeof(value)) ? atoi(value) : -1;
    if (slot < 0 || slot >= s_slot_count || !slot_used(slot)) {
        return gw_http_send_error(req, 404, "no such sample");
    }
    int16_t *audio = heap_caps_malloc(LEARN_AUDIO * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    if (!audio || read_audio(slot, audio) != ESP_OK) {
        free(audio);
        return gw_http_send_error(req, 500, "read failed");
    }
    const uint32_t n = LEARN_AUDIO;
    struct __attribute__((packed)) {
        char riff[4];
        uint32_t size;
        char wave[4], fmt[4];
        uint32_t fmt_size;
        uint16_t format, channels;
        uint32_t rate, byte_rate;
        uint16_t align, bits;
        char data[4];
        uint32_t data_size;
    } hdr = {{'R', 'I', 'F', 'F'}, 36 + n * 2, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16, 1, 1,
             MEL_SAMPLE_RATE, MEL_SAMPLE_RATE * 2, 2, 16, {'d', 'a', 't', 'a'}, n * 2};
    httpd_resp_set_type(req, "audio/wav");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)&hdr, sizeof(hdr));
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, (const char *)audio, n * sizeof(int16_t));
    }
    free(audio);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

/* ---------------------------------------------------------------- uploaded recordings */

static float window_db(const int16_t *a)
{
    double e = 0;
    for (int i = 0; i < LEARN_AUDIO; i++) {
        e += (double)a[i] * a[i];
    }
    double rms = sqrt(e / LEARN_AUDIO) / 32768.0;
    return rms > 1e-6 ? 20.0f * log10f((float)rms) : -120.0f;
}

/* Inference task: windows of the upload 0.48 s apart (non-overlapping for background). */
void sound_learn_poll(void)
{
    if (!s_job.pending) {
        return;
    }
    int hop = MEL_PATCH_HOP * MEL_HOP;
    int windows = s_job.n >= LEARN_AUDIO ? (s_job.n - LEARN_AUDIO) / hop + 1 : 0;
    float *emb = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
    esp_err_t err = emb && windows ? ESP_OK : windows ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_SIZE;
    snprintf(s_job.msg, sizeof(s_job.msg), "%s", windows ? "" : "recording shorter than 0.96 s");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (err == ESP_OK && s_job.test) {
        /* highest score of every profile over the windows */
        cJSON *res = cJSON_CreateObject();
        cJSON_AddNumberToObject(res, "windows", windows);
        cJSON *scores = cJSON_AddObjectToObject(res, "scores");
        float best[MAX_PROFILES];
        for (int i = 0; i < s_prof_count; i++) {
            best[i] = 0;
        }
        for (int w = 0; w < windows && err == ESP_OK; w++) {
            const int16_t *a = s_job.audio + w * hop;
            err = s_hooks.embed(a, emb);
            for (int i = 0; i < s_prof_count && err == ESP_OK; i++) {
                const profile_t *p = &s_prof[i];
                float sc = p->type == TYPE_TONE
                               ? (p->tone_ratio > 0 ? tone_ratio(a, LEARN_AUDIO, p->tone_hz) / p->tone_ratio : 0)
                               : (p->centroid ? cosine(emb, p->centroid) : 0);
                best[i] = fmaxf(best[i], sc > 1 ? 1 : sc);
            }
        }
        for (int i = 0; i < s_prof_count; i++) {
            cJSON_AddNumberToObject(scores, s_prof[i].id, round((double)best[i] * 100) / 100);
        }
        s_job.result = res;
    } else if (err == ESP_OK && s_job.kind == KIND_SAMPLE) {
        /* the loudest window is the sample */
        int loudest = 0;
        float level = -200;
        for (int w = 0; w < windows; w++) {
            float db = window_db(s_job.audio + w * hop);
            if (db > level) {
                level = db;
                loudest = w;
            }
        }
        memcpy(s_rec.audio, s_job.audio + loudest * hop, LEARN_AUDIO * sizeof(int16_t));
        err = s_hooks.embed(s_rec.audio, s_rec.emb);
        if (err == ESP_OK) {
            strlcpy(s_rec.profile, s_job.profile, sizeof(s_rec.profile));
            s_rec.kind = KIND_SAMPLE;
            s_rec.type = s_job.type;
            err = save_slot(level);
            snprintf(s_job.msg, sizeof(s_job.msg), "sample saved (%.1f dBFS)", level);
        }
    } else if (err == ESP_OK) {
        s_rec.kind = KIND_BACKGROUND;
        int saved = 0;
        for (int w = 0; w < windows && saved < BACKGROUND_WINDOWS && err == ESP_OK; w += 2) {
            memcpy(s_rec.audio, s_job.audio + w * hop, LEARN_AUDIO * sizeof(int16_t));
            err = s_hooks.embed(s_rec.audio, s_rec.emb);
            if (err == ESP_OK && (err = save_slot(window_db(s_rec.audio))) == ESP_OK) {
                saved++;
            }
        }
        snprintf(s_job.msg, sizeof(s_job.msg), "%d background recordings saved", saved);
    }
    if (!s_job.test) {
        rebuild();
    }
    xSemaphoreGive(s_lock);
    free(emb);
    free(s_job.audio);
    s_job.audio = NULL;
    s_job.err = err;
    if (s_job.abandoned) {
        cJSON_Delete(s_job.result);
        s_job.result = NULL;
    }
    s_job.pending = false;
    xSemaphoreGive(s_job.done);
}

/* Fills s_job.audio from a 16 kHz mono 16-bit WAV. */
static esp_err_t read_wav(httpd_req_t *req, char *err, size_t err_len)
{
    size_t len = req->content_len;
    if (len < 44 || len > 44 + 1024 + UPLOAD_MAX_S * MEL_SAMPLE_RATE * 2) {
        snprintf(err, err_len, "WAV of 1 to %d s expected", UPLOAD_MAX_S);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *buf = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    for (size_t got = 0; got < len;) {
        int n = httpd_req_recv(req, (char *)buf + got, len - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            continue;
        }
        if (n <= 0) {
            free(buf);
            return ESP_FAIL;
        }
        got += n;
    }
    esp_err_t ret = ESP_ERR_INVALID_ARG;
    snprintf(err, err_len, "16 kHz mono 16-bit PCM WAV expected");
    if (memcmp(buf, "RIFF", 4) == 0 && memcmp(buf + 8, "WAVE", 4) == 0) {
        bool fmt_ok = false;
        for (size_t off = 12; off + 8 <= len;) {
            uint32_t size;
            memcpy(&size, buf + off + 4, 4);
            if (memcmp(buf + off, "fmt ", 4) == 0 && size >= 16) {
                uint16_t format, channels, bits;
                uint32_t rate;
                memcpy(&format, buf + off + 8, 2);
                memcpy(&channels, buf + off + 10, 2);
                memcpy(&rate, buf + off + 12, 4);
                memcpy(&bits, buf + off + 22, 2);
                fmt_ok = format == 1 && channels == 1 && rate == MEL_SAMPLE_RATE && bits == 16;
            } else if (memcmp(buf + off, "data", 4) == 0 && fmt_ok) {
                size_t n = (size > len - off - 8 ? len - off - 8 : size) / 2;
                s_job.audio = heap_caps_malloc(n * sizeof(int16_t), MALLOC_CAP_SPIRAM);
                if (s_job.audio) {
                    memcpy(s_job.audio, buf + off + 8, n * sizeof(int16_t));
                    s_job.n = n;
                    ret = ESP_OK;
                } else {
                    ret = ESP_ERR_NO_MEM;
                }
                break;
            }
            off += 8 + size + (size & 1);
        }
    }
    free(buf);
    return ret;
}

/* ?profile=id&type=embedding|tone&kind=sample|background, or ?test=1: body WAV */
static esp_err_t upload_post(httpd_req_t *req)
{
    char value[24] = "", err[96] = "";
    bool test = query_value(req, "test", value, sizeof(value)) && strcmp(value, "1") == 0;
    bool background = query_value(req, "kind", value, sizeof(value)) && strcmp(value, "background") == 0;
    uint8_t type = query_value(req, "type", value, sizeof(value)) && strcmp(value, "tone") == 0 ? TYPE_TONE : TYPE_EMBEDDING;
    char profile[24] = "";
    query_value(req, "profile", profile, sizeof(profile));

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int existing = profile_find(profile);
    const char *error = NULL;
    if (!s_hooks.ready || !s_hooks.ready()) {
        error = "sound recognition must run (microphone, model and recognition on)";
    } else if (s_rec.active || s_job.pending) {
        error = "a recording is running";
    } else if (!test && !background && !valid_id(profile)) {
        error = "profile: a-z, 0-9, _ (max 23)";
    } else if (!test && existing >= 0 && s_prof[existing].type != type) {
        error = "profile exists with the other type";
    } else if (!test && free_slots() < (background ? BACKGROUND_WINDOWS : 1)) {
        error = "no free sample slot, delete some";
    }
    xSemaphoreGive(s_lock);
    if (error) {
        return gw_http_send_error(req, 409, error);
    }
    esp_err_t ret = read_wav(req, err, sizeof(err));
    if (ret != ESP_OK) {
        return gw_http_send_error(req, ret == ESP_ERR_NO_MEM ? 500 : 400, err[0] ? err : esp_err_to_name(ret));
    }
    s_job.test = test;
    s_job.kind = background ? KIND_BACKGROUND : KIND_SAMPLE;
    s_job.type = type;
    strlcpy(s_job.profile, profile, sizeof(s_job.profile));
    s_job.result = NULL;
    s_job.abandoned = false;
    xSemaphoreTake(s_job.done, 0);
    s_job.pending = true;
    if (xSemaphoreTake(s_job.done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        s_job.abandoned = true; /* the inference task still frees the recording */
        return gw_http_send_error(req, 503, "recognition did not process the recording");
    }
    if (s_job.err != ESP_OK) {
        cJSON_Delete(s_job.result);
        return gw_http_send_error(req, 400, s_job.msg[0] ? s_job.msg : esp_err_to_name(s_job.err));
    }
    cJSON *json = s_job.result ? s_job.result : cJSON_CreateObject();
    if (!s_job.test) {
        cJSON_AddStringToObject(json, "result", s_job.msg);
    }
    return gw_http_send_json(req, json);
}

esp_err_t sound_learn_init(const sound_learn_hooks_t *hooks)
{
    s_hooks = *hooks;
    s_job.done = xSemaphoreCreateBinary();
    s_part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, PART_SUBTYPE, NULL);
    ESP_RETURN_ON_FALSE(s_part, ESP_ERR_NOT_FOUND, TAG, "no sounds partition");
    s_slot_count = s_part->size / SLOT_SIZE;
    s_slot_count = s_slot_count > MAX_SLOTS ? MAX_SLOTS : s_slot_count;
    s_rec.emb = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
    s_rec.audio = heap_caps_malloc(LEARN_AUDIO * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_rec.emb && s_rec.audio, ESP_ERR_NO_MEM, TAG, "no mem");
    s_lock = xSemaphoreCreateMutex();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    scan();
    rebuild();
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "%d profiles, %d of %d slots free", s_prof_count, free_slots(), s_slot_count);
    gw_http_register("/api/sound/learn", HTTP_GET, learn_get);
    gw_http_register("/api/sound/learn", HTTP_DELETE, learn_delete);
    gw_http_register("/api/sound/learn/record", HTTP_POST, record_post);
    gw_http_register("/api/sound/learn/sample", HTTP_GET, sample_get);
    gw_http_register("/api/sound/learn/upload", HTTP_POST, upload_post);
    return ESP_OK;
}
