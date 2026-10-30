/*
 * Sound service: onboard microphone (ES8311 over I2S) -> level meter and YAMNet sound
 * classification. Off by default and independent of the gateway role. Capture and inference
 * run on core 1 below the network tasks; a CPU guard stops inference if it costs too much.
 */
#include "sound.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "sound_aweight.h"
#include "sound_events.h"
#include "sound_learn.h"
#include "sound_mel.h"
#include "sound_model.h"

static const char *TAG = "sound";

#define PIN_I2C_SDA 7
#define PIN_I2C_SCL 8
#define PIN_I2S_MCLK 13
#define PIN_I2S_BCLK 12
#define PIN_I2S_WS 10
#define PIN_I2S_DIN 48 /* ES8311 ASDOUT */

#define KEY_ENABLED "snd_en"
#define KEY_GAIN "snd_gain"
#define KEY_INFER "snd_ai"
#define KEY_GATE "snd_gate"
#define KEY_SPL "snd_spl" /* calibration: dB SPL = dBFS(A) + offset - gain */

#define RATE MEL_SAMPLE_RATE
#define BLOCK 320                 /* 20 ms */
#define RING_SECONDS 10
#define RING (RATE * RING_SECONDS) /* int16 mono samples */
#define LEVEL_BLOCKS 5            /* level over 100 ms */
/* Sound pressure: without a calibration a typical analog microphone (-42 dBV/Pa) into the ES8311
 * (full scale about -1 dBV) gives 94 dB SPL at -41 dBFS with no gain. */
#define SPL_OFFSET_DEFAULT 135.0f
#define SPL_WINDOW_LEVELS 600 /* LAeq and LAmax over 60 s of 100 ms levels */
#define SPL_SLOW 0.0952f       /* "slow" time weighting (1 s) per 100 ms level: 1 - exp(-0.1) */
#define TOP_N 5
#define CORE 1
#define CAPTURE_PRIO 6
#define INFER_PRIO 2
#define CPU_LIMIT_PCT 60          /* of one core, averaged over CPU_WINDOW_US */
#define CPU_WINDOW_US (30 * 1000000LL)
#define MAX_LAG_SAMPLES RATE      /* older audio is dropped instead of caught up */

extern const char class_map_start[] asm("_binary_yamnet_class_map_csv_start");
extern const char class_map_end[] asm("_binary_yamnet_class_map_csv_end");

typedef struct {
    int index;
    float score;
} top_t;

static struct {
    bool running;
    char error[96];
    int gain_db;
    int gate_db;
    bool infer_enabled;
    /* capture */
    int16_t *ring;
    uint64_t total; /* samples written, read with samples_total() */
    float level_db, peak_db, floor_db, gate_level_db;
    uint32_t read_errors;
    /* sound pressure, A-weighted levels in dBFS (spl() makes dB of them) */
    float spl_offset;
    bool spl_calibrated;
    float las_ms;               /* slow mean square */
    float win_ms;               /* sum of the mean squares in the current window (32-bit: read from other tasks) */
    int win_levels;
    float win_max_db;
    bool win_overload;
    float laeq_db, lamax_db;    /* last whole window */
    bool laeq_valid, laeq_overload;
    int64_t laeq_us;
    /* inference */
    char model_error[96];
    uint32_t model_load_ms;
    uint32_t runs, skipped, late, flash_skips;
    float last_ms, avg_ms, max_ms;
    float cpu_pct;
    top_t top[TOP_N];
    int64_t top_us;
} s;

static SemaphoreHandle_t s_lock;
static portMUX_TYPE s_total_mux = portMUX_INITIALIZER_UNLOCKED;

/* 64-bit counter: the capture task can preempt a reader halfway through a plain load. */
static uint64_t samples_total(void)
{
    portENTER_CRITICAL(&s_total_mux);
    uint64_t t = s.total;
    portEXIT_CRITICAL(&s_total_mux);
    return t;
}
static esp_codec_dev_handle_t s_codec;
static const char **s_names;

/* ---------------------------------------------------------------- class names */

