#include <netdb.h>
#include <stdlib.h>
#include <string.h>

#include "c6_flasher.h"
#include "driver/temperature_sensor.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_chip_info.h"
#include "esp_core_dump.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "ping/ping_sock.h"

static const char *TAG = "gw_api";

#define UPLOAD_CHUNK 4096

static cJSON *(*s_role_status)(void);
static temperature_sensor_handle_t s_temp;
static SemaphoreHandle_t s_upload_lock;

void gw_api_set_role_status(cJSON *(*provider)(void))
{
    s_role_status = provider;
}

cJSON *gw_api_role_status(void)
{
    return s_role_status ? s_role_status() : NULL;
}

bool gw_api_temperature(float *celsius)
{
    return s_temp && temperature_sensor_get_celsius(s_temp, celsius) == ESP_OK;
}

static const char *reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON: return "power-on";
    case ESP_RST_SW: return "software";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "interrupt watchdog";
    case ESP_RST_TASK_WDT: return "task watchdog";
    case ESP_RST_WDT: return "watchdog";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_DEEPSLEEP: return "deep sleep";
    case ESP_RST_EXT: return "external";
    default: return "other";
    }
}

static cJSON *slot_json(const esp_partition_t *part)
{
    cJSON *slot = cJSON_CreateObject();
    esp_app_desc_t desc;
    esp_ota_img_states_t state;
    static const char *const states[] = {"new", "pending_verify", "valid", "invalid", "aborted"};

    cJSON_AddStringToObject(slot, "label", part->label);
    cJSON_AddNumberToObject(slot, "size", part->size);
    if (esp_ota_get_partition_description(part, &desc) == ESP_OK) {
        cJSON_AddStringToObject(slot, "project", desc.project_name);
        cJSON_AddStringToObject(slot, "version", desc.version);
        cJSON_AddStringToObject(slot, "built", desc.date);
    }
    if (esp_ota_get_state_partition(part, &state) == ESP_OK && state <= ESP_OTA_IMG_ABORTED) {
        cJSON_AddStringToObject(slot, "state", states[state]);
    } else if (state == ESP_OTA_IMG_UNDEFINED) {
        cJSON_AddStringToObject(slot, "state", "undefined");
    }
    cJSON_AddBoolToObject(slot, "running", part == esp_ota_get_running_partition());
    cJSON_AddBoolToObject(slot, "boot", part == esp_ota_get_boot_partition());
    return slot;
}

