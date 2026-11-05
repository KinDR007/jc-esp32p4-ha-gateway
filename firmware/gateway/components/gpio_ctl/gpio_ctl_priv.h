#pragma once
/* Functions of gpio_ctl over a peripheral: UART-TCP bridge, WS2812 strip, 1-Wire temperatures. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

/* ---- UART-TCP bridge (gpio_uart.c), UART4 ---- */
typedef struct {
    int tx, rx;
    uint32_t baud;
    uint8_t data_bits; /* 7 or 8 */
    uint8_t parity;    /* 0 none, 1 even, 2 odd */
    uint8_t stop_bits; /* 1 or 2 */
    uint16_t port;
} gpio_uart_cfg_t;
esp_err_t gpio_uart_start(const gpio_uart_cfg_t *cfg, char *why, size_t len);
void gpio_uart_stop(void);
/* client, sessions, rx_bytes, tx_bytes */
void gpio_uart_state(cJSON *j);

/* ---- WS2812 strip (gpio_ws2812.c), RMT ---- */
enum { WS_GRB, WS_RGB, WS_BRG, WS_RBG };
enum { WS_SOLID, WS_RAINBOW };
typedef struct {
    bool on;
    uint8_t rgb[3];
    uint8_t brightness; /* 0-255 */
    uint8_t effect;
} gpio_light_t;
typedef struct gpio_strip gpio_strip_t;
gpio_strip_t *gpio_ws2812_start(int gpio, int count, int order, char *why, size_t len);
void gpio_ws2812_stop(gpio_strip_t *strip);
/* one frame; phase moves an effect */
esp_err_t gpio_ws2812_show(gpio_strip_t *strip, const gpio_light_t *light, uint8_t phase);

/* ---- 1-Wire DS18B20 (gpio_onewire.c) ---- */
#define OW_MAX 8
typedef struct {
    uint8_t rom[8];
    bool ok;
    float temp;
} gpio_ow_sensor_t;
typedef struct gpio_ow gpio_ow_t;
gpio_ow_t *gpio_onewire_start(int gpio, int interval_s, char *why, size_t len);
void gpio_onewire_stop(gpio_ow_t *bus);
void gpio_onewire_rescan(gpio_ow_t *bus);
/* the sensors found and their last values; changed: something changed since the last call */
int gpio_onewire_read(gpio_ow_t *bus, gpio_ow_sensor_t *out, bool *present, bool *changed);