static void load_class_names(void)
{
    /* yamnet_class_map.csv: index,mid,display_name (quoted when it contains commas) */
    size_t len = class_map_end - class_map_start;
    char *text = heap_caps_malloc(len + 1, MALLOC_CAP_SPIRAM);
    s_names = heap_caps_calloc(SOUND_CLASSES, sizeof(char *), MALLOC_CAP_SPIRAM);
    if (!text || !s_names) {
        return;
    }
    memcpy(text, class_map_start, len);
    text[len] = '\0';
    char *line = strchr(text, '\n');
    for (int i = 0; line && i < SOUND_CLASSES; i++) {
        line++;
        char *end = strchr(line, '\n');
        if (end) {
            *end = '\0';
            if (end > line && end[-1] == '\r') {
                end[-1] = '\0';
            }
        }
        char *name = strchr(line, ',');
        name = name ? strchr(name + 1, ',') : NULL;
        if (name) {
            name++;
            if (*name == '"') {
                name++;
                char *q = strrchr(name, '"');
                if (q) {
                    *q = '\0';
                }
            }
            s_names[i] = name;
        }
        line = end;
    }
}

static const char *class_name(int i)
{
    return s_names && i >= 0 && i < SOUND_CLASSES && s_names[i] ? s_names[i] : "?";
}

/* ---------------------------------------------------------------- audio input */

static esp_err_t audio_open(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = PIN_I2C_SDA,
        .scl_io_num = PIN_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_cfg, &bus), TAG, "I2C bus");

    i2s_chan_handle_t rx;
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = 6;
    chan_cfg.dma_frame_num = BLOCK;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, NULL, &rx), TAG, "I2S channel");
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = PIN_I2S_MCLK,
            .bclk = PIN_I2S_BCLK,
            .ws = PIN_I2S_WS,
            .dout = I2S_GPIO_UNUSED,
            .din = PIN_I2S_DIN,
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(rx, &std_cfg), TAG, "I2S std mode");

    audio_codec_i2s_cfg_t i2s_cfg = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = NULL};
    audio_codec_i2c_cfg_t i2c_cfg = {.port = 0, .addr = ES8311_CODEC_DEFAULT_ADDR, .bus_handle = bus};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    es8311_codec_cfg_t es_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_ADC,
        .pa_pin = -1,
        .use_mclk = true,
        .no_dac_ref = true,
    };
    const audio_codec_if_t *codec_if = es8311_codec_new(&es_cfg);
    ESP_RETURN_ON_FALSE(data_if && ctrl_if && codec_if, ESP_FAIL, TAG, "ES8311 not found on I2C");

    esp_codec_dev_cfg_t dev_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = codec_if, .data_if = data_if};
    s_codec = esp_codec_dev_new(&dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "codec device");
    esp_codec_dev_sample_info_t fs = {.sample_rate = RATE, .channel = 2, .bits_per_sample = 16};
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &fs) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "codec open");
    esp_codec_dev_set_in_gain(s_codec, s.gain_db);
    return ESP_OK;
}

/* dB SPL (A-weighted) of an A-weighted level in dBFS */
static float spl(float dbfs)
{
    return dbfs + s.spl_offset - s.gain_db;
}

static float to_db(float rms)
{
    return rms > 1e-6f ? 20.0f * log10f(rms) : -120.0f;
}