static esp_err_t status_get(httpd_req_t *req)
{
    gw_state_t *st = gw_state();
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    char password[2];
    cJSON *json = cJSON_CreateObject();

    esp_chip_info(&chip);
    cJSON_AddStringToObject(json, "hostname", st->hostname);
    cJSON_AddStringToObject(json, "version", app->version);
    cJSON_AddStringToObject(json, "idf", app->idf_ver);
    cJSON_AddStringToObject(json, "built", app->date);
    cJSON_AddNumberToObject(json, "chip_revision", chip.revision);
    cJSON_AddNumberToObject(json, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(json, "reset_reason", reset_reason_name(esp_reset_reason()));
    cJSON_AddBoolToObject(json, "auth", gw_config_get_str(GW_KEY_PASSWORD, password, sizeof(password)) ||
                                            password[0]);

    cJSON *heap = cJSON_AddObjectToObject(json, "heap");
    cJSON_AddNumberToObject(heap, "internal_free", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "internal_min", heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "internal_largest", heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(heap, "psram_free", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    float temp;
    if (gw_api_temperature(&temp)) {
        cJSON_AddNumberToObject(json, "temperature_c", (int)(temp * 10) / 10.0);
    }

    cJSON *role = cJSON_AddObjectToObject(json, "role");
    cJSON_AddStringToObject(role, "active", gw_role_name(st->active));
    cJSON_AddStringToObject(role, "desired", gw_role_name(st->desired));
    cJSON_AddStringToObject(role, "maintenance", st->maintenance);
    cJSON_AddBoolToObject(role, "stable", st->stable);

    cJSON_AddItemToObject(json, "eth", gw_eth_status());

    cJSON *slots = cJSON_AddArrayToObject(json, "slots");
    esp_partition_iterator_t it = esp_partition_find(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_ANY, NULL);
    for (; it; it = esp_partition_next(it)) {
        cJSON_AddItemToArray(slots, slot_json(esp_partition_get(it)));
    }
    esp_partition_iterator_release(it);

    cJSON *c6 = cJSON_AddObjectToObject(json, "c6");
    char bundle[16];
    gw_config_get_str(GW_KEY_C6_BUNDLE, bundle, sizeof(bundle));
    cJSON_AddStringToObject(c6, "bundle", bundle);
    cJSON_AddBoolToObject(c6, "adopted", gw_config_get_u8(GW_KEY_C6_ADOPTED, 0));
    c6fw_dir_t *dir = malloc(sizeof(*dir));
    cJSON *images = cJSON_AddArrayToObject(c6, "images");
    if (dir && c6fw_read_dir(dir) == ESP_OK) {
        for (uint32_t i = 0; i < dir->count; i++) {
            cJSON *img = cJSON_CreateObject();
            cJSON_AddStringToObject(img, "name", dir->entries[i].name);
            cJSON_AddStringToObject(img, "version", dir->entries[i].version);
            cJSON_AddNumberToObject(img, "size", dir->entries[i].size);
            cJSON_AddItemToArray(images, img);
        }
    }
    free(dir);
    const c6_progress_t *p = c6_progress();
    cJSON *prog = cJSON_AddObjectToObject(c6, "flash");
    cJSON_AddBoolToObject(prog, "busy", p->busy);
    cJSON_AddNumberToObject(prog, "percent", p->percent);
    cJSON_AddStringToObject(prog, "stage", p->stage);
    cJSON_AddStringToObject(prog, "last_error", p->last_error);
    cJSON *snaps = cJSON_AddArrayToObject(c6, "snapshots");
    static const char *const snap_bundles[] = {"zigbee_ncp", "zigbee_router"};
    for (size_t i = 0; i < sizeof(snap_bundles) / sizeof(snap_bundles[0]); i++) {
        uint32_t snap_size, snap_gen;
        if (c6_snapshot_info(snap_bundles[i], &snap_size, &snap_gen) == ESP_OK) {
            cJSON *snap = cJSON_CreateObject();
            cJSON_AddStringToObject(snap, "bundle", snap_bundles[i]);
            cJSON_AddNumberToObject(snap, "size", snap_size);
            cJSON_AddNumberToObject(snap, "generation", snap_gen);
            cJSON_AddItemToArray(snaps, snap);
        }
    }

    if (s_role_status) {
        cJSON_AddItemToObject(json, "role_status", s_role_status());
    }
    return gw_http_send_json(req, json);
}

static esp_err_t logs_get(httpd_req_t *req)
{
    char query[32], value[16];
    uint32_t seq = 0;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "since", value, sizeof(value)) == ESP_OK) {
        seq = strtoul(value, NULL, 10);
    }
    size_t cap = 16384;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!buf) {
        return gw_http_send_error(req, 500, "out of memory");
    }
    size_t n = gw_log_read(&seq, buf, cap);
    char header[16];
    snprintf(header, sizeof(header), "%lu", (unsigned long)seq);
    httpd_resp_set_hdr(req, "X-Log-Seq", header);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t err = httpd_resp_send(req, buf, n);
    free(buf);
    return err;
}

