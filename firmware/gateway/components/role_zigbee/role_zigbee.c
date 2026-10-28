#include "role_zigbee.h"

#include <errno.h>
#include <string.h>

#include "c6_flasher.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "zb_filter.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "role_zigbee";

#define UART_PORT CONFIG_GW_C6_UART_PORT
#define UART_RX_BUF 16384
#define UART_TX_BUF 8192
#define CHUNK 1024
#define BRIDGE_PRIO 15
#define ROUTER_LINE_MAX 512

static bool s_coordinator;

/* ---- statistics ---- */
static struct {
    uint64_t uart_rx;     /* bytes C6 -> host */
    uint64_t uart_tx;     /* bytes host -> C6 */
    uint32_t frames_rx;   /* ZBOSS frame signatures seen from the C6 */
    uint32_t frames_tx;
    uint32_t dropped;     /* bytes from the C6 while no client was connected */
    uint32_t uart_errors; /* FIFO / buffer overflows */
    uint32_t sessions;
    uint32_t c6_resets;
    int64_t last_rx_us;
    int64_t last_tx_us;
    int64_t session_start_us;
} s_stats;

static int s_client = -1;
static char s_client_addr[48];
static SemaphoreHandle_t s_client_lock;
static QueueHandle_t s_uart_queue;

/* ZBOSS NCP frames start with 0xDE 0xAD; count them for a rough frame rate. */
static uint32_t count_signatures(const uint8_t *data, int len, uint8_t *prev)
{
    uint32_t n = 0;
    for (int i = 0; i < len; i++) {
        if (*prev == 0xde && data[i] == 0xad) {
            n++;
        }
        *prev = data[i];
    }
    return n;
}

static void uart_setup(void)
{
    uart_config_t cfg = {
        .baud_rate = CONFIG_GW_C6_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(UART_PORT, UART_RX_BUF, UART_TX_BUF, 32, &s_uart_queue, 0));
    ESP_ERROR_CHECK(uart_param_config(UART_PORT, &cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_PORT, CONFIG_GW_C6_UART_TX, CONFIG_GW_C6_UART_RX, UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));
    /* Wake the reader early: deliver bytes as soon as the line goes idle for ~2 characters. */
    uart_set_rx_timeout(UART_PORT, 2);
    uart_set_rx_full_threshold(UART_PORT, 64);
}

static void reset_c6_and_flush(void)
{
    c6_reset();
    s_stats.c6_resets++;
    /* Drop the ROM boot banner (115200 Bd garbage at our baud rate). */
    vTaskDelay(pdMS_TO_TICKS(300));
    uart_flush_input(UART_PORT);
    xQueueReset(s_uart_queue);
}

/* ---- coordinator: UART <-> TCP bridge ---- */

/* Only shuts the socket down; its client_task wakes from recv() and closes it. */
static void close_client_locked(void)
{
    if (s_client >= 0) {
        shutdown(s_client, SHUT_RDWR);
        s_client = -1;
        ESP_LOGI(TAG, "client %s disconnected", s_client_addr);
    }
}

static zb_filter_t *s_filter; /* owned by uart_to_tcp_task, counters read by the status */

static void on_telemetry(const char *json, void *ctx)
{
    gw_c6_telemetry_set(json);
}

/* Sends coordinator bytes (telemetry frames already removed) to the connected host. */
static void forward_to_host(const uint8_t *buf, int len, uint8_t *prev)
{
    if (len <= 0) {
        return;
    }
    s_stats.frames_rx += count_signatures(buf, len, prev);
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client < 0) {
        s_stats.dropped += len;
    } else {
        for (int sent = 0; sent < len;) {
            int n = send(s_client, buf + sent, len - sent, 0);
            if (n < 0) {
                if (errno == EAGAIN || errno == EINTR) {
                    vTaskDelay(1);
                    continue;
                }
                ESP_LOGW(TAG, "send: errno %d", errno);
                close_client_locked();
                break;
            }
            sent += n;
        }
    }
    xSemaphoreGive(s_client_lock);
}