static void capture_task(void *arg)
{
    int16_t *stereo = heap_caps_malloc(BLOCK * 2 * sizeof(int16_t), MALLOC_CAP_INTERNAL);
    double acc = 0, acc_a = 0;
    int blocks = 0;
    float peak = 0;
    aweight_t aw;
    aweight_reset(&aw);
    int settle = 10; /* levels (1 s) left out of the sound pressure after the start */
    s.win_max_db = -120.0f;

    while (true) {
        if (esp_codec_dev_read(s_codec, stereo, BLOCK * 2 * sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
            s.read_errors++;
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        uint32_t pos = s.total % RING; /* only this task writes s.total */
        for (int i = 0; i < BLOCK; i++) {
            int16_t v = stereo[2 * i]; /* microphone on the left slot */
            s.ring[(pos + i) % RING] = v;
            float f = v / 32768.0f;
            acc += f * f;
            peak = fmaxf(peak, fabsf(f));
            float a = aweight_step(&aw, f);
            acc_a += a * a;
        }
        portENTER_CRITICAL(&s_total_mux);
        s.total += BLOCK;
        portEXIT_CRITICAL(&s_total_mux);
        if (++blocks == LEVEL_BLOCKS) {
            float db = to_db(sqrtf(acc / (BLOCK * LEVEL_BLOCKS)));
            s.level_db = db;
            s.peak_db = to_db(peak);
            /* Noise floor: follows quiet levels fast and loud ones slowly. */
            s.floor_db = db < s.floor_db ? db : s.floor_db + 0.01f * (db - s.floor_db);

            float ms = acc_a / (BLOCK * LEVEL_BLOCKS);
            s.las_ms += SPL_SLOW * (ms - s.las_ms);
            if (settle > 0) { /* codec start click and the filter settling */
                settle--;
                s.las_ms = ms;
                acc = 0;
                acc_a = 0;
                blocks = 0;
                peak = 0;
                continue;
            }
            s.win_ms += ms;
            s.win_max_db = fmaxf(s.win_max_db, to_db(sqrtf(ms)));
            s.win_overload |= peak >= 0.99f;
            if (++s.win_levels == SPL_WINDOW_LEVELS) {
                s.laeq_db = to_db(sqrtf(s.win_ms / s.win_levels));
                s.lamax_db = s.win_max_db;
                s.laeq_overload = s.win_overload;
                s.laeq_valid = true;
                s.laeq_us = esp_timer_get_time();
                s.win_ms = 0;
                s.win_levels = 0;
                s.win_max_db = -120.0f;
                s.win_overload = false;
            }
            acc = 0;
            acc_a = 0;
            blocks = 0;
            peak = 0;
        }
    }
}

/* ---------------------------------------------------------------- inference */

static void infer_task(void *arg)
{
    float *history = heap_caps_malloc(MEL_PATCH_FRAMES * MEL_BANDS * sizeof(float), MALLOC_CAP_SPIRAM);
    float *patch = heap_caps_malloc(MEL_PATCH_FRAMES * MEL_BANDS * sizeof(float), MALLOC_CAP_SPIRAM);
    float *scores = heap_caps_malloc(SOUND_CLASSES * sizeof(float), MALLOC_CAP_SPIRAM);
    float *embedding = heap_caps_malloc(SOUND_EMBEDDING * sizeof(float), MALLOC_CAP_SPIRAM);
    int16_t *audio = heap_caps_malloc(LEARN_AUDIO * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    float frame[MEL_WINDOW];
    uint64_t next_frame = 0;
    uint32_t frames = 0, since_patch = 0;
    int64_t window_start = esp_timer_get_time(), busy_us = 0;
    float window_peak_db = -120.0f;

    int64_t t0 = esp_timer_get_time();
    esp_err_t err = sound_model_load();
    s.model_load_ms = (esp_timer_get_time() - t0) / 1000;
    if (err != ESP_OK) {
        snprintf(s.model_error, sizeof(s.model_error), "model not loaded: %s", esp_err_to_name(err));
        ESP_LOGW(TAG, "%s", s.model_error);
    } else {
        ESP_LOGI(TAG, "YAMNet loaded in %lu ms", (unsigned long)s.model_load_ms);
    }
    next_frame = samples_total() / MEL_HOP;

    while (true) {
        sound_learn_poll();
        uint64_t total = samples_total();
        if (total - next_frame * MEL_HOP > MAX_LAG_SAMPLES + MEL_PATCH_FRAMES * MEL_HOP) {
            /* Behind real time (flash writes stall both cores): rebuild the history from the
             * newest audio instead of catching up back-to-back, which would starve IDLE1. */
            next_frame = total / MEL_HOP - MEL_PATCH_FRAMES;
            frames = since_patch = 0;
            s.late++;
        }
        if (next_frame * MEL_HOP + MEL_WINDOW > total) {
            vTaskDelay(pdMS_TO_TICKS(40));
            continue;
        }
        int64_t start = esp_timer_get_time();
        uint64_t base = next_frame * MEL_HOP;
        for (int i = 0; i < MEL_WINDOW; i++) {
            frame[i] = s.ring[(base + i) % RING] / 32768.0f;
        }
        memmove(history, history + MEL_BANDS, (MEL_PATCH_FRAMES - 1) * MEL_BANDS * sizeof(float));
        mel_frame(frame, history + (MEL_PATCH_FRAMES - 1) * MEL_BANDS);
        next_frame++;
        frames++;
        since_patch++;
        window_peak_db = fmaxf(window_peak_db, s.level_db);
        busy_us += esp_timer_get_time() - start;
        if (frames % 16 == 0) {
            vTaskDelay(1); /* keep IDLE1 alive while catching up */
        }

        if (frames < MEL_PATCH_FRAMES || since_patch < MEL_PATCH_HOP) {
            continue;
        }
        /* Let the idle task (task watchdog) run between patches whatever the backlog. */
        vTaskDelay(1);
        since_patch = 0;
        bool loud = window_peak_db >= s.gate_db || sound_learn_wants_all();
        window_peak_db = -120.0f;
        if (!s.infer_enabled || !sound_model_ready() || !loud) {
            s.skipped++;
            sound_events_update(NULL, s.level_db, esp_timer_get_time());
            continue;
        }
        if (!gw_flash_try_lock()) {
            /* an upload or C6 flashing is writing the P4 flash, see gw_flash_lock() */
            s.flash_skips++;
            sound_events_update(NULL, s.level_db, esp_timer_get_time());
            continue;
        }
        memcpy(patch, history, MEL_PATCH_FRAMES * MEL_BANDS * sizeof(float));
        start = esp_timer_get_time();
        sound_model_run(patch, scores, embedding);
        gw_flash_unlock();
        int64_t took = esp_timer_get_time() - start;
        busy_us += took;

        /* the audio the patch was computed from, for learned sounds */
        uint64_t first = (next_frame - MEL_PATCH_FRAMES) * MEL_HOP;
        double energy = 0;
        for (int i = 0; i < LEARN_AUDIO; i++) {
            audio[i] = s.ring[(first + i) % RING];
            energy += (double)audio[i] * audio[i];
        }
        sound_learn_window(embedding, audio, to_db(sqrt(energy / LEARN_AUDIO) / 32768.0), esp_timer_get_time());

        top_t top[TOP_N] = {0};
        for (int c = 0; c < SOUND_CLASSES; c++) {
            for (int k = 0; k < TOP_N; k++) {
                if (scores[c] > top[k].score) {
                    memmove(&top[k + 1], &top[k], (TOP_N - k - 1) * sizeof(top_t));
                    top[k] = (top_t){c, scores[c]};
                    break;
                }
            }
        }
        sound_events_update(scores, s.level_db, esp_timer_get_time());
        xSemaphoreTake(s_lock, portMAX_DELAY);
        memcpy(s.top, top, sizeof(top));
        s.top_us = esp_timer_get_time();
        s.runs++;
        s.last_ms = took / 1000.0f;
        s.avg_ms = s.runs == 1 ? s.last_ms : s.avg_ms + 0.05f * (s.last_ms - s.avg_ms);
        s.max_ms = fmaxf(s.max_ms, s.last_ms);
        xSemaphoreGive(s_lock);

        int64_t now = esp_timer_get_time();
        if (now - window_start >= CPU_WINDOW_US) {
            s.cpu_pct = 100.0f * busy_us / (now - window_start);
            window_start = now;
            busy_us = 0;
            if (s.cpu_pct > CPU_LIMIT_PCT) {
                s.infer_enabled = false;
                snprintf(s.model_error, sizeof(s.model_error), "inference stopped: %.0f %% CPU over %d %% limit",
                         s.cpu_pct, CPU_LIMIT_PCT);
                ESP_LOGE(TAG, "%s", s.model_error);
            }
        }
    }
}

/* ---------------------------------------------------------------- service */

static esp_err_t start(void)
{
    s.ring = heap_caps_calloc(RING, sizeof(int16_t), MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s.ring, ESP_ERR_NO_MEM, TAG, "ring");
    s.floor_db = 0;
    mel_init();
    ESP_RETURN_ON_ERROR(audio_open(), TAG, "audio");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(capture_task, "snd_cap", 4096, NULL, CAPTURE_PRIO, NULL, CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "capture task");
    ESP_RETURN_ON_FALSE(xTaskCreatePinnedToCore(infer_task, "snd_ai", 12288, NULL, INFER_PRIO, NULL, CORE) == pdPASS,
                        ESP_ERR_NO_MEM, TAG, "inference task");
    s.running = true;
    ESP_LOGI(TAG, "microphone running, gain %d dB", s.gain_db);
    return ESP_OK;
}

/* ---------------------------------------------------------------- API */

/* LAeq and LAmax (dBFS) of the last whole window; until there is one, of the window so far. */
static void spl_window(float *laeq, float *lamax, bool *overload)
{
    if (s.laeq_valid) {
        *laeq = s.laeq_db;
        *lamax = s.lamax_db;
        *overload = s.laeq_overload;
    } else {
        int n = s.win_levels;
        *laeq = n ? to_db(sqrtf(s.win_ms / n)) : to_db(sqrtf(s.las_ms));
        *lamax = n ? s.win_max_db : *laeq;
        *overload = s.win_overload;
    }
}

static void spl_load(void)
{
    char text[16];
    s.spl_offset = SPL_OFFSET_DEFAULT;
    s.spl_calibrated = false;
    if (gw_config_get_str(KEY_SPL, text, sizeof(text)) && text[0]) {
        s.spl_offset = strtof(text, NULL);
        s.spl_calibrated = true;
    }
}

static cJSON *status_json(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "enabled", gw_config_get_u8(KEY_ENABLED, 0));
    cJSON_AddBoolToObject(json, "running", s.running);
    cJSON_AddStringToObject(json, "error", s.error);
    cJSON_AddNumberToObject(json, "gain_db", s.gain_db);
    cJSON_AddNumberToObject(json, "gate_db", s.gate_db);
    if (s.running) {
        cJSON_AddNumberToObject(json, "level_db", round((double)(s.level_db) * 10) / 10.0);
        cJSON_AddNumberToObject(json, "peak_db", round((double)(s.peak_db) * 10) / 10.0);
        cJSON_AddNumberToObject(json, "floor_db", round((double)(s.floor_db) * 10) / 10.0);
        cJSON_AddNumberToObject(json, "seconds", samples_total() / (double)RATE);
        cJSON_AddNumberToObject(json, "read_errors", s.read_errors);
        cJSON *p = cJSON_AddObjectToObject(json, "spl");
        float laeq, lamax;
        bool overload;
        spl_window(&laeq, &lamax, &overload);
        cJSON_AddNumberToObject(p, "las", round((double)spl(to_db(sqrtf(s.las_ms))) * 10) / 10.0);
        cJSON_AddNumberToObject(p, "laeq", round((double)spl(laeq) * 10) / 10.0);
        cJSON_AddNumberToObject(p, "lamax", round((double)spl(lamax) * 10) / 10.0);
        cJSON_AddBoolToObject(p, "overload", overload);
        cJSON_AddNumberToObject(p, "window_s", SPL_WINDOW_LEVELS / 10);
        cJSON_AddBoolToObject(p, "partial", !s.laeq_valid);
        cJSON_AddNumberToObject(p, "offset", round((double)s.spl_offset * 10) / 10.0);
        cJSON_AddBoolToObject(p, "calibrated", s.spl_calibrated);
    }
    cJSON *model = cJSON_AddObjectToObject(json, "model");
    cJSON_AddNumberToObject(model, "stored_bytes", sound_model_stored_size());
    cJSON_AddBoolToObject(model, "loaded", sound_model_ready());
    cJSON_AddNumberToObject(model, "load_ms", s.model_load_ms);
    cJSON_AddStringToObject(model, "error", s.model_error);
    cJSON *inf = cJSON_AddObjectToObject(json, "inference");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(inf, "enabled", s.infer_enabled);
    cJSON_AddNumberToObject(inf, "runs", s.runs);
    cJSON_AddNumberToObject(inf, "skipped_quiet", s.skipped);
    cJSON_AddNumberToObject(inf, "late_resyncs", s.late);
    cJSON_AddNumberToObject(inf, "flash_skips", s.flash_skips);
    cJSON_AddNumberToObject(inf, "last_ms", round((double)(s.last_ms) * 10) / 10.0);
    cJSON_AddNumberToObject(inf, "avg_ms", round((double)(s.avg_ms) * 10) / 10.0);
    cJSON_AddNumberToObject(inf, "max_ms", round((double)(s.max_ms) * 10) / 10.0);
    cJSON_AddNumberToObject(inf, "cpu_pct", round((double)(s.cpu_pct) * 10) / 10.0);
    if (s.top_us) {
        cJSON_AddNumberToObject(inf, "top_age_s", (esp_timer_get_time() - s.top_us) / 1e6);
        cJSON *top = cJSON_AddArrayToObject(inf, "top");
        for (int k = 0; k < TOP_N; k++) {
            cJSON *t = cJSON_CreateObject();
            cJSON_AddNumberToObject(t, "index", s.top[k].index);
            cJSON_AddStringToObject(t, "name", class_name(s.top[k].index));
            cJSON_AddNumberToObject(t, "score", round((double)(s.top[k].score) * 1000) / 1000.0);
            cJSON_AddItemToArray(top, t);
        }
    }
    xSemaphoreGive(s_lock);
    return json;
}

static esp_err_t status_get(httpd_req_t *req)
{
    return gw_http_send_json(req, status_json());
}

static esp_err_t config_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    bool restart = false;
    cJSON *v;
    if (cJSON_IsBool(v = cJSON_GetObjectItem(body, "enabled"))) {
        bool en = cJSON_IsTrue(v);
        restart = en != (bool)gw_config_get_u8(KEY_ENABLED, 0) && !(en && s.running);
        gw_config_set_u8(KEY_ENABLED, en);
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "gain_db"))) {
        s.gain_db = v->valueint < 0 ? 0 : v->valueint > 42 ? 42 : v->valueint;
        gw_config_set_u8(KEY_GAIN, s.gain_db);
        if (s_codec) {
            esp_codec_dev_set_in_gain(s_codec, s.gain_db);
        }
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "gate_db"))) {
        s.gate_db = v->valueint < -90 ? -90 : v->valueint > 0 ? 0 : v->valueint;
        gw_config_set_u8(KEY_GATE, (uint8_t)(-s.gate_db));
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(body, "inference"))) {
        s.infer_enabled = cJSON_IsTrue(v);
        gw_config_set_u8(KEY_INFER, s.infer_enabled);
        if (s.infer_enabled) {
            s.model_error[0] = '\0';
        }
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "spl_reference"))) {
        /* the reading of a sound level meter (dB(A), slow) next to the gateway right now */
        if (!s.running || s.las_ms <= 0) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 409, "microphone is off");
        }
        if (v->valuedouble < 20 || v->valuedouble > 130) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 400, "reference 20-130 dB(A)");
        }
        char text[16];
        float offset = (float)v->valuedouble - to_db(sqrtf(s.las_ms)) + s.gain_db;
        snprintf(text, sizeof(text), "%.1f", (double)offset);
        gw_config_set_str(KEY_SPL, text);
        spl_load();
        ESP_LOGI(TAG, "sound pressure calibrated: %.1f dB(A) at offset %s", v->valuedouble, text);
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "spl_offset"))) {
        if (v->valuedouble < 60 || v->valuedouble > 220) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 400, "offset 60-220 dB");
        }
        char text[16];
        snprintf(text, sizeof(text), "%.1f", v->valuedouble);
        gw_config_set_str(KEY_SPL, text);
        spl_load();
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "spl_reset"))) {
        gw_config_erase(KEY_SPL);
        spl_load();
    }
    cJSON_Delete(body);
    cJSON *json = status_json();
    /* Turning the microphone off frees the audio pipeline only on restart. */
    cJSON_AddBoolToObject(json, "restart_required", restart);
    return gw_http_send_json(req, json);
}

