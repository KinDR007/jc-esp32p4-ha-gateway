#include "netscan.h"

#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "c6_flasher.h"
#include "driver/uart.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "sdkconfig.h"

static const char *TAG = "netscan";

#define KEY_RESULT "netscan"     /* blob, last result as JSON */
#define KEY_REQUEST "netscan_req" /* u8, full scan on the next boot */
#define RESULT_MAX (24 * 1024)
#define SCANNER_BUNDLE "scanner"
#define FULL_TIMEOUT_US (90 * 1000000LL)
#define LINE_MAX 512

static SemaphoreHandle_t s_lock;
static char *s_result;          /* last result, JSON text (PSRAM) */
static char s_state[24] = "idle"; /* idle, quick, full */
static netscan_quick_fn s_quick;

static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0; /* 0 until SNTP set the clock */
}

/* Stores the result (adds time and mode) in RAM and NVS. Drops the weakest Wi-Fi entries if too big. */
static void store_result(cJSON *res, const char *mode, int64_t started_us)
{
    cJSON_AddStringToObject(res, "mode", mode);
    cJSON_AddNumberToObject(res, "at", (double)now_unix());
    cJSON_AddNumberToObject(res, "duration_ms", (double)((esp_timer_get_time() - started_us) / 1000));
    char *text = cJSON_PrintUnformatted(res);
    cJSON *wifi = cJSON_GetObjectItem(res, "wifi");
    while (text && strlen(text) >= RESULT_MAX && cJSON_GetArraySize(wifi) > 0) {
        int weakest = 0, rssi = 0;
        for (int i = 0; i < cJSON_GetArraySize(wifi); i++) {
            int r = cJSON_GetObjectItem(cJSON_GetArrayItem(wifi, i), "rssi")->valueint;
            if (i == 0 || r < rssi) {
                weakest = i;
                rssi = r;
            }
        }
        cJSON_DeleteItemFromArray(wifi, weakest);
        cJSON_free(text);
        text = cJSON_PrintUnformatted(res);
    }
    if (!text) {
        return;
    }
    char *copy = heap_caps_malloc(strlen(text) + 1, MALLOC_CAP_SPIRAM);
    if (copy) {
        strcpy(copy, text);
        xSemaphoreTake(s_lock, portMAX_DELAY);
        free(s_result);
        s_result = copy;
        xSemaphoreGive(s_lock);
    }
    if (gw_config_set_blob(KEY_RESULT, text, strlen(text) + 1) != ESP_OK) {
        ESP_LOGW(TAG, "result (%u B) not saved", (unsigned)strlen(text));
    }
    cJSON_free(text);
}

static void set_state(const char *state)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    strlcpy(s_state, state, sizeof(s_state));
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- full scan (boot) */

bool netscan_full_pending(void)
{
    if (!gw_config_get_u8(KEY_REQUEST, 0)) {
        return false;
    }
    gw_config_erase(KEY_REQUEST);
    return true;
}

static void add_error(cJSON *res, const char *msg)
{
    cJSON *errors = cJSON_GetObjectItem(res, "errors");
    if (!errors) {
        errors = cJSON_AddArrayToObject(res, "errors");
    }
    cJSON_AddItemToArray(errors, cJSON_CreateString(msg));
    ESP_LOGW(TAG, "%s", msg);
}

static esp_err_t flash_scanner(cJSON *res)
{
    c6fw_entry_t entry;
    char current[16] = "";
    if (c6fw_find(SCANNER_BUNDLE, &entry) != ESP_OK) {
        add_error(res, "the C6 firmware bundle has no scanner image, upload a current c6fw.bin");
        return ESP_ERR_NOT_FOUND;
    }
    if (!gw_config_get_str(GW_KEY_C6_BUNDLE, current, sizeof(current)) && !gw_config_get_u8(GW_KEY_C6_ADOPTED, 0)) {
        add_error(res, "the C6 still runs its factory firmware: back it up first (System, Maintenance)");
        return ESP_ERR_INVALID_STATE;
    }
    /* Zigbee network data of the current bundle is saved by the flasher and restored with the role. */
    esp_err_t err = c6_flash_bundle(&entry, current);
    if (err != ESP_OK) {
        /* unknown C6 content: make the role flash its image again */
        gw_config_set_str(GW_KEY_C6_BUNDLE, "");
        add_error(res, c6_progress()->last_error[0] ? c6_progress()->last_error : "flashing the scanner failed");
        return err;
    }
    gw_config_set_str(GW_KEY_C6_BUNDLE, entry.name);
    gw_config_set_blob(GW_KEY_C6_SHA, entry.sha256, sizeof(entry.sha256));
    return ESP_OK;
}