static void uart_to_tcp_task(void *arg)
{
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    uint8_t *out = heap_caps_malloc(CHUNK + ZB_FILTER_OUT_EXTRA, MALLOC_CAP_INTERNAL);
    zb_filter_t *filter = s_filter = heap_caps_malloc(sizeof(zb_filter_t), MALLOC_CAP_INTERNAL);
    uint8_t prev = 0;
    uart_event_t event;

    zb_filter_init(filter, on_telemetry, NULL);
    while (true) {
        /* Bytes held back by the telemetry filter go out once the line is idle for 5 ms. */
        TickType_t wait = zb_filter_pending(filter) ? pdMS_TO_TICKS(5) : portMAX_DELAY;
        if (xQueueReceive(s_uart_queue, &event, wait) != pdTRUE) {
            forward_to_host(out, zb_filter_flush(filter, out), &prev);
            continue;
        }
        if (event.type == UART_FIFO_OVF || event.type == UART_BUFFER_FULL) {
            s_stats.uart_errors++;
            ESP_LOGW(TAG, "UART overflow (%d)", event.type);
            uart_flush_input(UART_PORT);
            xQueueReset(s_uart_queue);
            zb_filter_init(filter, on_telemetry, NULL);
            continue;
        }
        if (event.type != UART_DATA) {
            continue;
        }
        size_t avail = 0;
        uart_get_buffered_data_len(UART_PORT, &avail);
        while (avail > 0) {
            int len = uart_read_bytes(UART_PORT, buf, avail < CHUNK ? avail : CHUNK, 0);
            if (len <= 0) {
                break;
            }
            avail -= len;
            s_stats.uart_rx += len;
            s_stats.last_rx_us = esp_timer_get_time();
            forward_to_host(out, zb_filter_run(filter, buf, len, out), &prev);
        }
    }
}

static void client_task(void *arg)
{
    int sock = (int)(intptr_t)arg;
    uint8_t *buf = heap_caps_malloc(CHUNK, MALLOC_CAP_INTERNAL);
    uint8_t prev = 0;

    while (true) {
        int len = recv(sock, buf, CHUNK, 0);
        if (len <= 0) {
            break;
        }
        uart_write_bytes(UART_PORT, buf, len);
        s_stats.uart_tx += len;
        s_stats.frames_tx += count_signatures(buf, len, &prev);
        s_stats.last_tx_us = esp_timer_get_time();
    }
    xSemaphoreTake(s_client_lock, portMAX_DELAY);
    if (s_client == sock) {
        close_client_locked();
    }
    xSemaphoreGive(s_client_lock);
    close(sock);
    free(buf);
    vTaskDelete(NULL);
}

static void server_task(void *arg)
{
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(CONFIG_GW_ZIGBEE_TCP_PORT)};
    int listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    if (listener < 0 || bind(listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listener, 1) != 0) {
        gw_set_maintenance("Zigbee TCP port %d unavailable", CONFIG_GW_ZIGBEE_TCP_PORT);
        vTaskDelete(NULL);
    }
    ESP_LOGI(TAG, "ZBOSS NCP on tcp port %d", CONFIG_GW_ZIGBEE_TCP_PORT);

    while (true) {
        struct sockaddr_storage peer;
        socklen_t peer_len = sizeof(peer);
        int sock = accept(listener, (struct sockaddr *)&peer, &peer_len);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        int idle = 10, interval = 5, count = 3;
        setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
        setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
        struct timeval tv = {.tv_sec = 2};
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

        /* A new client wins: the previous one is usually a restarted Zigbee2MQTT. */
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        close_client_locked();
        if (peer.ss_family == AF_INET6) {
            inet6_ntoa_r(((struct sockaddr_in6 *)&peer)->sin6_addr, s_client_addr, sizeof(s_client_addr));
        } else {
            inet_ntoa_r(((struct sockaddr_in *)&peer)->sin_addr, s_client_addr, sizeof(s_client_addr));
        }
        uart_flush_input(UART_PORT);
        s_client = sock;
        s_stats.sessions++;
        s_stats.session_start_us = esp_timer_get_time();
        xSemaphoreGive(s_client_lock);
        ESP_LOGI(TAG, "client %s connected", s_client_addr);

        if (xTaskCreate(client_task, "zb_client", 4096, (void *)(intptr_t)sock, BRIDGE_PRIO, NULL) != pdPASS) {
            xSemaphoreTake(s_client_lock, portMAX_DELAY);
            close_client_locked();
            xSemaphoreGive(s_client_lock);
            close(sock);
        }
    }
}

/* ---- router: line protocol with the C6 router firmware ---- */

static SemaphoreHandle_t s_router_lock;
static char s_router_status[ROUTER_LINE_MAX];
static int64_t s_router_status_us;

static void router_reader_task(void *arg)
{
    char line[ROUTER_LINE_MAX];
    size_t len = 0;
    uint8_t c;
    while (true) {
        if (uart_read_bytes(UART_PORT, &c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        s_stats.uart_rx++;
        if (c == '\n' || c == '\r') {
            line[len] = '\0';
            if (len > 4 && strncmp(line, "@ZR ", 4) == 0) {
                xSemaphoreTake(s_router_lock, portMAX_DELAY);
                strlcpy(s_router_status, line + 4, sizeof(s_router_status));
                s_router_status_us = esp_timer_get_time();
                xSemaphoreGive(s_router_lock);
            } else if (len > 4 && strncmp(line, "@ZT ", 4) == 0) {
                gw_c6_telemetry_set(line + 4);
            } else if (len > 0) {
                ESP_LOGI("c6", "%s", line);
            }
            len = 0;
        } else if (len < sizeof(line) - 1 && c >= 0x20 && c < 0x7f) {
            line[len++] = c;
        }
    }
}

static esp_err_t router_command_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    static const char *const allowed[] = {"status", "steer", "permit", "leave", "factory_reset"};
    const char *cmd = GW_JSON_STR(body, "cmd");
    bool ok = false;
    for (size_t i = 0; cmd && i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        ok |= strcmp(cmd, allowed[i]) == 0;
    }
    if (!ok) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "cmd: status, steer, permit, leave, factory_reset");
    }
    char line[32];
    int n = snprintf(line, sizeof(line), "%s\n", cmd);
    cJSON_Delete(body);
    uart_write_bytes(UART_PORT, line, n);
    s_stats.uart_tx += n;
    return gw_http_send_json(req, cJSON_CreateObject());
}

