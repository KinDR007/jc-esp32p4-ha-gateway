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

#define CHECK_INTERVAL_MS (6 * 3600 * 1000)
#define MANIFEST_MAX 8192
#define URL_MAX 256
#define C6FW_SUBTYPE 0x40

/* Offered update, from the last manifest check. */
static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    char latest[32];
    char image_url[URL_MAX];
    char notes[1024];
    char release_url[160];
    char build[128]; /* "commit abc1234 · 2026-10-30 · ESP-IDF v5.5.5" */
    /* C6 firmware offered by the manifest: downloaded into PSRAM on request, checked, then written
     * to the c6fw partition (the C6 gets it at the next start) */
    struct {
        bool offered;
        bool current; /* the c6fw partition holds exactly this file */
        char url[URL_MAX];
        uint8_t sha256[32];
        uint32_t size;
        cJSON *bundles; /* {name: version} */
        bool install_requested;
        bool installing;
        int percent;
        uint8_t notified[32]; /* sha256 of the package last announced */
        char installed_id[9]; /* first bytes of the sha256 of the stored package, "" when none */
    } c6;
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
    cfg->buffer_size = 2048;
    cfg->keep_alive_enable = true;
    return esp_http_client_init(cfg);
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

/* sha256 of the first len bytes of the c6fw partition */
static bool c6_partition_sha(uint32_t len, uint8_t out[32])
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, C6FW_SUBTYPE, NULL);
    uint8_t *buf = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    if (!part || !buf || len > part->size) {
        free(buf);
        return false;
    }
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    bool ok = true;
    for (uint32_t off = 0; off < len && ok; off += 4096) {
        uint32_t n = len - off < 4096 ? len - off : 4096;
        ok = esp_partition_read(part, off, buf, n) == ESP_OK;
        mbedtls_sha256_update(&ctx, buf, n);
    }
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
    free(buf);
    return ok;
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

/* Short id of the package in the c6fw partition: the sha256 of the file as it was written (packages
 * end on a 4 KiB boundary after the last bundle). */
static void c6_installed_id(void)
{
    c6fw_dir_t dir;
    uint8_t sha[32];
    s_up.c6.installed_id[0] = '\0';
    if (c6fw_read_dir(&dir) != ESP_OK) {
        return;
    }
    uint32_t end = 0;
    for (uint32_t i = 0; i < dir.count && i < C6FW_MAX_ENTRIES; i++) {
        end = MAX(end, dir.entries[i].offset + dir.entries[i].size);
    }
    if (c6_partition_sha((end + 0xfff) & ~0xfff, sha)) {
        snprintf(s_up.c6.installed_id, sizeof(s_up.c6.installed_id), "%02x%02x%02x%02x", sha[0], sha[1], sha[2], sha[3]);
    }
}

/* Called with the lock held: the C6 part of a manifest (absent: nothing offered). */
static void c6_offer(const char *manifest_url, const cJSON *c6)
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
    s_up.c6.current = c6_partition_sha(s_up.c6.size, have) && memcmp(have, sha, 32) == 0;
    if (!s_up.c6.current && memcmp(s_up.c6.notified, sha, 32) != 0) {
        memcpy(s_up.c6.notified, sha, 32);
        gw_notify("system", GW_NOTIFY_INFO, "C6 radio firmware update available (%lu kB)", (unsigned long)(s_up.c6.size / 1024));
    }
}

