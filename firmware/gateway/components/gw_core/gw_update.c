#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"

static const char *TAG = "gw_update";

#define CHECK_INTERVAL_MS (6 * 3600 * 1000)
#define MANIFEST_MAX 4096
#define URL_MAX 256

/* Offered update, from the last manifest check. */
static struct {
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    char latest[32];
    char image_url[URL_MAX];
    char notes[256];
    char release_url[160];
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

static void check(void)
{
    char url[URL_MAX];
    if (!gw_config_get_str(GW_KEY_UPDATE_URL, url, sizeof(url)) || !url[0]) {
        xSemaphoreTake(s_up.lock, portMAX_DELAY);
        s_up.latest[0] = s_up.error[0] = '\0';
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

static void update_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(30 * 1000)); /* after start-up: network, clock, MQTT */
    while (true) {
        if (s_up.install_requested) {
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

static esp_err_t install_post(httpd_req_t *req)
{
    esp_err_t err = gw_update_install();
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
