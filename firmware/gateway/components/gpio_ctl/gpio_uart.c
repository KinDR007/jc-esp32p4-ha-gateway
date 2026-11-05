#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gpio_ctl_priv.h"
#include "gw.h"
#include "lwip/sockets.h"

static const char *TAG = "gpio_uart";

/*
 * UART-TCP bridge on two free pins of JP1 (UART4, the only UART left): one TCP client at a time, a new
 * one replaces the previous (usually the same program reconnecting). Bytes pass unchanged both ways.
 */

#define UART_PORT UART_NUM_4
#define BUF 1024
#define POLL_MS 10

static struct {
    TaskHandle_t task;
    SemaphoreHandle_t done;
    volatile bool stop;
    int listener, client;
    uint16_t port;
    char peer[48];
    uint32_t sessions;
    uint64_t rx, tx; /* bytes from the UART to the client, from the client to the UART */
} s = {.listener = -1, .client = -1};

static void drop_client(void)
{
    if (s.client >= 0) {
        close(s.client);
        s.client = -1;
        s.peer[0] = '\0';
    }
}

static void accept_client(void)
{
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    int sock = accept(s.listener, (struct sockaddr *)&peer, &len);
    if (sock < 0) {
        return;
    }
    int one = 1;
    setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    struct timeval tv = {.tv_sec = 2};
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    drop_client(); /* a new client wins */
    if (peer.ss_family == AF_INET6) {
        inet6_ntoa_r(((struct sockaddr_in6 *)&peer)->sin6_addr, s.peer, sizeof(s.peer));
    } else {
        inet_ntoa_r(((struct sockaddr_in *)&peer)->sin_addr, s.peer, sizeof(s.peer));
    }
    uart_flush_input(UART_PORT);
    s.client = sock;
    s.sessions++;
    ESP_LOGI(TAG, "client %s on port %u", s.peer, s.port);
}

static void task(void *arg)
{
    uint8_t *buf = malloc(BUF);
    while (buf && !s.stop) {
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(s.listener, &rd);
        int maxfd = s.listener;
        if (s.client >= 0) {
            FD_SET(s.client, &rd);
            maxfd = s.client > maxfd ? s.client : maxfd;
        }
        struct timeval tv = {.tv_usec = POLL_MS * 1000};
        int n = select(maxfd + 1, &rd, NULL, NULL, &tv);
        if (s.stop) {
            break;
        }
        if (n > 0 && FD_ISSET(s.listener, &rd)) {
            accept_client();
        }
        if (n > 0 && s.client >= 0 && FD_ISSET(s.client, &rd)) {
            int len = recv(s.client, buf, BUF, 0);
            if (len <= 0) {
                ESP_LOGI(TAG, "client %s gone", s.peer);
                drop_client();
            } else {
                uart_write_bytes(UART_PORT, buf, len);
                s.tx += len;
            }
        }
        int len = uart_read_bytes(UART_PORT, buf, BUF, 0);
        if (len > 0) {
            s.rx += len;
            if (s.client >= 0 && send(s.client, buf, len, 0) < 0) {
                drop_client();
            }
        }
    }
    free(buf);
    drop_client();
    xSemaphoreGive(s.done);
    vTaskDelete(NULL);
}

esp_err_t gpio_uart_start(const gpio_uart_cfg_t *c, char *why, size_t len)
{
    uart_config_t cfg = {.baud_rate = c->baud,
                         .data_bits = c->data_bits == 7 ? UART_DATA_7_BITS : UART_DATA_8_BITS,
                         .parity = c->parity == 1 ? UART_PARITY_EVEN : c->parity == 2 ? UART_PARITY_ODD : UART_PARITY_DISABLE,
                         .stop_bits = c->stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
                         .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                         .source_clk = UART_SCLK_DEFAULT};
    esp_err_t err = uart_driver_install(UART_PORT, 4096, 4096, 0, NULL, 0);
    if (err == ESP_OK && (err = uart_param_config(UART_PORT, &cfg)) == ESP_OK) {
        err = uart_set_pin(UART_PORT, c->tx, c->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        snprintf(why, len, "UART: %s", esp_err_to_name(err));
        uart_driver_delete(UART_PORT);
        return err;
    }
    gpio_set_pull_mode(c->rx, GPIO_PULLUP_ONLY); /* idle high while nothing is connected */
    struct sockaddr_in6 addr = {.sin6_family = AF_INET6, .sin6_port = htons(c->port)};
    int one = 1;
    s.listener = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s.listener >= 0) {
        setsockopt(s.listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    }
    if (s.listener < 0 || bind(s.listener, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(s.listener, 1) != 0) {
        snprintf(why, len, "TCP port %u unavailable", c->port);
        if (s.listener >= 0) {
            close(s.listener);
        }
        s.listener = -1;
        uart_driver_delete(UART_PORT);
        return ESP_FAIL;
    }
    if (!s.done) {
        s.done = xSemaphoreCreateBinary();
    }
    s.port = c->port;
    s.stop = false;
    s.rx = s.tx = 0;
    s.sessions = 0;
    if (xTaskCreate(task, "gpio_uart", 4096, NULL, 6, &s.task) != pdPASS) {
        snprintf(why, len, "no memory for the bridge task");
        close(s.listener);
        s.listener = -1;
        uart_driver_delete(UART_PORT);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "TX GPIO%d, RX GPIO%d, %lu Bd, TCP port %u", c->tx, c->rx, (unsigned long)c->baud, c->port);
    return ESP_OK;
}

void gpio_uart_stop(void)
{
    if (!s.task) {
        return;
    }
    s.stop = true;
    xSemaphoreTake(s.done, pdMS_TO_TICKS(1000));
    s.task = NULL;
    close(s.listener);
    s.listener = -1;
    uart_driver_delete(UART_PORT);
}

void gpio_uart_state(cJSON *j)
{
    cJSON_AddStringToObject(j, "client", s.peer);
    cJSON_AddNumberToObject(j, "sessions", s.sessions);
    cJSON_AddNumberToObject(j, "rx_bytes", (double)s.rx);
    cJSON_AddNumberToObject(j, "tx_bytes", (double)s.tx);
}