/* The scanner runs two Wi-Fi passes: keep one entry per BSSID, with the stronger signal. */
static bool merge_wifi(cJSON *wifi, cJSON *item)
{
    const char *bssid = GW_JSON_STR(item, "bssid");
    cJSON *ap;
    cJSON_ArrayForEach(ap, wifi)
    {
        const char *b = GW_JSON_STR(ap, "bssid");
        if (bssid && b && strcmp(b, bssid) == 0) {
            cJSON *r = cJSON_GetObjectItem(item, "rssi"), *old = cJSON_GetObjectItem(ap, "rssi");
            if (cJSON_IsNumber(r) && cJSON_IsNumber(old) && r->valueint > old->valueint) {
                cJSON_SetIntValue(old, r->valueint);
            }
            return true;
        }
    }
    return false;
}

static void collect_line(cJSON *res, const char *line)
{
    static const struct {
        const char *prefix, *array;
    } kinds[] = {{"@NW ", "wifi"}, {"@NC ", "channels"}, {"@NB ", "beacons"}, {"@NP ", "pans"}};
    for (size_t i = 0; i < sizeof(kinds) / sizeof(kinds[0]); i++) {
        if (strncmp(line, kinds[i].prefix, 4) == 0) {
            cJSON *item = cJSON_Parse(line + 4);
            cJSON *list = cJSON_GetObjectItem(res, kinds[i].array);
            if (item && i == 0 && merge_wifi(list, item)) {
                cJSON_Delete(item);
            } else if (item) {
                cJSON_AddItemToArray(list, item);
            }
            return;
        }
    }
    if (strncmp(line, "@NS ", 4) == 0) {
        cJSON *s = cJSON_Parse(line + 4);
        const char *ver = s ? GW_JSON_STR(s, "ver") : NULL;
        cJSON_AddStringToObject(res, "scanner", ver ? ver : "");
        cJSON_Delete(s);
    } else if (line[0]) {
        ESP_LOGW("c6", "%s", line);
    }
}