static esp_err_t clip_get(httpd_req_t *req)
{
    if (!s.running) {
        return gw_http_send_error(req, 409, "microphone is off");
    }
    char query[32], value[8];
    int seconds = 5;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "seconds", value, sizeof(value)) == ESP_OK) {
        seconds = atoi(value);
    }
    seconds = seconds < 1 ? 1 : seconds > RING_SECONDS - 1 ? RING_SECONDS - 1 : seconds;
    uint32_t n = seconds * RATE;
    uint64_t from = samples_total() - n;
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
    } hdr = {{'R', 'I', 'F', 'F'}, 36 + n * 2, {'W', 'A', 'V', 'E'}, {'f', 'm', 't', ' '}, 16, 1, 1, RATE, RATE * 2, 2,
             16, {'d', 'a', 't', 'a'}, n * 2};
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"p4gw_clip.wav\"");
    esp_err_t err = httpd_resp_send_chunk(req, (const char *)&hdr, sizeof(hdr));
    int16_t *buf = heap_caps_malloc(RATE * sizeof(int16_t), MALLOC_CAP_SPIRAM);
    for (uint32_t off = 0; buf && err == ESP_OK && off < n; off += RATE) {
        uint32_t chunk = n - off < RATE ? n - off : RATE;
        for (uint32_t i = 0; i < chunk; i++) {
            buf[i] = s.ring[(from + off + i) % RING];
        }
        err = httpd_resp_send_chunk(req, (const char *)buf, chunk * sizeof(int16_t));
    }
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

