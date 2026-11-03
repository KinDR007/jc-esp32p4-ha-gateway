#include "mb_bus.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "mb_bus";

#define PORT CONFIG_GW_RS485_UART_PORT
#define TCP_RETRY_US (10 * 1000000LL)

static bool s_open;
static uint32_t s_baud;
static int64_t s_last_us; /* end of the last frame on the bus */
static mb_bus_stats_t s_stats;

bool mb_rtu_open(const mb_serial_t *cfg)
{
    uart_config_t uc = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = cfg->parity == 'E' ? UART_PARITY_EVEN : cfg->parity == 'O' ? UART_PARITY_ODD : UART_PARITY_DISABLE,
        .stop_bits = cfg->stop == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (!s_open) {
        if (uart_driver_install(PORT, 512, 0, 0, NULL, 0) != ESP_OK) {
            ESP_LOGE(TAG, "UART%d driver", PORT);
            return false;
        }
        uart_set_pin(PORT, CONFIG_GW_RS485_TX, CONFIG_GW_RS485_RX, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        s_open = true;
    }
    if (uart_param_config(PORT, &uc) != ESP_OK) {
        return false;
    }
    s_baud = cfg->baud;
    ESP_LOGI(TAG, "RS485 %lu %c%d", (unsigned long)cfg->baud, cfg->parity, cfg->stop);
    return true;
}

bool mb_rtu_ready(void)
{
    return s_open;
}

mb_bus_stats_t mb_rtu_stats(void)
{
    return s_stats;
}

static int read_exact(uint8_t *buf, size_t n, uint32_t timeout_ms)
{
    size_t got = 0;
    int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (got < n) {
        int64_t left = end - esp_timer_get_time();
        if (left <= 0) {
            break;
        }
        int r = uart_read_bytes(PORT, buf + got, n - got, pdMS_TO_TICKS(left / 1000) + 1);
        if (r > 0) {
            got += r;
        }
    }
    return (int)got;
}

int mb_rtu_request(uint8_t slave, const mb_req_t *req, uint16_t timeout_ms, uint16_t gap_ms, uint8_t *data, size_t *len)
{
    uint8_t pdu[MB_PDU_MAX], frame[MB_RTU_MAX];
    if (!s_open) {
        return MB_ERR_IO;
    }
    size_t plen = mb_pdu_encode(req, pdu, sizeof(pdu));
    size_t flen = plen ? mb_rtu_frame(slave, pdu, plen, frame, sizeof(frame)) : 0;
    if (!flen) {
        return MB_ERR_ARG;
    }
    /* the silent interval before the frame, and the device's own pause between requests */
    int64_t wait = s_last_us + (int64_t)gap_ms * 1000 + mb_rtu_gap_us(s_baud) - esp_timer_get_time();
    if (wait > 0) {
        vTaskDelay(pdMS_TO_TICKS(wait / 1000) + 1);
    }
    uart_flush_input(PORT);
    uart_write_bytes(PORT, frame, flen);
    uart_wait_tx_done(PORT, pdMS_TO_TICKS(500));
    s_stats.requests++;

    /* the transceiver switches direction by itself after our last bit; no echo */
    uint8_t ans[MB_RTU_MAX];
    int got = read_exact(ans, 3, timeout_ms);
    int err = MB_OK;
    if (got < 3) {
        err = got ? MB_ERR_FRAME : MB_ERR_TIMEOUT;
    } else {
        size_t want = mb_rtu_expected(ans, 3);
        if (want < 4 || want > sizeof(ans)) {
            err = MB_ERR_FRAME;
        } else {
            /* the rest arrives back to back: its transfer time and some slack */
            uint32_t ms = (uint32_t)((want - 3) * 11 * 1000 / s_baud) + 50;
            got += read_exact(ans + 3, want - 3, ms);
            if ((size_t)got != want) {
                err = MB_ERR_FRAME;
            } else {
                const uint8_t *p, *d;
                size_t pl, dl;
                err = mb_rtu_unframe(slave, ans, want, &p, &pl);
                if (err == MB_OK) {
                    err = mb_pdu_check(req, p, pl, &d, &dl);
                }
                if (err == MB_OK && data && len) {
                    size_t n = dl < *len ? dl : *len;
                    memcpy(data, d, n);
                    *len = n;
                }
            }
        }
    }
    s_last_us = esp_timer_get_time();
    if (err == MB_ERR_TIMEOUT) {
        s_stats.timeouts++;
    } else if (err == MB_ERR_CRC || err == MB_ERR_FRAME) {
        s_stats.crc_errors++;
        vTaskDelay(pdMS_TO_TICKS(20)); /* let a garbled answer end before the next frame */
        uart_flush_input(PORT);
    } else if (err > 0) {
        s_stats.exceptions++;
    }
    return err;
}

/* ---------------------------------------------------------------- TCP */

void mb_tcp_init(mb_tcp_t *c)
{
    c->sock = -1;
    c->transaction = 0;
    c->retry_us = 0;
}

void mb_tcp_close(mb_tcp_t *c)
{
    if (c->sock >= 0) {
        close(c->sock);
    }
    c->sock = -1;
}

static bool tcp_connect(mb_tcp_t *c, const char *host, uint16_t port, uint16_t timeout_ms)
{
    if (esp_timer_get_time() < c->retry_us) {
        return false;
    }
    struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM}, *res = NULL;
    char service[8];
    snprintf(service, sizeof(service), "%u", port);
    if (getaddrinfo(host, service, &hints, &res) != 0 || !res) {
        c->retry_us = esp_timer_get_time() + TCP_RETRY_US;
        return false;
    }
    int s = socket(res->ai_family, res->ai_socktype, 0);
    bool ok = s >= 0;
    if (ok) {
        fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
        int r = connect(s, res->ai_addr, res->ai_addrlen);
        if (r < 0 && errno == EINPROGRESS) {
            fd_set wr;
            FD_ZERO(&wr);
            FD_SET(s, &wr);
            struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
            int soerr = 0;
            socklen_t sl = sizeof(soerr);
            ok = select(s + 1, NULL, &wr, NULL, &tv) == 1 && getsockopt(s, SOL_SOCKET, SO_ERROR, &soerr, &sl) == 0 && soerr == 0;
        } else {
            ok = r == 0;
        }
    }
    freeaddrinfo(res);
    if (!ok) {
        if (s >= 0) {
            close(s);
        }
        c->retry_us = esp_timer_get_time() + TCP_RETRY_US;
        return false;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) & ~O_NONBLOCK);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    c->sock = s;
    return true;
}