static esp_err_t role_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    gw_role_t role;
    bool ok = gw_role_from_name(GW_JSON_STR(body, "role"), &role) && role != GW_ROLE_NONE;
    bool skip_backup = cJSON_IsTrue(cJSON_GetObjectItem(body, "skip_backup"));
    cJSON_Delete(body);
    if (!ok) {
        return gw_http_send_error(req, 400, "unknown role");
    }
    char flashed[16];
    bool factory_c6 = !gw_config_get_str(GW_KEY_C6_BUNDLE, flashed, sizeof(flashed));
    if (factory_c6 && role != GW_ROLE_ESPHOME && !gw_config_get_u8(GW_KEY_C6_ADOPTED, 0)) {
        if (!skip_backup) {
            return gw_http_send_error(req, 409, "c6_backup_required");
        }
        gw_config_set_u8(GW_KEY_C6_ADOPTED, 1);
    }
    if (c6_progress()->busy) {
        return gw_http_send_error(req, 409, "C6 flashing in progress");
    }
    if (role == GW_ROLE_ESPHOME) {
        const esp_partition_t *part =
            esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL);
        esp_app_desc_t desc;
        if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) {
            return gw_http_send_error(req, 409, "no ESPHome image uploaded");
        }
    }
    gw_config_set_u8(GW_KEY_ROLE, role);
    gw_config_set_u8(GW_KEY_C6_TRIES, 0);
    ESP_LOGW(TAG, "role switch to %s requested", gw_role_name(role));
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "role", gw_role_name(role));
    cJSON_AddBoolToObject(json, "restarting", true);
    gw_restart_later(500);
    return gw_http_send_json(req, json);
}

static esp_err_t reboot_post(httpd_req_t *req)
{
    gw_restart_later(500);
    return gw_http_send_json(req, cJSON_CreateObject());
}

/* ---- uploads ---- */

esp_err_t gw_http_receive(httpd_req_t *req, gw_sink_fn sink, void *ctx)
{
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = ESP_OK;
    uint32_t received = 0;
    int timeouts = 0;
    /* Sinks get full blocks (only the last one shorter), whatever the TCP segmentation. */
    while (received < req->content_len && err == ESP_OK) {
        size_t want = MIN(UPLOAD_CHUNK, req->content_len - received);
        size_t fill = 0;
        while (fill < want) {
            int n = httpd_req_recv(req, buf + fill, want - fill);
            if (n == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 5) {
                continue;
            }
            if (n <= 0) {
                err = ESP_ERR_TIMEOUT;
                break;
            }
            timeouts = 0;
            fill += n;
        }
        if (err == ESP_OK) {
            err = sink(ctx, received, buf, fill);
        }
        received += fill;
        /* Flash writes stall both cores; a long upload must still let the idle tasks run
         * (task watchdog), whichever core this unpinned task is on. */
        vTaskDelay(1);
    }
    free(buf);
    return err;
}

typedef struct {
    const esp_partition_t *target;
    size_t size;
    bool gateway;
    bool begun;
    esp_ota_handle_t handle;
    const char *reject; /* why the image was refused before touching the slot */
} ota_ctx_t;

/*
 * Checks the image header and the project name in the first chunk, before esp_ota_begin()
 * erases the slot: a wrong file must not destroy the image already stored there.
 */
static esp_err_t ota_sink(void *arg, uint32_t offset, const void *data, size_t len)
{
    ota_ctx_t *ctx = arg;
    if (!ctx->begun) {
        const uint8_t *p = data;
        const size_t desc_offset = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
        esp_app_desc_t desc;
        if (offset != 0 || len < desc_offset + sizeof(desc) || p[0] != ESP_IMAGE_HEADER_MAGIC) {
            ctx->reject = "not an ESP application image";
            return ESP_ERR_INVALID_ARG;
        }
        memcpy(&desc, p + desc_offset, sizeof(desc));
        if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
            ctx->reject = "image has no application description";
            return ESP_ERR_INVALID_ARG;
        }
        /* The gateway slots only take this firmware; ESPHome must not land there and vice versa. */
        if ((strncmp(desc.project_name, "p4gw", sizeof(desc.project_name)) == 0) != ctx->gateway) {
            ctx->reject = ctx->gateway ? "not a p4gw image" : "this is the gateway image, not ESPHome";
            return ESP_ERR_INVALID_ARG;
        }
        esp_err_t err = esp_ota_begin(ctx->target, ctx->size, &ctx->handle);
        if (err != ESP_OK) {
            return err;
        }
        ctx->begun = true;
    }
    return esp_ota_write(ctx->handle, data, len);
}