typedef struct {
    const esp_partition_t *part;
    uint32_t total;
    bool erased;
} model_sink_t;

/* Checks the ESP-DL header before erasing the model partition. */
static esp_err_t model_sink(void *arg, uint32_t offset, const void *data, size_t len)
{
    model_sink_t *ctx = arg;
    if (!ctx->erased) {
        const uint32_t *hdr = data;
        if (offset != 0 || len < 16 || memcmp(data, "EDL2", 4) != 0 || hdr[2] + 16 != ctx->total) {
            return ESP_ERR_INVALID_VERSION;
        }
        ESP_RETURN_ON_ERROR(esp_partition_erase_range(ctx->part, 0, (ctx->total + 0xfff) & ~0xfff), TAG, "erase");
        ctx->erased = true;
    }
    return esp_partition_write(ctx->part, offset, data, len);
}

static esp_err_t model_upload(httpd_req_t *req)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, "model");
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return gw_http_send_error(req, 413, "model does not fit the model partition");
    }
    if (sound_model_ready()) {
        return gw_http_send_error(req, 409, "model in use, turn sound inference off and restart first");
    }
    model_sink_t ctx = {.part = part, .total = req->content_len};
    esp_err_t err = gw_http_receive(req, model_sink, &ctx);
    if (err != ESP_OK) {
        if (ctx.erased) {
            esp_partition_erase_range(part, 0, 0x1000);
        }
        return gw_http_send_error(req, 400, err == ESP_ERR_INVALID_VERSION ? "not an ESP-DL model" : esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "model stored, %u B", (unsigned)req->content_len);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "bytes", req->content_len);
    cJSON_AddBoolToObject(json, "restart_required", true);
    return gw_http_send_json(req, json);
}