static void check(void)
{
    char url[URL_MAX];
    if (!gw_config_get_str(GW_KEY_UPDATE_URL, url, sizeof(url)) || !url[0]) {
        xSemaphoreTake(s_up.lock, portMAX_DELAY);
        s_up.latest[0] = s_up.error[0] = s_up.build[0] = '\0';
        c6_offer(NULL, NULL);
        xSemaphoreGive(s_up.lock);
        return;
    }
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client = http_open(url, &cfg);
    char *body = heap_caps_calloc(1, MANIFEST_MAX + 1, MALLOC_CAP_SPIRAM);
    int status = 0, len = 0;
    esp_err_t err = client && body ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        for (int n; len < MANIFEST_MAX && (n = esp_http_client_read(client, body + len, MANIFEST_MAX - len)) > 0;) {
            len += n;
        }
    }
    esp_http_client_cleanup(client);

    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON *m = status == 200 ? cJSON_Parse(body) : NULL;
    const char *version = GW_JSON_STR(m, "version"), *image = GW_JSON_STR(m, "p4gw");
    if (err != ESP_OK) {
        set_error("manifest: %s", esp_err_to_name(err));
    } else if (status != 200) {
        char code[16];
        snprintf(code, sizeof(code), "%d", status);
        set_error("manifest: HTTP %s", code);
    } else if (!version || !image || strlen(version) >= sizeof(s_up.latest)) {
        set_error("manifest: %s", "expected {\"version\", \"p4gw\"}");
    } else {
        if (strcmp(s_up.latest, version) != 0 && strcmp(version, esp_app_get_description()->version) != 0) {
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
        c6_offer(url, cJSON_GetObjectItem(m, "c6fw"));
        s_up.error[0] = '\0';
        s_up.checked = time(NULL);
        ESP_LOGI(TAG, "offered %s, running %s", s_up.latest, esp_app_get_description()->version);
    }
    cJSON_Delete(m);
    free(body);
    xSemaphoreGive(s_up.lock);
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
        /* perform returns at once while no data is waiting; without a yield this loop (at a priority
         * inherited from the network stack) starves both idle tasks until the task watchdog fires */
        vTaskDelay(1);
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

/* c6fw.bin into PSRAM, checked against the manifest (sha256) and its own header before the c6fw
 * partition is erased; the C6 itself is reflashed by the boot sequence after the restart. */
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
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, C6FW_SUBTYPE, NULL);
    if (!part || size > part->size) {
        set_error("%s", "the C6 package does not fit the c6fw partition");
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t *data = heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (!data) {
        set_error("%s", "no PSRAM for the C6 package");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_config_t cfg;
    esp_http_client_handle_t client = http_open(url, &cfg);
    esp_err_t err = client ? esp_http_client_open(client, 0) : ESP_ERR_NO_MEM;
    uint32_t got = 0;
    if (err == ESP_OK) {
        esp_http_client_fetch_headers(client);
        int status = esp_http_client_get_status_code(client);
        if (status != 200) {
            char code[16];
            snprintf(code, sizeof(code), "%d", status);
            set_error("C6 package: HTTP %s", code);
            err = ESP_FAIL;
        }
        for (int n; err == ESP_OK && got < size && (n = esp_http_client_read(client, (char *)data + got, size - got)) > 0;) {
            got += n;
            int percent = (int)(90LL * got / size);
            if (percent / 10 != s_up.c6.percent / 10) {
                s_up.c6.percent = percent;
                gw_update_mqtt_state();
            }
            s_up.c6.percent = percent;
            vTaskDelay(1);
        }
    } else {
        set_error("cannot download the C6 package (%s)", esp_err_to_name(err));
    }
    esp_http_client_cleanup(client);
    uint8_t sha[32];
    if (err == ESP_OK && got != size) {
        set_error("%s", "C6 package incomplete");
        err = ESP_ERR_INVALID_SIZE;
    }
    if (err == ESP_OK) {
        mbedtls_sha256(data, size, sha, 0);
        if (memcmp(sha, want, 32) != 0) {
            set_error("%s", "C6 package sha256 does not match the manifest");
            err = ESP_ERR_INVALID_CRC;
        } else if (memcmp(data, C6FW_DIR_MAGIC, 4) != 0 || ((uint32_t *)data)[1] != C6FW_FORMAT_VERSION) {
            set_error("%s", "not a C6 package of this format");
            err = ESP_ERR_INVALID_VERSION;
        }
    }
    if (err == ESP_OK) {
        s_up.c6.percent = 92;
        err = esp_partition_erase_range(part, 0, (size + 0xfff) & ~0xfff);
        for (uint32_t off = 0; err == ESP_OK && off < size; off += 4096) {
            err = esp_partition_write(part, off, data + off, size - off < 4096 ? size - off : 4096);
        }
        if (err == ESP_OK) {
            err = c6fw_verify_all();
        }
        if (err != ESP_OK) {
            esp_partition_erase_range(part, 0, 0x1000); /* never leave a half-written package that parses */
            set_error("C6 package not stored: %s", esp_err_to_name(err));
        }
    }
    free(data);
    if (err == ESP_OK) {
        s_up.c6.current = true;
        ESP_LOGW(TAG, "C6 package stored (%lu bytes), the C6 is updated after the restart", (unsigned long)size);
        gw_notify("system", GW_NOTIFY_INFO, "C6 radio firmware downloaded; the gateway restarts and updates the C6");
    }
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
                err = install_c6();
                gw_upload_end();
            } else {
                set_error("%s", "another upload or C6 flash is running");
            }
            s_up.c6.installing = false;
            s_up.c6.percent = err == ESP_OK ? 100 : 0;
            gw_update_mqtt_state();
            if (err == ESP_OK) {
                gw_restart_later_full(2000, false);
                vTaskDelete(NULL);
            }
        } else if (s_up.install_requested) {
            s_up.install_requested = false;
            esp_err_t err = ESP_ERR_INVALID_STATE;
            if (gw_upload_begin()) {
                s_up.installing = true;
                s_up.percent = 0;
                s_up.error[0] = '\0';
                gw_update_mqtt_state();
                err = install();
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
            gw_update_mqtt_state();
        } else {
            s_up.checking = true;
            check();
            s_up.checking = false;
            gw_update_mqtt_state();
        }
        /* woken early by gw_update_check_soon() and gw_update_install() */
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_INTERVAL_MS));
    }
}