static const esp_partition_t *other_gateway_slot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_partition_subtype_t subtype = running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0
                                          ? ESP_PARTITION_SUBTYPE_APP_OTA_1
                                          : ESP_PARTITION_SUBTYPE_APP_OTA_0;
    return esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, NULL);
}

static esp_err_t app_upload(httpd_req_t *req, const esp_partition_t *target, bool gateway)
{
    esp_app_desc_t desc;
    esp_err_t err;

    if (req->content_len == 0 || req->content_len > target->size) {
        return gw_http_send_error(req, 413, "image size does not fit the slot");
    }
    ota_ctx_t ctx = {.target = target, .size = req->content_len, .gateway = gateway};
    err = gw_http_receive(req, ota_sink, &ctx);
    if (ctx.begun) {
        err = err == ESP_OK ? esp_ota_end(ctx.handle) : (esp_ota_abort(ctx.handle), err);
    }
    if (ctx.reject) {
        return gw_http_send_error(req, 400, ctx.reject);
    }
    if (err == ESP_OK) {
        err = esp_ota_get_partition_description(target, &desc);
    }
    if (err == ESP_ERR_OTA_ROLLBACK_INVALID_STATE) {
        return gw_http_send_error(req, 409, "the running gateway image is not confirmed yet, retry in a minute");
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "upload to %s: %s", target->label, esp_err_to_name(err));
        return gw_http_send_error(req, 400, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%s <- %s %s", target->label, desc.project_name, desc.version);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "slot", target->label);
    cJSON_AddStringToObject(json, "version", desc.version);
    if (gateway) {
        err = esp_ota_set_boot_partition(target);
        cJSON_AddBoolToObject(json, "restarting", err == ESP_OK);
        if (err == ESP_OK) {
            gw_restart_later(800);
        }
    }
    return gw_http_send_json(req, json);
}

static esp_err_t locked_upload(httpd_req_t *req, esp_err_t (*fn)(httpd_req_t *))
{
    if (xSemaphoreTake(s_upload_lock, 0) != pdTRUE || c6_progress()->busy) {
        if (xSemaphoreGetMutexHolder(s_upload_lock) == xTaskGetCurrentTaskHandle()) {
            xSemaphoreGive(s_upload_lock);
        }
        return gw_http_send_error(req, 409, "another upload or C6 flash is running");
    }
    esp_err_t err = fn(req);
    xSemaphoreGive(s_upload_lock);
    return err;
}

static esp_err_t gateway_upload_inner(httpd_req_t *req)
{
    return app_upload(req, other_gateway_slot(), true);
}

static esp_err_t esphome_upload_inner(httpd_req_t *req)
{
    return app_upload(req, esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL),
                      false);
}

typedef struct {
    const esp_partition_t *part;
    uint32_t erase_len;
    bool erased;
} c6fw_sink_t;

/* Checks the directory header before erasing, so a wrong file keeps the stored bundles. */
static esp_err_t c6fw_sink(void *arg, uint32_t offset, const void *data, size_t len)
{
    c6fw_sink_t *ctx = arg;
    if (!ctx->erased) {
        uint32_t format;
        if (offset != 0 || len < 8 || memcmp(data, C6FW_DIR_MAGIC, 4) != 0) {
            return ESP_ERR_INVALID_VERSION;
        }
        memcpy(&format, (const uint8_t *)data + 4, sizeof(format));
        if (format != C6FW_FORMAT_VERSION) {
            return ESP_ERR_INVALID_VERSION;
        }
        esp_err_t err = esp_partition_erase_range(ctx->part, 0, ctx->erase_len);
        if (err != ESP_OK) {
            return err;
        }
        ctx->erased = true;
    }
    return esp_partition_write(ctx->part, offset, data, len);
}