/* ---------------------------------------------------------------- MQTT */

static void level_extra(cJSON *cfg)
{
    cJSON_AddStringToObject(cfg, "unit_of_measurement", "dBFS");
    cJSON_AddStringToObject(cfg, "state_class", "measurement");
    cJSON_AddStringToObject(cfg, "icon", "mdi:microphone");
    cJSON_AddStringToObject(cfg, "entity_category", "diagnostic");
}

static void noise_extra(cJSON *cfg)
{
    cJSON_AddStringToObject(cfg, "device_class", "sound_pressure");
    cJSON_AddStringToObject(cfg, "unit_of_measurement", "dBA");
    cJSON_AddStringToObject(cfg, "state_class", "measurement");
}

static void mqtt_discovery(void)
{
    if (s.running) {
        gw_mqtt_entity("sensor", "sound_level", "Sound level", "{{ value_json.sound_level }}", level_extra);
        gw_mqtt_entity("sensor", "noise", "Noise", "{{ value_json.noise_laeq }}", noise_extra);
        gw_mqtt_entity("sensor", "noise_max", "Noise max", "{{ value_json.noise_max }}", noise_extra);
        sound_events_mqtt_discovery();
    }
}

static void mqtt_state(cJSON *state)
{
    if (s.running) {
        cJSON_AddNumberToObject(state, "sound_level", round((double)(s.level_db) * 10) / 10.0);
        float laeq, lamax;
        bool overload;
        spl_window(&laeq, &lamax, &overload);
        cJSON_AddNumberToObject(state, "noise_laeq", round((double)spl(laeq) * 10) / 10.0);
        cJSON_AddNumberToObject(state, "noise_max", round((double)spl(lamax) * 10) / 10.0);
    }
}

