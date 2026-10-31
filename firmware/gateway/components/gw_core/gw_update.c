#include <stdlib.h>
#include <sys/param.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "c6_flasher.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "mbedtls/sha256.h"

static const char *TAG = "gw_update";

/* in ticks: pdMS_TO_TICKS(6 h) overflows (ms * tick rate in 32 bits) and gave 125 s */
#define CHECK_INTERVAL_TICKS ((TickType_t)6 * 3600 * configTICK_RATE_HZ)
#define MANIFEST_MAX 8192
#define RELEASES_MAX 32768 /* releases.json: 10 releases with notes of up to 1000 characters */
#define RELEASES_KEEP 10
#define URL_MAX 256
#define C6FW_SUBTYPE 0x40
/* releases of the project (.github/workflows/release.yml); used while no URL is stored, "" stored = off */
#define DEFAULT_URL "https://github.com/KinDR007/jc-esp32p4-ha-gateway/releases/latest/download/manifest.json"

static bool manifest_url(char *url, size_t len)
{
    if (!gw_config_get_str(GW_KEY_UPDATE_URL, url, len)) {
        strlcpy(url, DEFAULT_URL, len);
    }
    return url[0] != '\0';
}

/* Offered update, from the last manifest check. */
static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    char latest[32];
    char image_url[URL_MAX];
    char notes[1024];
    char release_url[160];
    char build[128]; /* "commit abc1234 · 2026-10-30 · ESP-IDF v5.5.5" */
    /* C6 firmware offered by the manifest: downloaded into PSRAM on request, checked, flashed into the
     * C6 at the next start */
    struct {
        bool offered;
        bool current; /* the C6 was last flashed from exactly this file */
        char url[URL_MAX];
        uint8_t sha256[32];
        uint32_t size;
        cJSON *bundles; /* {name: version} */
        bool install_requested;
        bool installing;
        int percent;
        uint8_t notified[32]; /* sha256 of the package last announced */
        char installed_id[9]; /* first bytes of the sha256 of the package last flashed, "" when none */
    } c6;
    /* C5 package (external radio B, docs/dual-radio.md): downloaded when radio B needs it */
    struct {
        bool offered;
        char url[URL_MAX];
        uint8_t sha256[32];
        uint32_t size;
        int percent;
    } c5;
    char want[32];   /* release picked for the requested installation, "" = the offered one */
    char *releases;  /* JSON array of the listed releases this board runs (PSRAM), NULL = no list */
    char error[96];
    int64_t checked;    /* unix time of the last successful check, 0 = never */
    bool installing;
    bool checking;
    int percent;
    bool install_requested;
} s_up;

static void set_error(const char *fmt, const char *detail)
{
    snprintf(s_up.error, sizeof(s_up.error), fmt, detail);
    ESP_LOGW(TAG, "%s", s_up.error);
}

static esp_http_client_handle_t http_open(const char *url, esp_http_client_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->url = url;
    cfg->timeout_ms = 15000;
    cfg->crt_bundle_attach = esp_crt_bundle_attach;
    cfg->buffer_size = 4096; /* GitHub redirects with a Location header of about 1 kB */
    cfg->buffer_size_tx = 2048;
    cfg->keep_alive_enable = true;
    return esp_http_client_init(cfg);
}

/* Opens a GET and follows redirects (GitHub release assets: github.com -> 302 -> CDN); returns the
 * final status, or -1 when the connection failed (err set). */
static int http_get(esp_http_client_handle_t client, esp_err_t *err)
{
    for (int hop = 0; hop < 5; hop++) {
        if ((*err = esp_http_client_open(client, 0)) != ESP_OK) {
            return -1;
        }
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 301 && status != 302 && status != 303 && status != 307 && status != 308) {
            return status;
        }
        esp_http_client_set_redirection(client);
        esp_http_client_close(client);
    }
    return 310; /* too many redirects */
}

/* GET into a new PSRAM buffer of at most max bytes (NUL terminated, free it); returns the HTTP
 * status, -1 when the connection failed (err set). */
static int fetch(const char *url, int max, char **body, esp_err_t *err)
{
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client = http_open(url, &cfg);
    *body = heap_caps_calloc(1, max + 1, MALLOC_CAP_SPIRAM);
    int status = -1, len = 0;
    *err = client && *body ? ESP_OK : ESP_ERR_NO_MEM;
    if (*err == ESP_OK) {
        status = http_get(client, err);
    }
    if (*err == ESP_OK) {
        for (int n; len < max && (n = esp_http_client_read(client, *body + len, max - len)) > 0;) {
            len += n;
        }
    }
    esp_http_client_cleanup(client);
    return status;
}

