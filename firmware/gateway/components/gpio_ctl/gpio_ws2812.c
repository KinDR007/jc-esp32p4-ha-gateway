#include <stdlib.h>
#include <string.h>

#include "driver/rmt_tx.h"
#include "esp_log.h"
#include "gpio_ctl_priv.h"

static const char *TAG = "gpio_ws2812";

/*
 * WS2812 (and compatible: SK6812 RGB, WS2813) LED strip on one pin through an RMT TX channel: 800 kHz,
 * 0 = 0.3 us high + 0.9 us low, 1 = 0.9 + 0.3, a frame ends with the line low for more than 50 us
 * (every frame here is at least 50 ms after the previous one).
 */

#define RES_HZ 10000000 /* 0.1 us ticks */

struct gpio_strip {
    rmt_channel_handle_t chan;
    rmt_encoder_handle_t enc;
    uint16_t count;
    uint8_t order;
    uint8_t *buf;
};

/* byte positions of r, g, b in a LED's three bytes, by order */
static const uint8_t s_pos[][3] = {
    [WS_GRB] = {1, 0, 2}, [WS_RGB] = {0, 1, 2}, [WS_BRG] = {1, 2, 0}, [WS_RBG] = {0, 2, 1},
};

gpio_strip_t *gpio_ws2812_start(int gpio, int count, int order, char *why, size_t len)
{
    gpio_strip_t *st = calloc(1, sizeof(*st));
    if (st) {
        st->buf = calloc(count, 3);
    }
    if (!st || !st->buf) {
        snprintf(why, len, "no memory for %d LEDs", count);
        gpio_ws2812_stop(st);
        return NULL;
    }
    st->count = count;
    st->order = order;
    rmt_tx_channel_config_t cc = {.gpio_num = gpio, .clk_src = RMT_CLK_SRC_DEFAULT, .resolution_hz = RES_HZ,
                                  .mem_block_symbols = 48, .trans_queue_depth = 2};
    esp_err_t err = rmt_new_tx_channel(&cc, &st->chan);
    if (err == ESP_OK) {
        rmt_bytes_encoder_config_t ec = {
            .bit0 = {.level0 = 1, .duration0 = 3, .level1 = 0, .duration1 = 9},
            .bit1 = {.level0 = 1, .duration0 = 9, .level1 = 0, .duration1 = 3},
            .flags.msb_first = 1,
        };
        err = rmt_new_bytes_encoder(&ec, &st->enc);
    }
    if (err == ESP_OK) {
        err = rmt_enable(st->chan);
    }
    if (err != ESP_OK) {
        snprintf(why, len, err == ESP_ERR_NOT_FOUND ? "no RMT channel left (2 strips at most)" : "RMT: %s", esp_err_to_name(err));
        gpio_ws2812_stop(st);
        return NULL;
    }
    return st;
}

void gpio_ws2812_stop(gpio_strip_t *st)
{
    if (!st) {
        return;
    }
    if (st->chan) {
        rmt_tx_wait_all_done(st->chan, 100);
        rmt_disable(st->chan);
        rmt_del_channel(st->chan);
    }
    if (st->enc) {
        rmt_del_encoder(st->enc);
    }
    free(st->buf);
    free(st);
}

/* hue 0-255 around the colour wheel at full saturation */
static void wheel(uint8_t h, uint8_t rgb[3])
{
    uint8_t region = h / 43, rem = (h - region * 43) * 6;
    uint8_t up = rem, down = 255 - rem;
    const uint8_t t[6][3] = {{255, up, 0}, {down, 255, 0}, {0, 255, up}, {0, down, 255}, {up, 0, 255}, {255, 0, down}};
    memcpy(rgb, t[region > 5 ? 5 : region], 3);
}

esp_err_t gpio_ws2812_show(gpio_strip_t *st, const gpio_light_t *l, uint8_t phase)
{
    if (!st) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = rmt_tx_wait_all_done(st->chan, 100); /* the buffer of the previous frame is free */
    if (err != ESP_OK) {
        return err;
    }
    for (int i = 0; i < st->count; i++) {
        uint8_t rgb[3] = {0, 0, 0};
        if (l->on) {
            if (l->effect == WS_RAINBOW) {
                wheel((uint8_t)(i * 256 / st->count + phase), rgb);
            } else {
                memcpy(rgb, l->rgb, 3);
            }
            for (int k = 0; k < 3; k++) {
                rgb[k] = (uint16_t)rgb[k] * l->brightness / 255;
            }
        }
        for (int k = 0; k < 3; k++) {
            st->buf[i * 3 + s_pos[st->order][k]] = rgb[k];
        }
    }
    rmt_transmit_config_t tc = {.loop_count = 0};
    err = rmt_transmit(st->chan, st->enc, st->buf, st->count * 3, &tc);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "transmit: %s", esp_err_to_name(err));
    }
    return err;
}