static bool learn_ready(void)
{
    return s.running && s.infer_enabled && sound_model_ready();
}

static float learn_gate_db(void)
{
    return s.gate_db;
}

/* The model on audio that is not in the ring (an uploaded recording). Inference task only. */
static esp_err_t learn_embed(const int16_t *audio, float *embedding)
{
    static float *patch, *scores;
    float frame[MEL_WINDOW];
    if (!patch) {
        patch = heap_caps_malloc(MEL_PATCH_FRAMES * MEL_BANDS * sizeof(float), MALLOC_CAP_SPIRAM);
        scores = heap_caps_malloc(SOUND_CLASSES * sizeof(float), MALLOC_CAP_SPIRAM);
    }
    if (!patch || !scores) {
        return ESP_ERR_NO_MEM;
    }
    for (int f = 0; f < MEL_PATCH_FRAMES; f++) {
        for (int i = 0; i < MEL_WINDOW; i++) {
            frame[i] = audio[f * MEL_HOP + i] / 32768.0f;
        }
        mel_frame(frame, patch + f * MEL_BANDS);
    }
    if (!gw_flash_try_lock()) {
        return ESP_ERR_INVALID_STATE; /* an upload or C6 flashing is writing the flash */
    }
    esp_err_t err = sound_model_run(patch, scores, embedding);
    gw_flash_unlock();
    vTaskDelay(1); /* several windows in a row: let the idle task run */
    return err;
}