/* ---- API ---- */

static double age_s(int64_t us)
{
    return us ? (esp_timer_get_time() - us) / 1e6 : -1;
}

static cJSON *zigbee_status(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", s_coordinator ? "coordinator" : "router");
    cJSON_AddNumberToObject(json, "uart_baud", CONFIG_GW_C6_UART_BAUD);
    cJSON_AddNumberToObject(json, "rx_bytes", (double)s_stats.uart_rx);
    cJSON_AddNumberToObject(json, "tx_bytes", (double)s_stats.uart_tx);
    cJSON_AddNumberToObject(json, "uart_errors", s_stats.uart_errors);
    cJSON_AddNumberToObject(json, "c6_resets", s_stats.c6_resets);
    if (s_coordinator) {
        cJSON_AddNumberToObject(json, "tcp_port", CONFIG_GW_ZIGBEE_TCP_PORT);
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        cJSON_AddBoolToObject(json, "client_connected", s_client >= 0);
        if (s_client >= 0) {
            cJSON_AddStringToObject(json, "client", s_client_addr);
            cJSON_AddNumberToObject(json, "session_s", age_s(s_stats.session_start_us));
        }
        xSemaphoreGive(s_client_lock);
        cJSON_AddNumberToObject(json, "sessions", s_stats.sessions);
        cJSON_AddNumberToObject(json, "frames_rx", s_stats.frames_rx);
        cJSON_AddNumberToObject(json, "frames_tx", s_stats.frames_tx);
        cJSON_AddNumberToObject(json, "dropped_bytes", s_stats.dropped);
        if (s_filter) {
            cJSON_AddNumberToObject(json, "telemetry_frames", s_filter->frames);
            cJSON_AddNumberToObject(json, "telemetry_bad", s_filter->bad_frames);
        }
        cJSON_AddNumberToObject(json, "last_rx_s", age_s(s_stats.last_rx_us));
        cJSON_AddNumberToObject(json, "last_tx_s", age_s(s_stats.last_tx_us));
    } else {
        xSemaphoreTake(s_router_lock, portMAX_DELAY);
        cJSON *router = cJSON_Parse(s_router_status);
        double age = age_s(s_router_status_us);
        xSemaphoreGive(s_router_lock);
        cJSON_AddItemToObject(json, "router", router ? router : cJSON_CreateNull());
        cJSON_AddNumberToObject(json, "router_status_age_s", age);
    }
    return json;
}

static esp_err_t status_get(httpd_req_t *req)
{
    return gw_http_send_json(req, zigbee_status());
}

static esp_err_t reset_post(httpd_req_t *req)
{
    if (s_coordinator) {
        /* The host must re-initialise the NCP anyway, so drop it as well. */
        xSemaphoreTake(s_client_lock, portMAX_DELAY);
        close_client_locked();
        xSemaphoreGive(s_client_lock);
    }
    reset_c6_and_flush();
    return gw_http_send_json(req, cJSON_CreateObject());
}

esp_err_t role_zigbee_start(bool coordinator)
{
    s_coordinator = coordinator;
    s_client_lock = xSemaphoreCreateMutex();
    s_router_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_client_lock && s_router_lock, ESP_ERR_NO_MEM, TAG, "no mem");

    uart_setup();
    reset_c6_and_flush();

    gw_http_register("/api/zigbee/status", HTTP_GET, status_get);
    gw_http_register("/api/zigbee/reset", HTTP_POST, reset_post);
    gw_api_set_role_status(zigbee_status);

    if (coordinator) {
        xTaskCreate(uart_to_tcp_task, "zb_uart", 4096, NULL, BRIDGE_PRIO, NULL);
        xTaskCreate(server_task, "zb_server", 4096, NULL, BRIDGE_PRIO - 1, NULL);
    } else {
        gw_http_register("/api/zigbee/command", HTTP_POST, router_command_post);
        xTaskCreate(router_reader_task, "zb_router", 4096, NULL, 6, NULL);
    }
    return ESP_OK;
}
