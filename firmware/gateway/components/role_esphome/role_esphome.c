#include "role_esphome.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"

static const char *TAG = "role_esphome";

#define KEY_SSID "espw_ssid"
#define KEY_PASS "espw_pass" /* secret setting (gw_config) */
#define IMPROV_BAUD 115200   /* the ESPHome logger UART carries Improv */
#define REPLY_MS 3000
#define CONNECT_MS 40000

/* Improv serial (https://www.improv-wifi.com/serial/) */
enum { IMPROV_CURRENT_STATE = 1, IMPROV_ERROR_STATE = 2, IMPROV_RPC = 3, IMPROV_RPC_RESULT = 4 };
enum { RPC_WIFI_SETTINGS = 1, RPC_CURRENT_STATE = 2, RPC_DEVICE_INFO = 3 };
enum { STATE_AUTHORIZED = 2, STATE_PROVISIONING = 3, STATE_PROVISIONED = 4 };

static struct {
    c6_radio_t radio;
    TaskHandle_t task;
    int state;          /* last Improv state, 0 = unknown */
    int error;          /* last Improv error code, 0 = none */
    char device[96];    /* "ESPHome 2026.10.1 ESP32-C5 p4gw-ble-proxy-xxxxxx" */
    char url[96];       /* where the device answers, after provisioning */
    char note[96];      /* what the gateway did last */
} s_esp;

static void send_rpc(uart_port_t port, uint8_t cmd, const uint8_t *data, uint8_t len)
{
    uint8_t pkt[9 + 2 + 200 + 1];
    memcpy(pkt, "IMPROV", 6);
    pkt[6] = 1;
    pkt[7] = IMPROV_RPC;
    pkt[8] = len + 2;
    pkt[9] = cmd;
    pkt[10] = len;
    memcpy(pkt + 11, data, len);
    uint8_t sum = 0;
    for (int i = 0; i < 11 + len; i++) {
        sum += pkt[i];
    }
    pkt[11 + len] = sum;
    uart_write_bytes(port, pkt, 12 + len);
    uart_write_bytes(port, "\n", 1);
}

/* Reads Improv packets out of the log stream for up to ms; returns once one of type want came
 * (0: any), with its data in data/len. */
static bool read_packet(uart_port_t port, int want, int *got, uint8_t *data, uint8_t *len, int ms)
{
    static uint8_t buf[512];
    static int fill;
    int64_t end = esp_timer_get_time() + ms * 1000LL;
    while (esp_timer_get_time() < end) {
        int n = uart_read_bytes(port, buf + fill, sizeof(buf) - fill, pdMS_TO_TICKS(100));
        fill += n > 0 ? n : 0;
        for (int i = 0; i + 10 <= fill; i++) {
            if (memcmp(buf + i, "IMPROV", 6) != 0) {
                continue;
            }
            int plen = buf[i + 8];
            if (i + 10 + plen > fill) {
                break; /* incomplete: read more */
            }
            uint8_t sum = 0;
            for (int k = 0; k < 9 + plen; k++) {
                sum += buf[i + k];
            }
            int type = buf[i + 7];
            bool ok = sum == buf[i + 9 + plen] && (!want || type == want);
            if (ok) {
                memcpy(data, buf + i + 9, plen);
                *len = plen;
                if (got) {
                    *got = type;
                }
            }
            memmove(buf, buf + i + 10 + plen, fill - (i + 10 + plen));
            fill -= i + 10 + plen;
            i = -1;
            if (ok) {
                return true;
            }
        }
        if (fill == sizeof(buf)) {
            memmove(buf, buf + sizeof(buf) - 16, 16); /* keep a possible header start */
            fill = 16;
        }
    }
    return false;
}