/* ---- Telegram commands ---- */

static void bot_noise(const char *args, bool cz, char *out, size_t len)
{
    if (!s.running) {
        strlcpy(out, cz ? "Mikrofon je vypnutý." : "The microphone is off.", len);
        return;
    }
    float laeq, lamax;
    bool overload;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    spl_window(&laeq, &lamax, &overload);
    float las = to_db(sqrtf(s.las_ms));
    xSemaphoreGive(s_lock);
    snprintf(out, len, cz ? "<b>Hluk</b>\nTeď: %.1f dB(A)\nLAeq 60 s: %.1f dB(A)\nMax 60 s: %.1f dB(A)%s%s"
                          : "<b>Noise</b>\nNow: %.1f dB(A)\nLAeq 60 s: %.1f dB(A)\nMax 60 s: %.1f dB(A)%s%s",
             (double)spl(las), (double)spl(laeq), (double)spl(lamax), overload ? (cz ? "\nPřebuzeno" : "\nOverload") : "",
             s.spl_calibrated ? "" : (cz ? "\n(nekalibrováno, ±6 dB)" : "\n(not calibrated, ±6 dB)"));
}

/* /sound on|off: the recognition (the microphone and the noise level keep running) */
static void bot_sound(const char *args, bool cz, char *out, size_t len)
{
    if (strcmp(args, "on") != 0 && strcmp(args, "off") != 0) {
        snprintf(out, len, cz ? "Rozpoznávání je %s. Použijte /sound on nebo /sound off." : "Recognition is %s. Use /sound on or /sound off.",
                 s.infer_enabled ? (cz ? "zapnuté" : "on") : (cz ? "vypnuté" : "off"));
        return;
    }
    s.infer_enabled = strcmp(args, "on") == 0;
    gw_config_set_u8(KEY_INFER, s.infer_enabled);
    if (s.infer_enabled) {
        s.model_error[0] = '\0';
    }
    strlcpy(out, s.infer_enabled ? (cz ? "Rozpoznávání zvuků zapnuto." : "Sound recognition on.")
                                 : (cz ? "Rozpoznávání zvuků vypnuto." : "Sound recognition off."), len);
}

esp_err_t sound_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s.gain_db = gw_config_get_u8(KEY_GAIN, 30);
    s.gate_db = -(int)gw_config_get_u8(KEY_GATE, 60);
    spl_load();
    s.infer_enabled = gw_config_get_u8(KEY_INFER, 1);
    gw_http_register("/api/sound/status", HTTP_GET, status_get);
    gw_bot_command("noise", "noise level in dB(A)", "hlučnost v dB(A)", "sound/live", 1, false, bot_noise);
    gw_bot_command("sound", "sound recognition on or off (/sound on, /sound off)", "rozpoznávání zvuků (/sound on, /sound off)",
                   "sound/live", 2, true, bot_sound);
    gw_http_register("/api/sound/config", HTTP_POST, config_post);
    gw_http_register("/api/sound/clip", HTTP_GET, clip_get);
    gw_http_register("/api/ota/model", HTTP_POST, model_upload);
    gw_mqtt_set_extension(mqtt_discovery, mqtt_state);
    load_class_names();
    sound_events_init(class_name);
    static const sound_learn_hooks_t hooks = {.ready = learn_ready, .gate_db = learn_gate_db, .embed = learn_embed};
    sound_learn_init(&hooks);
    if (!gw_config_get_u8(KEY_ENABLED, 0)) {
        return ESP_OK;
    }
    esp_err_t err = start();
    if (err != ESP_OK) {
        snprintf(s.error, sizeof(s.error), "microphone start failed: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", s.error);
    }
    /* A sound problem never stops the gateway. */
    return ESP_OK;
}