void netscan_run_full(void)
{
    int64_t started = esp_timer_get_time();
    cJSON *res = cJSON_CreateObject();
    for (const char *a = "wifi\0channels\0beacons\0pans\0"; *a; a += strlen(a) + 1) {
        cJSON_AddArrayToObject(res, a);
    }
    set_state("full");
    ESP_LOGI(TAG, "full scan: flashing the scanner image");
    if (flash_scanner(res) == ESP_OK) {
        const uart_port_t port = CONFIG_GW_C6_UART_PORT;
        uart_config_t cfg = {
            .baud_rate = CONFIG_GW_C6_UART_BAUD,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        char *line = malloc(LINE_MAX);
        if (line && uart_driver_install(port, 8192, 0, 0, NULL, 0) == ESP_OK) {
            uart_param_config(port, &cfg);
            uart_set_pin(port, CONFIG_GW_C6_UART_TX, CONFIG_GW_C6_UART_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
            c6_reset(); /* from the start: the flasher already let it run */
            vTaskDelay(pdMS_TO_TICKS(200));
            uart_flush_input(port);
            size_t len = 0;
            bool done = false;
            uint8_t c;
            while (!done && esp_timer_get_time() - started < FULL_TIMEOUT_US) {
                if (uart_read_bytes(port, &c, 1, pdMS_TO_TICKS(200)) != 1) {
                    continue;
                }
                if (c == '\n' || c == '\r') {
                    line[len] = '\0';
                    if (strncmp(line, "@ND ", 4) == 0) {
                        done = true;
                    } else if (len) {
                        collect_line(res, line);
                    }
                    len = 0;
                } else if (len < LINE_MAX - 1 && c >= 0x20) {
                    line[len++] = c;
                }
            }
            if (!done) {
                add_error(res, "the scanner did not finish in time, results may be incomplete");
            }
            uart_driver_delete(port);
        } else {
            add_error(res, "UART to the C6 unavailable");
        }
        free(line);
    }
    ESP_LOGI(TAG, "full scan: %d Wi-Fi, %d beacons, %d channels", cJSON_GetArraySize(cJSON_GetObjectItem(res, "wifi")),
             cJSON_GetArraySize(cJSON_GetObjectItem(res, "beacons")),
             cJSON_GetArraySize(cJSON_GetObjectItem(res, "channels")));
    /* wait briefly for SNTP so the result carries its time */
    for (int i = 0; i < 20 && !now_unix(); i++) {
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    store_result(res, "full", started);
    cJSON_Delete(res);
    set_state("idle");
}

/* ---------------------------------------------------------------- quick scan (role) */

void netscan_set_quick(netscan_quick_fn fn)
{
    s_quick = fn;
}

static void quick_task(void *arg)
{
    int64_t started = esp_timer_get_time();
    cJSON *res = cJSON_CreateObject();
    char err[96] = "";
    if (s_quick(res, err, sizeof(err)) != ESP_OK) {
        add_error(res, err[0] ? err : "quick scan failed");
    }
    store_result(res, "quick", started);
    cJSON_Delete(res);
    set_state("idle");
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- API */

static esp_err_t scan_get(httpd_req_t *req)
{
    cJSON *json = cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddStringToObject(json, "state", s_state);
    cJSON *last = s_result ? cJSON_Parse(s_result) : NULL;
    xSemaphoreGive(s_lock);
    cJSON_AddItemToObject(json, "last", last ? last : cJSON_CreateNull());
    cJSON_AddBoolToObject(json, "quick_available", s_quick != NULL);
    cJSON_AddNumberToObject(json, "now", (double)now_unix());
    return gw_http_send_json(req, json);
}

static esp_err_t scan_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *mode = GW_JSON_STR(body, "mode");
    bool quick = mode && strcmp(mode, "quick") == 0, full = mode && strcmp(mode, "full") == 0;
    cJSON_Delete(body);
    if (!quick && !full) {
        return gw_http_send_error(req, 400, "mode must be quick or full");
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool busy = strcmp(s_state, "idle") != 0;
    xSemaphoreGive(s_lock);
    if (busy) {
        return gw_http_send_error(req, 409, "a scan is running");
    }
    cJSON *json = cJSON_CreateObject();
    if (quick) {
        if (!s_quick) {
            cJSON_Delete(json);
            return gw_http_send_error(req, 409, "the quick scan needs a Thread role");
        }
        set_state("quick");
        if (xTaskCreate(quick_task, "netscan", 6144, NULL, 4, NULL) != pdPASS) {
            set_state("idle");
            cJSON_Delete(json);
            return gw_http_send_error(req, 500, "no memory");
        }
        cJSON_AddBoolToObject(json, "started", true);
    } else {
        gw_config_set_u8(KEY_REQUEST, 1);
        cJSON_AddBoolToObject(json, "restarting", true);
        gw_restart_later(800);
    }
    return gw_http_send_json(req, json);
}

void netscan_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    size_t len = 0;
    if (gw_config_blob_size(KEY_RESULT, &len) == ESP_OK && len > 1 && len <= RESULT_MAX) {
        s_result = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (s_result && !gw_config_get_blob(KEY_RESULT, s_result, len)) {
            free(s_result);
            s_result = NULL;
        }
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&sntp);
    gw_http_register("/api/net/scan", HTTP_GET, scan_get);
    gw_http_register("/api/net/scan", HTTP_POST, scan_post);
}