static esp_err_t c6fw_upload_inner(httpd_req_t *req)
{
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, 0x40, NULL);
    if (!part || req->content_len == 0 || req->content_len > part->size) {
        return gw_http_send_error(req, 413, "image does not fit the c6fw partition");
    }
    c6fw_sink_t ctx = {.part = part, .erase_len = (req->content_len + 0xfff) & ~0xfff};
    esp_err_t err = gw_http_receive(req, c6fw_sink, &ctx);
    if (err == ESP_OK) {
        err = c6fw_verify_all();
    }
    if (err != ESP_OK) {
        if (ctx.erased) {
            /* Never leave a half-written image that parses as valid. */
            esp_partition_erase_range(part, 0, 0x1000);
        }
        return gw_http_send_error(req, 400, esp_err_to_name(err));
    }
    /* The boot sequence compares bundle hashes and reflashes the C6 if its bundle changed. */
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "ok", true);
    return gw_http_send_json(req, json);
}

static esp_err_t gateway_upload(httpd_req_t *req) { return locked_upload(req, gateway_upload_inner); }
static esp_err_t esphome_upload(httpd_req_t *req) { return locked_upload(req, esphome_upload_inner); }
static esp_err_t c6fw_upload(httpd_req_t *req) { return locked_upload(req, c6fw_upload_inner); }

static esp_err_t c6_reflash_post(httpd_req_t *req)
{
    gw_config_set_u8(GW_KEY_C6_FORCE, 1);
    gw_config_set_u8(GW_KEY_C6_TRIES, 0);
    gw_restart_later(500);
    return gw_http_send_json(req, cJSON_CreateObject());
}

static esp_err_t snapshot_get(httpd_req_t *req)
{
    char query[48], bundle[16] = "zigbee_ncp", disposition[80];
    uint32_t size, gen;
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
        httpd_query_key_value(query, "bundle", bundle, sizeof(bundle));
    }
    if (c6_snapshot_info(bundle, &size, &gen) != ESP_OK) {
        return gw_http_send_error(req, 404, "no Zigbee snapshot");
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        return gw_http_send_error(req, 500, "out of memory");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s_nvram_%lu.bin\"", bundle,
             (unsigned long)gen);
    httpd_resp_set_hdr(req, "Content-Disposition", disposition);
    esp_err_t err = ESP_OK;
    for (uint32_t off = 0; off < size && err == ESP_OK; off += UPLOAD_CHUNK) {
        size_t n = MIN(UPLOAD_CHUNK, size - off);
        err = c6_snapshot_read(bundle, off, buf, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

typedef struct {
    httpd_req_t *req;
    size_t sent;
} backup_ctx_t;

static esp_err_t http_sink(void *arg, const void *data, size_t len)
{
    backup_ctx_t *ctx = arg;
    if (ctx->sent == 0) {
        /* Headers only once the C6 answered, so a failed connect still gets a proper error. */
        httpd_resp_set_type(ctx->req, "application/octet-stream");
        httpd_resp_set_hdr(ctx->req, "Content-Disposition", "attachment; filename=\"c6_flash_backup.bin\"");
    }
    ctx->sent += len;
    return httpd_resp_send_chunk(ctx->req, data, len);
}

/* Full C6 flash dump, only while no role uses the C6 UART. */
static esp_err_t c6_backup_get(httpd_req_t *req)
{
    if (gw_state()->active != GW_ROLE_NONE) {
        return gw_http_send_error(req, 409, "only in maintenance mode (no active role)");
    }
    if (xSemaphoreTake(s_upload_lock, 0) != pdTRUE || c6_progress()->busy) {
        return gw_http_send_error(req, 409, "C6 busy");
    }
    backup_ctx_t ctx = {.req = req};
    esp_err_t err = c6_read_flash(http_sink, &ctx);
    xSemaphoreGive(s_upload_lock);
    if (err == ESP_OK) {
        gw_config_set_u8(GW_KEY_C6_ADOPTED, 1);
    } else {
        ESP_LOGE(TAG, "C6 backup failed: %s", esp_err_to_name(err));
        if (ctx.sent == 0) {
            return gw_http_send_error(req, 503, c6_progress()->last_error);
        }
        /* Mid-transfer: an aborted download is the error signal. */
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t settings_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *hostname = GW_JSON_STR(body, "hostname");
    const char *password = GW_JSON_STR(body, "password");
    esp_err_t err = ESP_OK;
    if (hostname) {
        size_t len = strlen(hostname);
        bool valid = len < 32;
        for (size_t i = 0; i < len && valid; i++) {
            char c = hostname[i];
            valid = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || (c == '-' && i > 0 && i + 1 < len);
        }
        if (!valid) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 400, "hostname: lowercase letters, digits and inner dashes, max 31");
        }
        err = gw_config_set_str(GW_KEY_HOSTNAME, hostname);
    }
    if (password && err == ESP_OK) {
        if (strlen(password) > 64) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 400, "password max 64 characters");
        }
        err = gw_config_set_str(GW_KEY_PASSWORD, password);
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return gw_http_send_error(req, 500, esp_err_to_name(err));
    }
    cJSON *json = cJSON_CreateObject();
    cJSON_AddBoolToObject(json, "restart_required", hostname != NULL);
    return gw_http_send_json(req, json);
}