static esp_err_t check_post(httpd_req_t *req)
{
    char url[URL_MAX];
    if (!gw_config_get_str(GW_KEY_UPDATE_URL, url, sizeof(url)) || !url[0]) {
        return gw_http_send_error(req, 409, "no update URL set");
    }
    /* the task checks: a request here would block the web server (and never reach a manifest on this gateway) */
    gw_update_check_soon();
    return gw_http_send_json(req, gw_update_status());
}

/* {"target": "c6"} installs the C6 package, else the gateway firmware */
static esp_err_t install_post(httpd_req_t *req)
{
    bool c6 = false;
    if (req->content_len) {
        cJSON *body = gw_http_read_json(req);
        if (!body) {
            return ESP_OK;
        }
        const char *target = GW_JSON_STR(body, "target");
        c6 = target && strcmp(target, "c6") == 0;
        cJSON_Delete(body);
    }
    esp_err_t err = c6 ? gw_update_install_c6() : gw_update_install();
    if (err == ESP_ERR_NOT_FOUND) {
        return gw_http_send_error(req, 409, "no update offered, check first");
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

esp_err_t gw_update_install_c6(void)
{
    if (!s_up.task || !s_up.c6.offered) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_up.installing || s_up.c6.installing || s_up.install_requested || s_up.c6.install_requested) {
        return ESP_ERR_INVALID_STATE;
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
    s_up.c6.current = s_up.c6.offered && c6_partition_sha(s_up.c6.size, have) && memcmp(have, s_up.c6.sha256, 32) == 0;
    c6_installed_id();
    xSemaphoreGive(s_up.lock);
}

esp_err_t gw_update_install(void)
{
    if (!s_up.task || !s_up.latest[0]) {
        return ESP_ERR_NOT_FOUND;
    }
    if (s_up.installing || s_up.install_requested) {
        return ESP_ERR_INVALID_STATE;
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
    xSemaphoreGive(s_up.lock);
    return j;
}

cJSON *gw_update_status(void)
{
    cJSON *j = state_json();
    char url[URL_MAX];
    gw_config_get_str(GW_KEY_UPDATE_URL, url, sizeof(url));
    cJSON_AddStringToObject(j, "url", url);
    cJSON_AddNumberToObject(j, "percent", s_up.percent);
    cJSON_AddNumberToObject(j, "checked", (double)s_up.checked);
    cJSON_AddBoolToObject(j, "checking", s_up.checking);
    cJSON_AddStringToObject(j, "error", s_up.error);
    cJSON_AddBoolToObject(j, "available", s_up.latest[0] && strcmp(s_up.latest, esp_app_get_description()->version) != 0);
    xSemaphoreTake(s_up.lock, portMAX_DELAY);
    cJSON_AddStringToObject(j, "build", s_up.build);
    cJSON *c6 = cJSON_GetObjectItem(j, "c6");
    cJSON_AddNumberToObject(c6, "size", s_up.c6.size);
    if (s_up.c6.bundles) {
        cJSON_AddItemToObject(c6, "offered_versions", cJSON_Duplicate(s_up.c6.bundles, true));
    }
    cJSON *inst = cJSON_AddObjectToObject(c6, "installed_versions");
    c6fw_dir_t dir;
    if (c6fw_read_dir(&dir) == ESP_OK) {
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