/* strings of an RPC result: command, total length, then length-prefixed strings */
static void rpc_strings(const uint8_t *data, uint8_t len, char *out, size_t out_len, const char *sep)
{
    out[0] = '\0';
    size_t o = 0;
    for (int i = 2; i < len && o + 1 < out_len;) {
        int n = data[i++];
        if (i + n > len) {
            break;
        }
        o += snprintf(out + o, out_len - o, "%s%.*s", o ? sep : "", n, (const char *)data + i);
        i += n;
    }
}

static void esphome_task(void *arg)
{
    const c6_radio_pins_t *p = c6_radio_pins(s_esp.radio);
    uart_port_t port = p->uart_port;
    uart_config_t cfg = {.baud_rate = IMPROV_BAUD, .data_bits = UART_DATA_8_BITS, .parity = UART_PARITY_DISABLE,
                         .stop_bits = UART_STOP_BITS_1, .flow_ctrl = UART_HW_FLOWCTRL_DISABLE, .source_clk = UART_SCLK_DEFAULT};
    if (uart_driver_install(port, 2048, 0, 0, NULL, 0) != ESP_OK) {
        strlcpy(s_esp.note, "UART of the radio unavailable", sizeof(s_esp.note));
        s_esp.task = NULL;
        vTaskDelete(NULL);
    }
    uart_param_config(port, &cfg);
    uart_set_pin(port, p->tx, p->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    vTaskDelay(pdMS_TO_TICKS(3000)); /* ESPHome boots */

    uint8_t data[256], len = 0;
    for (int attempt = 0; attempt < 5 && !s_esp.device[0]; attempt++) {
        send_rpc(port, RPC_DEVICE_INFO, NULL, 0);
        if (read_packet(port, IMPROV_RPC_RESULT, NULL, data, &len, REPLY_MS) && len >= 2 && data[0] == RPC_DEVICE_INFO) {
            rpc_strings(data, len, s_esp.device, sizeof(s_esp.device), " ");
        }
    }
    send_rpc(port, RPC_CURRENT_STATE, NULL, 0);
    if (read_packet(port, IMPROV_CURRENT_STATE, NULL, data, &len, REPLY_MS) && len) {
        s_esp.state = data[0];
    }
    if (s_esp.state == STATE_PROVISIONED && read_packet(port, IMPROV_RPC_RESULT, NULL, data, &len, REPLY_MS)) {
        rpc_strings(data, len, s_esp.url, sizeof(s_esp.url), " ");
    }
    char ssid[33] = "", pass[65] = "";
    gw_config_get_str(KEY_SSID, ssid, sizeof(ssid));
    gw_config_get_str(KEY_PASS, pass, sizeof(pass));
    if (!s_esp.device[0] && !s_esp.state) {
        strlcpy(s_esp.note, "no Improv answer from the radio", sizeof(s_esp.note));
    } else if (s_esp.state == STATE_PROVISIONED) {
        strlcpy(s_esp.note, "on Wi-Fi", sizeof(s_esp.note));
    } else if (!ssid[0]) {
        strlcpy(s_esp.note, "Wi-Fi not set: Home Assistant > ESPHome", sizeof(s_esp.note));
    } else {
        uint8_t req[2 + 32 + 64];
        size_t sl = strlen(ssid), pl = strlen(pass);
        req[0] = sl;
        memcpy(req + 1, ssid, sl);
        req[1 + sl] = pl;
        memcpy(req + 2 + sl, pass, pl);
        send_rpc(port, RPC_WIFI_SETTINGS, req, 2 + sl + pl);
        int64_t end = esp_timer_get_time() + CONNECT_MS * 1000LL;
        s_esp.error = 0;
        while (esp_timer_get_time() < end && s_esp.state != STATE_PROVISIONED && !s_esp.error) {
            int type = 0;
            if (!read_packet(port, 0, &type, data, &len, 1000) || !len) {
                continue;
            }
            if (type == IMPROV_CURRENT_STATE) {
                s_esp.state = data[0];
            } else if (type == IMPROV_ERROR_STATE) {
                s_esp.error = data[0];
            }
        }
        /* the packets come in order: state, then the result with the URL */
        send_rpc(port, RPC_CURRENT_STATE, NULL, 0);
        if (read_packet(port, IMPROV_CURRENT_STATE, NULL, data, &len, REPLY_MS) && len) {
            s_esp.state = data[0];
        }
        if (s_esp.state == STATE_PROVISIONED && read_packet(port, IMPROV_RPC_RESULT, NULL, data, &len, REPLY_MS)) {
            rpc_strings(data, len, s_esp.url, sizeof(s_esp.url), " ");
        }
        snprintf(s_esp.note, sizeof(s_esp.note), s_esp.state == STATE_PROVISIONED ? "Wi-Fi %s set" : "Wi-Fi %s not joined",
                 ssid);
        gw_notify("system", s_esp.state == STATE_PROVISIONED ? GW_NOTIFY_INFO : GW_NOTIFY_WARNING,
                  "ESPHome proxy on radio %c: %s", 'A' + s_esp.radio, s_esp.note);
    }
    memset(pass, 0, sizeof(pass));
    uart_driver_delete(port); /* the radio runs on its own now */
    ESP_LOGI(TAG, "radio %c: %s (%s) %s", 'A' + s_esp.radio, s_esp.device, s_esp.note, s_esp.url);
    s_esp.task = NULL;
    vTaskDelete(NULL);
}

static cJSON *esphome_status(void)
{
    cJSON *j = cJSON_CreateObject();
    static const char *const names[] = {"unknown", "", "not provisioned", "provisioning", "provisioned"};
    cJSON_AddStringToObject(j, "mode", "esphome");
    cJSON_AddStringToObject(j, "improv", s_esp.state >= 0 && s_esp.state <= 4 ? names[s_esp.state] : "unknown");
    cJSON_AddStringToObject(j, "device", s_esp.device);
    cJSON_AddStringToObject(j, "url", s_esp.url);
    cJSON_AddStringToObject(j, "note", s_esp.note);
    cJSON_AddBoolToObject(j, "busy", s_esp.task != NULL);
    return j;
}

esp_err_t role_esphome_start(c6_radio_t radio)
{
    s_esp.radio = radio;
    gw_api_set_role_status(radio, esphome_status);
    return xTaskCreate(esphome_task, "esphome_improv", 4096, NULL, 4, &s_esp.task) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_err_t wifi_get(httpd_req_t *req)
{
    char ssid[33] = "", pass[65] = "";
    gw_config_get_str(KEY_SSID, ssid, sizeof(ssid));
    bool has_pass = gw_config_get_str(KEY_PASS, pass, sizeof(pass)) && pass[0];
    memset(pass, 0, sizeof(pass));
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "ssid", ssid);
    cJSON_AddBoolToObject(j, "password_set", has_pass);
    return gw_http_send_json(req, j);
}

/* {"ssid", "password"} (password left out: kept); the radio is provisioned at its next start */
static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *ssid = GW_JSON_STR(body, "ssid"), *pass = GW_JSON_STR(body, "password");
    if (!ssid || strlen(ssid) > 32 || (pass && (strlen(pass) > 64 || (pass[0] && strlen(pass) < 8)))) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "ssid: 1-32 characters, password: 8-64 characters or empty for an open network");
    }
    gw_config_set_str(KEY_SSID, ssid);
    if (pass) {
        gw_config_set_str(KEY_PASS, pass);
    }
    cJSON_Delete(body);
    gw_notify("security", GW_NOTIFY_INFO, "Wi-Fi for the ESPHome radio set by %s", gw_auth_who()[0] ? gw_auth_who() : "the web UI");
    return wifi_get(req);
}

void role_esphome_init(void)
{
    gw_http_register("/api/esphome/wifi", HTTP_GET, wifi_get);
    gw_http_register("/api/esphome/wifi", HTTP_POST, wifi_post);
}