/* ---- ping ---- */

typedef struct {
    SemaphoreHandle_t done;
    cJSON *replies;
    uint32_t received;
} ping_ctx_t;

static void ping_success(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = args;
    uint32_t elapsed;
    uint16_t seq;
    esp_ping_get_profile(hdl, ESP_PING_PROF_TIMEGAP, &elapsed, sizeof(elapsed));
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "seq", seq);
    cJSON_AddNumberToObject(r, "ms", elapsed);
    cJSON_AddItemToArray(ctx->replies, r);
    ctx->received++;
}

static void ping_timeout(esp_ping_handle_t hdl, void *args)
{
    ping_ctx_t *ctx = args;
    uint16_t seq;
    esp_ping_get_profile(hdl, ESP_PING_PROF_SEQNO, &seq, sizeof(seq));
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "seq", seq);
    cJSON_AddBoolToObject(r, "timeout", true);
    cJSON_AddItemToArray(ctx->replies, r);
}

static void ping_end(esp_ping_handle_t hdl, void *args)
{
    xSemaphoreGive(((ping_ctx_t *)args)->done);
}

static esp_err_t ping_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *host = GW_JSON_STR(body, "host");
    cJSON *count_item = cJSON_GetObjectItem(body, "count");
    int count = cJSON_IsNumber(count_item) ? count_item->valueint : 4;
    count = count < 1 ? 1 : count > 10 ? 10 : count;

    struct addrinfo hints = {.ai_family = AF_UNSPEC}, *res = NULL;
    if (!host || getaddrinfo(host, NULL, &hints, &res) != 0 || !res) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "cannot resolve host");
    }
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    if (res->ai_family == AF_INET) {
        struct in_addr a = ((struct sockaddr_in *)res->ai_addr)->sin_addr;
        inet_addr_to_ip4addr(ip_2_ip4(&cfg.target_addr), &a);
        IP_SET_TYPE(&cfg.target_addr, IPADDR_TYPE_V4);
    } else {
        struct in6_addr a = ((struct sockaddr_in6 *)res->ai_addr)->sin6_addr;
        inet6_addr_to_ip6addr(ip_2_ip6(&cfg.target_addr), &a);
        IP_SET_TYPE(&cfg.target_addr, IPADDR_TYPE_V6);
    }
    freeaddrinfo(res);
    cJSON_Delete(body);
    cfg.count = count;
    cfg.interval_ms = 500;
    cfg.timeout_ms = 1500;

    ping_ctx_t ctx = {.done = xSemaphoreCreateBinary(), .replies = cJSON_CreateArray()};
    esp_ping_callbacks_t cbs = {.cb_args = &ctx, .on_ping_success = ping_success, .on_ping_timeout = ping_timeout,
                                .on_ping_end = ping_end};
    esp_ping_handle_t ping;
    if (esp_ping_new_session(&cfg, &cbs, &ping) != ESP_OK) {
        vSemaphoreDelete(ctx.done);
        cJSON_Delete(ctx.replies);
        return gw_http_send_error(req, 500, "ping session failed");
    }
    esp_ping_start(ping);
    xSemaphoreTake(ctx.done, pdMS_TO_TICKS(count * 2000 + 2000));
    esp_ping_stop(ping);
    esp_ping_delete_session(ping);
    vSemaphoreDelete(ctx.done);

    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "target", ipaddr_ntoa(&cfg.target_addr));
    cJSON_AddNumberToObject(json, "sent", count);
    cJSON_AddNumberToObject(json, "received", ctx.received);
    cJSON_AddItemToObject(json, "replies", ctx.replies);
    return gw_http_send_json(req, json);
}