/* Flash layout of this board (docs/installation.md): v3 keeps only a part of the C6 package in a
 * c6fw partition of 64 kB, v2 the whole package. */
static int flash_layout(void)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, C6FW_SUBTYPE, NULL);
    return part && part->size < 0x200000 ? 3 : 2;
}

/* "layouts" of a manifest or of a releases.json entry names the layout of this board */
static bool runs_here(const cJSON *layouts)
{
    const cJSON *l;
    cJSON_ArrayForEach(l, layouts)
    {
        if (cJSON_IsNumber(l) && l->valueint == flash_layout()) {
            return true;
        }
    }
    return false;
}

/* "p4gw.bin" relative to the manifest URL, or an absolute URL. */
static void resolve_url(const char *base, const char *ref, char *out, size_t len)
{
    if (strncmp(ref, "http://", 7) == 0 || strncmp(ref, "https://", 8) == 0) {
        strlcpy(out, ref, len);
        return;
    }
    const char *slash = strrchr(base, '/');
    snprintf(out, len, "%.*s/%s", (int)(slash ? slash - base : (int)strlen(base)), base, ref);
}

static bool unhex32(const char *in, uint8_t out[32])
{
    if (!in || strlen(in) != 64) {
        return false;
    }
    for (int i = 0; i < 32; i++) {
        unsigned v;
        if (sscanf(in + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = v;
    }
    return true;
}

/* Short id of the package radio A was last flashed from: the first bytes of its sha256. */
static void c6_installed_id(void)
{
    uint8_t sha[32];
    s_up.c6.installed_id[0] = '\0';
    if (c6fw_package_sha(C6_RADIO_A, sha)) {
        snprintf(s_up.c6.installed_id, sizeof(s_up.c6.installed_id), "%02x%02x%02x%02x", sha[0], sha[1], sha[2], sha[3]);
    }
}

/* Called with the lock held: the C5 part of a manifest (absent: nothing offered). */
static void c5_offer(const char *manifest_url, const cJSON *c5)
{
    const cJSON *size = cJSON_GetObjectItem(c5, "size"), *fmt = cJSON_GetObjectItem(c5, "format");
    uint8_t sha[32];
    s_up.c5.offered = c5 && GW_JSON_STR(c5, "url") && unhex32(GW_JSON_STR(c5, "sha256"), sha) && cJSON_IsNumber(size) &&
                      size->valueint > 0 && cJSON_IsNumber(fmt) && fmt->valueint == C6FW_FORMAT_VERSION;
    if (s_up.c5.offered) {
        resolve_url(manifest_url, GW_JSON_STR(c5, "url"), s_up.c5.url, sizeof(s_up.c5.url));
        memcpy(s_up.c5.sha256, sha, 32);
        s_up.c5.size = size->valueint;
    }
}

/* Called with the lock held: the C6 part of a manifest (absent: nothing offered). */
static void c6_offer(const char *manifest_url, const cJSON *c6, bool notify)
{
    const cJSON *size = cJSON_GetObjectItem(c6, "size"), *fmt = cJSON_GetObjectItem(c6, "format");
    uint8_t sha[32];
    bool valid = c6 && GW_JSON_STR(c6, "url") && unhex32(GW_JSON_STR(c6, "sha256"), sha) && cJSON_IsNumber(size) &&
                 size->valueint > 0 && cJSON_IsNumber(fmt);
    cJSON_Delete(s_up.c6.bundles);
    s_up.c6.bundles = NULL;
    s_up.c6.offered = false;
    if (!valid) {
        return;
    }
    if (fmt->valueint != C6FW_FORMAT_VERSION) {
        /* a C6 package of another format waits for the gateway firmware that reads it */
        ESP_LOGI(TAG, "C6 package format %d, this gateway reads %d", fmt->valueint, C6FW_FORMAT_VERSION);
        return;
    }
    s_up.c6.offered = true;
    resolve_url(manifest_url, GW_JSON_STR(c6, "url"), s_up.c6.url, sizeof(s_up.c6.url));
    memcpy(s_up.c6.sha256, sha, 32);
    s_up.c6.size = size->valueint;
    s_up.c6.bundles = cJSON_Duplicate(cJSON_GetObjectItem(c6, "bundles"), true);
    uint8_t have[32];
    s_up.c6.current = c6fw_package_sha(C6_RADIO_A, have) && memcmp(have, sha, 32) == 0;
    if (notify && !s_up.c6.current && memcmp(s_up.c6.notified, sha, 32) != 0) {
        memcpy(s_up.c6.notified, sha, 32);
        gw_notify("system", GW_NOTIFY_INFO, "C6 radio firmware update available (%lu kB)", (unsigned long)(s_up.c6.size / 1024));
    }
}

/* Makes the manifest at url the offer; latest: the configured manifest (announced, sets the time of
 * the check), else a release picked from the list. */
static esp_err_t check_manifest(const char *url, bool latest)
{
    char *body;
    esp_err_t err;
    int status = fetch(url, MANIFEST_MAX, &body, &err);

    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON *m = status == 200 ? cJSON_Parse(body) : NULL;
    const char *version = GW_JSON_STR(m, "version"), *image = GW_JSON_STR(m, "p4gw");
    const cJSON *layouts = cJSON_GetObjectItem(m, "layouts");
    if (err != ESP_OK) {
        set_error("manifest: %s", esp_err_to_name(err));
    } else if (status != 200) {
        char code[16];
        snprintf(code, sizeof(code), "%d", status);
        set_error("manifest: HTTP %s", code);
        err = ESP_FAIL;
    } else if (!version || !image || strlen(version) >= sizeof(s_up.latest)) {
        set_error("manifest: %s", "expected {\"version\", \"p4gw\"}");
        err = ESP_ERR_INVALID_RESPONSE;
    } else if ((layouts || !latest) && !runs_here(layouts)) {
        /* manifests before v1.6 name no layout: only the configured one is taken without */
        set_error("%s does not run on the flash layout of this gateway", version);
        err = ESP_ERR_NOT_SUPPORTED;
    } else {
        if (latest && strcmp(s_up.latest, version) != 0 && strcmp(version, esp_app_get_description()->version) != 0) {
            gw_notify("system", GW_NOTIFY_INFO, "Firmware update %s available (running %s)", version,
                      esp_app_get_description()->version);
        }
        strlcpy(s_up.latest, version, sizeof(s_up.latest));
        resolve_url(url, image, s_up.image_url, sizeof(s_up.image_url));
        strlcpy(s_up.notes, GW_JSON_STR(m, "notes") ? GW_JSON_STR(m, "notes") : "", sizeof(s_up.notes));
        strlcpy(s_up.release_url, GW_JSON_STR(m, "release_url") ? GW_JSON_STR(m, "release_url") : "",
                sizeof(s_up.release_url));
        const cJSON *b = cJSON_GetObjectItem(m, "build");
        snprintf(s_up.build, sizeof(s_up.build), "%s%s%s%s%s", GW_JSON_STR(b, "commit") ? GW_JSON_STR(b, "commit") : "",
                 GW_JSON_STR(b, "date") ? " · " : "", GW_JSON_STR(b, "date") ? GW_JSON_STR(b, "date") : "",
                 GW_JSON_STR(b, "idf") ? " · ESP-IDF " : "", GW_JSON_STR(b, "idf") ? GW_JSON_STR(b, "idf") : "");
        c6_offer(url, cJSON_GetObjectItem(m, "c6fw"), latest);
        c5_offer(url, cJSON_GetObjectItem(m, "c5fw"));
        s_up.error[0] = '\0';
        if (latest) {
            s_up.checked = time(NULL);
        }
        ESP_LOGI(TAG, "offered %s, running %s", s_up.latest, esp_app_get_description()->version);
    }
    if (err != ESP_OK && !latest) {
        /* nothing of a release that failed stays offered */
        s_up.latest[0] = s_up.build[0] = '\0';
        c6_offer(NULL, NULL, false);
    }
    cJSON_Delete(m);
    free(body);
    xSemaphoreGive(s_up.lock);
    return err;
}

/* releases.json next to the manifest (scripts/make_releases_index.py): the releases this board runs,
 * newest first. Kept when it cannot be read, dropped when the server has none. */
static void fetch_releases(const char *manifest)
{
    char url[URL_MAX], *body;
    esp_err_t err;
    resolve_url(manifest, "releases.json", url, sizeof(url));
    int status = fetch(url, RELEASES_MAX, &body, &err);
    cJSON *index = status == 200 ? cJSON_Parse(body) : NULL;
    free(body);
    if (!index && status != 404) {
        return;
    }
    cJSON *list = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, cJSON_GetObjectItem(index, "releases"))
    {
        if (cJSON_GetArraySize(list) < RELEASES_KEEP && GW_JSON_STR(r, "version") && GW_JSON_STR(r, "manifest") &&
            runs_here(cJSON_GetObjectItem(r, "layouts"))) {
            cJSON_AddItemToArray(list, cJSON_Duplicate(r, true));
        }
    }
    char *text = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    cJSON_Delete(index);
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON_free(s_up.releases);
    s_up.releases = text;
    xSemaphoreGive(s_up.lock);
}

static void check(void)
{
    char url[URL_MAX];
    if (!manifest_url(url, sizeof(url))) {
        xSemaphoreTake(s_up.lock, portMAX_DELAY);
        s_up.latest[0] = s_up.error[0] = s_up.build[0] = '\0';
        c6_offer(NULL, NULL, false);
        c5_offer(NULL, NULL);
        cJSON_free(s_up.releases);
        s_up.releases = NULL;
        xSemaphoreGive(s_up.lock);
        return;
    }
    if (check_manifest(url, true) == ESP_OK) {
        fetch_releases(url);
    }
}

/* Manifest URL of a listed release; false when the list has no such version. */
static bool release_manifest(const char *version, char *url, size_t len)
{
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON *list = s_up.releases ? cJSON_Parse(s_up.releases) : NULL;
    xSemaphoreGive(s_up.lock);
    const cJSON *r;
    bool found = false;
    cJSON_ArrayForEach(r, list)
    {
        const char *v = GW_JSON_STR(r, "version"), *m = GW_JSON_STR(r, "manifest");
        if (!found && v && m && strcmp(v, version) == 0) {
            strlcpy(url, m, len);
            found = true;
        }
    }
    cJSON_Delete(list);
    return found;
}

/* Called by the task: the release picked for the installation becomes the offer. */
static esp_err_t select_release(const char *version)
{
    char url[URL_MAX];
    if (!release_manifest(version, url, sizeof(url))) {
        set_error("%s is not offered for this gateway", version);
        return ESP_ERR_NOT_FOUND;
    }
    esp_err_t err = check_manifest(url, false);
    if (err == ESP_OK && strcmp(s_up.latest, version) != 0) {
        set_error("the manifest of %s names another version", version);
        err = ESP_ERR_INVALID_RESPONSE;
    }
    return err;
}

/* After a failed installation of a picked release: the latest release is offered again, the error kept. */
static void restore_offer(void)
{
    char error[sizeof(s_up.error)];
    strlcpy(error, s_up.error, sizeof(error));
    check();
    strlcpy(s_up.error, error, sizeof(s_up.error));
}

static esp_err_t install(void)
{
    char url[URL_MAX];
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    strlcpy(url, s_up.image_url, sizeof(url));
    xSemaphoreGive(s_up.lock);

    esp_http_client_config_t http;
    memset(&http, 0, sizeof(http));
    http.url = url;
    http.timeout_ms = 15000;
    http.crt_bundle_attach = esp_crt_bundle_attach;
    http.buffer_size = 4096;
    http.buffer_size_tx = 2048; /* the request after GitHub's redirect carries a URL of about 1 kB */
    http.keep_alive_enable = true;
    /* esp_https_ota would take the next OTA partition, which after gw_b is the esphome slot */
    esp_https_ota_config_t cfg = {.http_config = &http, .partition.staging = gw_other_gateway_slot()};
    esp_https_ota_handle_t ota = NULL;
    esp_app_desc_t desc;

    esp_err_t err = esp_https_ota_begin(&cfg, &ota);
    if (err != ESP_OK) {
        set_error("cannot download the image (%s): missing file or server down", esp_err_to_name(err));
        return err;
    }
    /* The image is checked before the first write erases the other slot. */
    if ((err = esp_https_ota_get_img_desc(ota, &desc)) != ESP_OK) {
        set_error("%s", "not an ESP application image");
        esp_https_ota_abort(ota);
        return err;
    }
    if (strncmp(desc.project_name, "p4gw", sizeof(desc.project_name)) != 0) {
        set_error("image is %s, not p4gw", desc.project_name);
        esp_https_ota_abort(ota);
        return ESP_ERR_INVALID_ARG;
    }
    int size = esp_https_ota_get_image_size(ota);
    while ((err = esp_https_ota_perform(ota)) == ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
        /* perform returns at once while no data is waiting, and every sector it erases stalls both
         * cores; this loop runs at a priority inherited from the network stack. A tick of rest was not
         * enough once two radios keep their UART bridges busy (IDLE1 starved, task watchdog during the
         * download, 2026-10-31): 10 ms per step, about 7 s more for a whole image. */
        vTaskDelay(pdMS_TO_TICKS(10));
        if (size > 0) {
            int percent = (int)(100LL * esp_https_ota_get_image_len_read(ota) / size);
            if (percent / 10 != s_up.percent / 10) {
                s_up.percent = percent;
                gw_update_mqtt_state();
            }
            s_up.percent = percent;
        }
    }
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(ota)) {
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(ota);
        set_error("download: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_https_ota_finish(ota); /* validates the image and selects it for the next boot */
    if (err != ESP_OK) {
        set_error("image rejected: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGW(TAG, "installed %s %s, restarting", desc.project_name, desc.version);
    return ESP_OK;
}

/* A radio package (c6fw.bin, c5fw.bin) into the PSRAM staging buffer, checked against the manifest
 * (sha256), then bundle by bundle and for its chip (c6fw_stage_commit); the radio is flashed from PSRAM. */
static esp_err_t download_package(const char *url, const uint8_t want[32], uint32_t size, uint32_t want_chip, int *pct)
{
    uint32_t capacity;
    uint8_t *data = c6fw_stage_buffer(&capacity);
    if (size > capacity) {
        set_error("%s", "the package is larger than the staging buffer");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client = http_open(url, &cfg);
    esp_err_t err = client ? ESP_OK : ESP_ERR_NO_MEM;
    uint32_t got = 0;
    int status = err == ESP_OK ? http_get(client, &err) : -1;
    if (err == ESP_OK) {
        if (status != 200) {
            char code[16];
            snprintf(code, sizeof(code), "%d", status);
            set_error("radio package: HTTP %s", code);
            err = ESP_FAIL;
        }
        for (int n; err == ESP_OK && got < size && (n = esp_http_client_read(client, (char *)data + got, size - got)) > 0;) {
            got += n;
            int percent = (int)(90LL * got / size);
            if (percent / 10 != *pct / 10) {
                *pct = percent;
                gw_update_mqtt_state();
            }
            *pct = percent;
            vTaskDelay(1);
        }
    } else {
        set_error("cannot download the radio package (%s)", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    if (err == ESP_OK && got != size) {
        set_error("%s", "radio package incomplete");
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        uint8_t sha[32];
        mbedtls_sha256(data, size, sha, 0);
        if (memcmp(sha, want, 32) != 0) {
            set_error("%s", "radio package sha256 does not match the manifest");
            err = ESP_ERR_INVALID_CRC;
        }
    }
    if (err == ESP_OK) {
        *pct = 92;
        uint32_t chip = 0;
        err = c6fw_stage_commit(size, &chip);
        if (err != ESP_OK) {
            set_error("radio package not usable: %s", esp_err_to_name(err));
        } else if (chip != want_chip) {
            c6fw_stage_drop();
            set_error("%s", "the radio package of the manifest is for another chip");
            err = ESP_ERR_INVALID_RESPONSE;
        }
    }
    return err;
}

static esp_err_t install_c6(void)
{
    char url[URL_MAX];
    uint8_t want[32];
    uint32_t size;
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    strlcpy(url, s_up.c6.url, sizeof(url));
    memcpy(want, s_up.c6.sha256, 32);
    size = s_up.c6.size;
    xSemaphoreGive(s_up.lock);
    esp_err_t err = download_package(url, want, size, C6FW_CHIP_C6, &s_up.c6.percent);
    if (err == ESP_OK) {
        s_up.c6.current = true;
        c6_installed_id();
        ESP_LOGW(TAG, "C6 package stored (%lu bytes), the C6 is updated after the restart", (unsigned long)size);
        gw_notify("system", GW_NOTIFY_INFO, "C6 radio firmware downloaded; the gateway restarts and updates the C6");
    }
    return err;
}

esp_err_t gw_update_fetch_c6(void)
{
    /* the start-up sequence asks before the update task checked: check here */
    if (!s_up.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    check();
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    bool offered = s_up.c6.offered;
    char url[URL_MAX];
    uint8_t want[32];
    uint32_t size = s_up.c6.size;
    strlcpy(url, s_up.c6.url, sizeof(url));
    memcpy(want, s_up.c6.sha256, 32);
    xSemaphoreGive(s_up.lock);
    if (!offered) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "downloading the C6 package (%lu kB) to flash the C6", (unsigned long)(size / 1024));
    esp_err_t err = download_package(url, want, size, C6FW_CHIP_C6, &s_up.c6.percent);
    s_up.c6.percent = 0;
    return err;
}

esp_err_t gw_update_fetch_package(uint32_t chip)
{
    if (chip != C6FW_CHIP_C5) {
        return gw_update_fetch_c6();
    }
    if (!s_up.lock) {
        return ESP_ERR_INVALID_STATE;
    }
    check();
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    bool offered = s_up.c5.offered;
    char url[URL_MAX];
    uint8_t want[32];
    uint32_t size = s_up.c5.size;
    strlcpy(url, s_up.c5.url, sizeof(url));
    memcpy(want, s_up.c5.sha256, 32);
    xSemaphoreGive(s_up.lock);
    if (!offered) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGW(TAG, "downloading the C5 package (%lu kB) to flash radio B", (unsigned long)(size / 1024));
    esp_err_t err = download_package(url, want, size, C6FW_CHIP_C5, &s_up.c5.percent);
    s_up.c5.percent = 0;
    return err;
}

static void update_task(void *arg)
{
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    c6_installed_id();
    xSemaphoreGive(s_up.lock);
    /* after start-up (network, clock, MQTT), or at once when someone asks for a check */
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(30 * 1000));
    while (true) {
        if (s_up.c6.install_requested) {
            s_up.c6.install_requested = false;
            esp_err_t err = ESP_ERR_INVALID_STATE;
            if (gw_upload_begin()) {
                s_up.c6.installing = true;
                s_up.c6.percent = 0;
                s_up.error[0] = '\0';
                gw_update_mqtt_state();
                err = s_up.want[0] ? select_release(s_up.want) : ESP_OK;
                if (err == ESP_OK && !s_up.c6.offered) {
                    set_error("%s", "no C6 package this gateway reads in the release");
                    err = ESP_ERR_NOT_FOUND;
                }
                if (err == ESP_OK) {
                    err = install_c6();
                }
                gw_upload_end();
            } else {
                set_error("%s", "another upload or C6 flash is running");
            }
            s_up.c6.installing = false;
            s_up.c6.percent = err == ESP_OK ? 100 : 0;
            if (err == ESP_OK) {
                gw_update_mqtt_state();
                gw_restart_later_full(2000, false);
                vTaskDelete(NULL);
            }
            if (s_up.want[0]) {
                s_up.want[0] = '\0';
                restore_offer();
            }
            gw_update_mqtt_state();
        } else if (s_up.install_requested) {
            s_up.install_requested = false;
            esp_err_t err = ESP_ERR_INVALID_STATE;
            if (gw_upload_begin()) {
                s_up.installing = true;
                s_up.percent = 0;
                s_up.error[0] = '\0';
                gw_update_mqtt_state();
                err = s_up.want[0] ? select_release(s_up.want) : ESP_OK;
                if (err == ESP_OK) {
                    err = install();
                }
                gw_upload_end();
            } else {
                set_error("%s", "another upload or C6 flash is running");
            }
            if (err == ESP_OK) {
                s_up.percent = 100;
                gw_update_mqtt_state();
                gw_restart_later(1000);
                vTaskDelete(NULL);
            }
            s_up.installing = false;
            if (s_up.want[0]) {
                s_up.want[0] = '\0';
                restore_offer();
            }
            gw_update_mqtt_state();
        } else {
            s_up.checking = true;
            check();
            s_up.checking = false;
            gw_update_mqtt_state();
        }
        /* woken early by gw_update_check_soon() and gw_update_install() */
        ulTaskNotifyTake(pdTRUE, CHECK_INTERVAL_TICKS);
    }
}

static esp_err_t check_post(httpd_req_t *req)
{
    char url[URL_MAX];
    if (!manifest_url(url, sizeof(url))) {
        return gw_http_send_error(req, 409, "no update URL set");
    }
    /* the task checks: a request here would block the web server (and never reach a manifest on this gateway) */
    gw_update_check_soon();
    return gw_http_send_json(req, gw_update_status());
}

/* {"target": "c6"} installs the C6 package, else the gateway firmware; {"version": "v1.6"} of the
 * listed releases, else of the offered one */
static esp_err_t install_post(httpd_req_t *req)
{
    bool c6 = false;
    char version[32] = "";
    if (req->content_len) {
        cJSON *body = gw_http_read_json(req);
        if (!body) {
            return ESP_OK;
        }
        const char *target = GW_JSON_STR(body, "target");
        c6 = target && strcmp(target, "c6") == 0;
        strlcpy(version, GW_JSON_STR(body, "version") ? GW_JSON_STR(body, "version") : "", sizeof(version));
        cJSON_Delete(body);
    }
    esp_err_t err = c6 ? gw_update_install_c6(version) : gw_update_install(version);
    if (err == ESP_ERR_NOT_FOUND) {
        return gw_http_send_error(req, 409, version[0] ? "release not offered for this gateway" : "no update offered, check first");
    }
    if (err != ESP_OK) {
        return gw_http_send_error(req, 409, "an installation is already running");
    }
    return gw_http_send_json(req, gw_update_status());
}

void gw_update_start(void)
{
    s_up.lock = xSemaphoreCreateMutex();
    xTaskCreate(update_task, "gw_update", 6144, NULL, 3, &s_up.task);
    gw_http_register("/api/update/check", HTTP_POST, check_post);
    gw_http_register("/api/update/install", HTTP_POST, install_post);
}

void gw_update_check_soon(void)
{
    if (s_up.task) {
        s_up.checking = true;
        xTaskNotifyGive(s_up.task);
    }
}

static bool busy(void)
{
    return s_up.installing || s_up.c6.installing || s_up.install_requested || s_up.c6.install_requested;
}

/* version of the listed releases, NULL or "" = the offered one */
static esp_err_t request(const char *version, bool offered)
{
    char url[URL_MAX];
    bool pick = version && version[0];
    if (!s_up.task || (pick ? !release_manifest(version, url, sizeof(url)) : !offered)) {
        return ESP_ERR_NOT_FOUND;
    }
    if (busy()) {
        return ESP_ERR_INVALID_STATE;
    }
    strlcpy(s_up.want, pick ? version : "", sizeof(s_up.want));
    return ESP_OK;
}

esp_err_t gw_update_install_c6(const char *version)
{
    esp_err_t err = request(version, s_up.c6.offered);
    if (err != ESP_OK) {
        return err;
    }
    s_up.c6.install_requested = true;
    xTaskNotifyGive(s_up.task);
    return ESP_OK;
}

void gw_update_c6_changed(void)
{
    if (!s_up.lock) {
        return;
    }
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    uint8_t have[32];
    s_up.c6.current = s_up.c6.offered && c6fw_package_sha(C6_RADIO_A, have) &&
                      memcmp(have, s_up.c6.sha256, 32) == 0;
    c6_installed_id();
    xSemaphoreGive(s_up.lock);
}

esp_err_t gw_update_install(const char *version)
{
    esp_err_t err = request(version, s_up.latest[0]);
    if (err != ESP_OK) {
        return err;
    }
    s_up.install_requested = true;
    xTaskNotifyGive(s_up.task);
    return ESP_OK;
}

static cJSON *state_json(void)
{
    const char *installed = esp_app_get_description()->version;
    cJSON *j = cJSON_CreateObject();
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON_AddStringToObject(j, "installed_version", installed);
    /* Home Assistant shows no update when latest equals installed; unknown latest = installed. */
    cJSON_AddStringToObject(j, "latest_version", s_up.latest[0] ? s_up.latest : installed);
    cJSON_AddStringToObject(j, "title", "P4 gateway firmware");
    cJSON_AddStringToObject(j, "release_summary", s_up.notes);
    if (s_up.release_url[0]) {
        cJSON_AddStringToObject(j, "release_url", s_up.release_url);
    }
    cJSON_AddBoolToObject(j, "in_progress", s_up.installing);
    if (s_up.installing) {
        cJSON_AddNumberToObject(j, "update_percentage", s_up.percent);
    }
    cJSON *c6 = cJSON_AddObjectToObject(j, "c6");
    cJSON_AddBoolToObject(c6, "offered", s_up.c6.offered);
    cJSON_AddBoolToObject(c6, "available", s_up.c6.offered && !s_up.c6.current);
    cJSON_AddBoolToObject(c6, "in_progress", s_up.c6.installing);
    cJSON_AddNumberToObject(c6, "percent", s_up.c6.percent);
    cJSON_AddStringToObject(c6, "installed_id", s_up.c6.installed_id);
    char offered[9] = "";
    if (s_up.c6.offered) {
        snprintf(offered, sizeof(offered), "%02x%02x%02x%02x", s_up.c6.sha256[0], s_up.c6.sha256[1], s_up.c6.sha256[2],
                 s_up.c6.sha256[3]);
    }
    cJSON_AddStringToObject(c6, "offered_id", offered);
    cJSON *c5 = cJSON_AddObjectToObject(j, "c5");
    cJSON_AddBoolToObject(c5, "offered", s_up.c5.offered);
    char c5_offered[9] = "";
    if (s_up.c5.offered) {
        snprintf(c5_offered, sizeof(c5_offered), "%02x%02x%02x%02x", s_up.c5.sha256[0], s_up.c5.sha256[1], s_up.c5.sha256[2],
                 s_up.c5.sha256[3]);
    }
    cJSON_AddStringToObject(c5, "offered_id", c5_offered);
    cJSON_AddNumberToObject(c5, "size", s_up.c5.size);
    uint8_t c5_have[32];
    char c5_installed[9] = "";
    if (c6fw_package_sha(C6_RADIO_B, c5_have)) {
        snprintf(c5_installed, sizeof(c5_installed), "%02x%02x%02x%02x", c5_have[0], c5_have[1], c5_have[2], c5_have[3]);
    }
    cJSON_AddStringToObject(c5, "installed_id", c5_installed);
    xSemaphoreGive(s_up.lock);
    return j;
}

cJSON *gw_update_status(void)
{
    cJSON *j = state_json();
    char url[URL_MAX];
    manifest_url(url, sizeof(url));
    cJSON_AddStringToObject(j, "url", url);
    cJSON_AddStringToObject(j, "default_url", DEFAULT_URL);
    cJSON_AddNumberToObject(j, "percent", s_up.percent);
    cJSON_AddNumberToObject(j, "checked", (double)s_up.checked);
    cJSON_AddBoolToObject(j, "checking", s_up.checking);
    cJSON_AddStringToObject(j, "error", s_up.error);
    cJSON_AddBoolToObject(j, "available", s_up.latest[0] && strcmp(s_up.latest, esp_app_get_description()->version) != 0);
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON_AddStringToObject(j, "build", s_up.build);
    cJSON_AddNumberToObject(j, "layout", flash_layout());
    cJSON *releases = s_up.releases ? cJSON_Parse(s_up.releases) : NULL;
    cJSON_AddItemToObject(j, "releases", releases ? releases : cJSON_CreateArray());
    cJSON *c6 = cJSON_GetObjectItem(j, "c6");
    cJSON_AddNumberToObject(c6, "size", s_up.c6.size);
    if (s_up.c6.bundles) {
        cJSON_AddItemToObject(c6, "offered_versions", cJSON_Duplicate(s_up.c6.bundles, true));
    }
    cJSON *inst = cJSON_AddObjectToObject(c6, "installed_versions");
    c6fw_dir_t dir;
    if (c6fw_read_dir(C6_RADIO_A, &dir) == ESP_OK) {
        for (uint32_t i = 0; i < dir.count && i < C6FW_MAX_ENTRIES; i++) {
            char name[17], version[33];
            strlcpy(name, dir.entries[i].name, sizeof(name));
            strlcpy(version, dir.entries[i].version, sizeof(version));
            cJSON_AddStringToObject(inst, name, version);
        }
    }
    xSemaphoreGive(s_up.lock);
    return j;
}

void gw_update_mqtt_discovery(void)
{
    char buf[96];
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", "Firmware");
    cJSON_AddStringToObject(cfg, "device_class", "firmware");
    cJSON_AddStringToObject(cfg, "entity_category", "config");
    snprintf(buf, sizeof(buf), "%s/update", gw_mqtt_base_topic());
    cJSON_AddStringToObject(cfg, "state_topic", buf);
    snprintf(buf, sizeof(buf), "%s/update/install", gw_mqtt_base_topic());
    cJSON_AddStringToObject(cfg, "command_topic", buf);
    cJSON_AddStringToObject(cfg, "payload_install", "install");
    gw_mqtt_discovery("update", "firmware", cfg);
}

void gw_update_mqtt_state(void)
{
    if (!s_up.lock) {
        return;
    }
    cJSON *j = state_json();
    char *payload = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    gw_mqtt_publish("update", payload, true);
    cJSON_free(payload);
}