static int recv_exact(int s, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        int r = recv(s, buf + got, n - got, 0);
        if (r <= 0) {
            return r == 0 || errno == EAGAIN || errno == EWOULDBLOCK ? MB_ERR_TIMEOUT : MB_ERR_IO;
        }
        got += r;
    }
    return MB_OK;
}

int mb_tcp_request(mb_tcp_t *c, const char *host, uint16_t port, uint8_t unit, const mb_req_t *req, uint16_t timeout_ms,
                   uint8_t *data, size_t *len)
{
    uint8_t pdu[MB_PDU_MAX], adu[MB_TCP_MAX];
    size_t plen = mb_pdu_encode(req, pdu, sizeof(pdu));
    if (!plen) {
        return MB_ERR_ARG;
    }
    if (c->sock < 0 && !tcp_connect(c, host, port, timeout_ms)) {
        return MB_ERR_IO;
    }
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    setsockopt(c->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    uint16_t tid = ++c->transaction;
    size_t alen = mb_tcp_frame(tid, unit, pdu, plen, adu, sizeof(adu));
    if (send(c->sock, adu, alen, 0) != (int)alen) {
        mb_tcp_close(c);
        return MB_ERR_IO;
    }
    int err = recv_exact(c->sock, adu, 7);
    size_t want = err == MB_OK ? mb_tcp_expected(adu) : 0;
    if (err == MB_OK && (want < 8 || want > sizeof(adu))) {
        err = MB_ERR_FRAME;
    }
    if (err == MB_OK) {
        err = recv_exact(c->sock, adu + 7, want - 7);
    }
    if (err == MB_OK) {
        const uint8_t *p, *d;
        size_t pl, dl;
        err = mb_tcp_unframe(tid, unit, adu, want, &p, &pl);
        if (err == MB_OK) {
            err = mb_pdu_check(req, p, pl, &d, &dl);
        }
        if (err == MB_OK && data && len) {
            size_t n = dl < *len ? dl : *len;
            memcpy(data, d, n);
            *len = n;
        }
    }
    if (err < 0) {
        mb_tcp_close(c); /* out of step or gone: a new connection next time */
    }
    return err;
}