/* ---- core dump ---- */

static esp_err_t coredump_get(httpd_req_t *req)
{
    size_t addr, size;
    if (esp_core_dump_image_get(&addr, &size) != ESP_OK) {
        return gw_http_send_error(req, 404, "no core dump");
    }
    char *buf = malloc(UPLOAD_CHUNK);
    if (!buf) {
        return gw_http_send_error(req, 500, "out of memory");
    }
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"coredump.elf\"");
    esp_err_t err = ESP_OK;
    for (size_t off = 0; off < size && err == ESP_OK; off += UPLOAD_CHUNK) {
        size_t n = MIN(UPLOAD_CHUNK, size - off);
        err = esp_flash_read(NULL, buf, addr + off, n);
        if (err == ESP_OK) {
            err = httpd_resp_send_chunk(req, buf, n);
        }
    }
    free(buf);
    return err == ESP_OK ? httpd_resp_send_chunk(req, NULL, 0) : err;
}

static esp_err_t coredump_delete(httpd_req_t *req)
{
    esp_err_t err = esp_core_dump_image_erase();
    return err == ESP_OK ? gw_http_send_json(req, cJSON_CreateObject()) : gw_http_send_error(req, 500, esp_err_to_name(err));
}

esp_err_t gw_api_register(void)
{
    s_upload_lock = xSemaphoreCreateMutex();
    temperature_sensor_config_t temp_cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(10, 80);
    if (temperature_sensor_install(&temp_cfg, &s_temp) == ESP_OK) {
        temperature_sensor_enable(s_temp);
    }

    gw_http_register("/api/status", HTTP_GET, status_get);
    gw_http_register("/api/logs", HTTP_GET, logs_get);
    gw_http_register("/api/role", HTTP_POST, role_post);
    gw_http_register("/api/reboot", HTTP_POST, reboot_post);
    gw_http_register("/api/settings", HTTP_POST, settings_post);
    gw_http_register("/api/ota/gateway", HTTP_POST, gateway_upload);
    gw_http_register("/api/ota/esphome", HTTP_POST, esphome_upload);
    gw_http_register("/api/ota/c6fw", HTTP_POST, c6fw_upload);
    gw_http_register("/api/c6/reflash", HTTP_POST, c6_reflash_post);
    gw_http_register("/api/c6/zigbee-snapshot", HTTP_GET, snapshot_get);
    gw_http_register("/api/c6/backup", HTTP_GET, c6_backup_get);
    gw_http_register("/api/tools/ping", HTTP_POST, ping_post);
    gw_http_register("/api/coredump", HTTP_GET, coredump_get);
    gw_http_register("/api/coredump", HTTP_DELETE, coredump_delete);
    return ESP_OK;
}
