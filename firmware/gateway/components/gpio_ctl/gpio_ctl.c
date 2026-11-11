#include "gpio_ctl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gpio_ctl_priv.h"
#include "gw.h"
#include "sdkconfig.h"
#include "sensors.h"
#include "soc/soc_caps.h"

/*
 * Free GPIO of header JP1 (docs/gpio.md). Radio A's pins are never free; radio B's are while radio B
 * has no role and nothing was found on them (or they were already GPIO at start, then radio B is left
 * alone). Inputs are sampled every 10 ms with a debounce, outputs and PWM keep their logical value
 * (inverted on the pin when asked), counters run on the PCNT and report a rate per second. Changes go
 * to the push channel (MQTT topic gpio, /api/ws) for the integration.
 */

static const char *TAG = "gpio_ctl";

#define KEY_CFG "gpio_cfg"   /* str JSON {"pins": [{gpio, mode, name, ...}]} */
#define KEY_LAST "gpio_last" /* str JSON {"<gpio>": value}: last output level or PWM duty, for restore "last" */
#define KEY_PUB "gpio_pub"   /* blob pub_t[]: discovery entries on the broker, removed once their pin is gone */
#define NAME_CHARS 32
#define TICK_MS 10
#define RATE_WINDOW_US 1000000
#define COUNT_REPORT_US 5000000 /* a running counter is pushed at most this often */
#define LAST_SAVE_US 5000000    /* after the last change */
#define DEBOUNCE_MAX_MS 1000
#define PULSE_MAX_MS 3600000
#define FREQ_MAX 1000000
#define PWM_CLK_HZ 80000000 /* LEDC_USE_PLL_DIV_CLK */
#define FILTER_MAX_NS 10000
#define CNT_LIMIT 30000
#define BAUD_MIN 300
#define BAUD_MAX 3000000
#define UART_PORT_DEFAULT 6639 /* next to the Zigbee bridge's 6638 */
#define LEDS_MAX 512
#define FX_US 50000 /* a frame of an effect */
#define OW_POLL_US 500000
#define EXPOSE_INTEGRATION 1
#define EXPOSE_MQTT 2
static const char *const s_exposes[] = {"none", "integration", "mqtt", "both"}; /* by the bits */

typedef enum { M_OFF, M_INPUT, M_OUTPUT, M_PWM, M_COUNTER, M_UART, M_WS2812, M_ONEWIRE, M_I2C, M_COUNT } pin_mode_t;
static const char *const s_modes[M_COUNT] = {"off", "input", "output", "pwm", "counter", "uart", "ws2812", "onewire", "i2c"};
static const char *const s_parities[] = {"none", "even", "odd"};
static const char *const s_orders[] = {"grb", "rgb", "brg", "rbg"};
static const char *const s_effects[] = {"none", "rainbow"};
static const char *const s_pulls[] = {"none", "up", "down"};
enum { RESTORE_OFF, RESTORE_ON, RESTORE_LAST };
static const char *const s_restores[] = {"off", "on", "last"};
enum { EDGE_RISING, EDGE_FALLING, EDGE_BOTH };
static const char *const s_edges[] = {"rising", "falling", "both"};

/* header JP1, 2 x 13: odd pins in one row, even in the other (docs/hardware.md) */
typedef enum { K_3V3, K_5V, K_GND, K_NC, K_GPIO, K_I2C, K_C6 } kind_t;
static const char *const s_kinds[] = {"3v3", "5v", "gnd", "nc", "gpio", "i2c", "c6"};
static const struct {
    uint8_t pin;
    kind_t kind;
    int8_t gpio;
    const char *label;
} s_hdr[] = {
    {1, K_3V3, -1, "3.3 V"},      {2, K_5V, -1, "5 V"},           {3, K_3V3, -1, "3.3 V"},     {4, K_5V, -1, "5 V"},
    {5, K_GND, -1, "GND"},        {6, K_GND, -1, "GND"},          {7, K_GPIO, 1, "GPIO1"},     {8, K_NC, -1, ""},
    {9, K_GPIO, 2, "GPIO2"},      {10, K_GPIO, 47, "GPIO47"},     {11, K_GPIO, 3, "GPIO3"},    {12, K_GPIO, 46, "GPIO46"},
    {13, K_GPIO, 4, "GPIO4"},     {14, K_GPIO, 45, "GPIO45"},     {15, K_GPIO, 5, "GPIO5"},    {16, K_GND, -1, "GND"},
    {17, K_GPIO, 20, "GPIO20"},   {18, K_3V3, -1, "3.3 V"},       {19, K_GPIO, 32, "GPIO32"},  {20, K_C6, -1, "C6 U0RXD"},
    {21, K_GPIO, 33, "GPIO33"},   {22, K_C6, -1, "C6 U0TXD"},     {23, K_I2C, 7, "I2C SDA"},   {24, K_C6, -1, "C6 IO9"},
    {25, K_I2C, 8, "I2C SCL"},    {26, K_C6, -1, "C6 CHIP_PU"},
};
#define HDR_PINS (sizeof(s_hdr) / sizeof(s_hdr[0]))

/* what a free pin carries on the board besides JP1 */
static const char *board_note(int gpio)
{
    switch (gpio) {
    case 1:
        return "XTAL_32K_N of the chip, no 32 kHz crystal on the board";
    case 45:
        return "also switches the SD card power on the board (the gateway does not use the SD card)";
    default:
        return NULL;
    }
}

static const char *radio_a_use(int gpio)
{
    return gpio == CONFIG_GW_C6_UART_TX   ? "radio A (C6): UART TX"
           : gpio == CONFIG_GW_C6_UART_RX ? "radio A (C6): UART RX"
           : gpio == CONFIG_GW_C6_BOOT_GPIO ? "radio A (C6): BOOT"
                                            : NULL;
}

static const char *radio_b_use(int gpio)
{
    return gpio == CONFIG_GW_RADIO_B_UART_TX      ? "radio B: UART TX"
           : gpio == CONFIG_GW_RADIO_B_UART_RX    ? "radio B: UART RX"
           : gpio == CONFIG_GW_RADIO_B_RESET_GPIO ? "radio B: RESET"
           : gpio == CONFIG_GW_RADIO_B_BOOT_GPIO  ? "radio B: BOOT"
                                                  : NULL;
}

typedef struct {
    int8_t gpio;
    uint8_t mode, pull, restore, edge, expose;
    bool invert, open_drain;
    uint16_t debounce_ms;
    uint32_t freq, filter_ns;
    char name[NAME_CHARS * 3 + 1];
    /* runtime */
    bool active;
    char why[96];
    int level; /* input: debounced; output: set (logical, before inversion) */
    int raw;
    int64_t raw_us;
    float duty; /* PWM, % */
    int timer, channel;
    uint32_t res_bits;
    pcnt_unit_handle_t unit;
    pcnt_channel_handle_t chan;
    int count, win_count;
    int64_t win_us, report_us;
    float rate;
    esp_timer_handle_t pulse;
    /* uart: TX on gpio, RX on partner; i2c: SDA on gpio, SCL on partner */
    int8_t partner;
    uint32_t hz;
    i2c_master_bus_handle_t i2c;
    uint32_t baud;
    uint8_t data_bits, parity, stop_bits;
    uint16_t port;
    /* ws2812 */
    uint16_t leds;
    uint8_t order;
    gpio_light_t light;
    gpio_strip_t *strip;
    uint8_t phase;
    int64_t fx_us;
    /* onewire */
    uint16_t interval_s;
    gpio_ow_t *ow;
    int ow_n;
    bool ow_present;
    gpio_ow_sensor_t ow_dev[OW_MAX];
    int64_t ow_us;
} pin_t;

#define MAX_PINS 8
static pin_t s_pin[MAX_PINS];
static int s_npin;
static SemaphoreHandle_t s_lock;
static bool s_changed;
static int64_t s_last_dirty_us;
static cJSON *s_last; /* {"<gpio>": value} */

static size_t utf8_chars(const char *s)
{
    size_t n = 0;
    for (; *s; s++) {
        n += ((unsigned char)*s & 0xc0) != 0x80;
    }
    return n;
}

static int find_name(const char *const *names, int n, const char *s)
{
    for (int i = 0; s && i < n; i++) {
        if (!strcmp(names[i], s)) {
            return i;
        }
    }
    return -1;
}

static int header_pin(int gpio)
{
    for (size_t i = 0; i < HDR_PINS; i++) {
        if (s_hdr[i].kind == K_GPIO && s_hdr[i].gpio == gpio) {
            return s_hdr[i].pin;
        }
    }
    return 0;
}

/* NULL when the pin may be used now, else why not */
static const char *reserved(int gpio, char *buf, size_t len)
{
    if (!header_pin(gpio)) {
        return "not a GPIO of JP1";
    }
    const char *a = radio_a_use(gpio);
    if (a) {
        return a;
    }
    const char *b = radio_b_use(gpio);
    if (!b) {
        return NULL;
    }
    gw_state_t *st = gw_state();
    if (st->desired_b != GW_ROLE_NONE) {
        snprintf(buf, len, "%s (radio B has the role %s)", b, gw_role_name(st->desired_b));
        return buf;
    }
    if (st->b_gpio) {
        return NULL;
    }
    if (!st->b_probed) {
        snprintf(buf, len, "%s (the check of radio B at start has not run yet)", b);
        return buf;
    }
    if (st->b_wired) {
        snprintf(buf, len, "%s (%s connected)", b, st->b_chip[0] ? st->b_chip : "a radio is");
        return buf;
    }
    return NULL;
}

/* ---------------------------------------------------------------- last values */

static float last_value(int gpio, float def)
{
    char key[8];
    snprintf(key, sizeof(key), "%d", gpio);
    const cJSON *v = s_last ? cJSON_GetObjectItem(s_last, key) : NULL;
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static void remember(const pin_t *p, float value)
{
    if (p->restore != RESTORE_LAST || !s_last) {
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "%d", p->gpio);
    cJSON_DeleteItemFromObject(s_last, key);
    cJSON_AddNumberToObject(s_last, key, value);
    s_last_dirty_us = esp_timer_get_time();
}

static void save_last(void)
{
    char *text = cJSON_PrintUnformatted(s_last);
    if (text) {
        gw_config_set_str(KEY_LAST, text);
        cJSON_free(text);
    }
    s_last_dirty_us = 0;
}

/* light of a strip: {"on", "rgb": [r, g, b], "b", "fx"} under its GPIO */
static void last_light(const pin_t *p, gpio_light_t *l)
{
    char key[8];
    snprintf(key, sizeof(key), "%d", p->gpio);
    const cJSON *o = s_last ? cJSON_GetObjectItem(s_last, key) : NULL;
    const cJSON *rgb = cJSON_GetObjectItem(o, "rgb"), *b = cJSON_GetObjectItem(o, "b"), *fx = cJSON_GetObjectItem(o, "fx");
    l->on = cJSON_IsTrue(cJSON_GetObjectItem(o, "on"));
    for (int k = 0; k < 3 && cJSON_GetArraySize(rgb) == 3; k++) {
        l->rgb[k] = cJSON_GetArrayItem(rgb, k)->valueint;
    }
    if (cJSON_IsNumber(b)) {
        l->brightness = b->valueint;
    }
    if (cJSON_IsNumber(fx) && fx->valueint < 2) {
        l->effect = fx->valueint;
    }
}

static void remember_light(const pin_t *p)
{
    if (p->restore != RESTORE_LAST || !s_last) {
        return;
    }
    char key[8];
    snprintf(key, sizeof(key), "%d", p->gpio);
    cJSON_DeleteItemFromObject(s_last, key);
    cJSON *o = cJSON_AddObjectToObject(s_last, key);
    cJSON_AddBoolToObject(o, "on", p->light.on);
    int rgb[3] = {p->light.rgb[0], p->light.rgb[1], p->light.rgb[2]};
    cJSON_AddItemToObject(o, "rgb", cJSON_CreateIntArray(rgb, 3));
    cJSON_AddNumberToObject(o, "b", p->light.brightness);
    cJSON_AddNumberToObject(o, "fx", p->light.effect);
    s_last_dirty_us = esp_timer_get_time();
}

/* ---------------------------------------------------------------- pins */

static void pulse_end(void *arg)
{
    pin_t *p = arg;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (p->active && p->mode == M_OUTPUT) {
        p->level = !p->level;
        gpio_set_level(p->gpio, p->level ^ p->invert);
        remember(p, p->level);
        s_changed = true;
    }
    xSemaphoreGive(s_lock);
}

static void release(pin_t *p)
{
    if (p->pulse) {
        esp_timer_stop(p->pulse);
        esp_timer_delete(p->pulse);
        p->pulse = NULL;
    }
    if (p->mode == M_PWM && p->channel >= 0) {
        ledc_stop(LEDC_LOW_SPEED_MODE, p->channel, p->invert);
        ledc_timer_pause(LEDC_LOW_SPEED_MODE, p->timer);
        ledc_timer_config(&(ledc_timer_config_t){.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = p->timer, .deconfigure = true});
    }
    if (p->unit) {
        pcnt_unit_stop(p->unit);
        pcnt_unit_disable(p->unit);
        if (p->chan) {
            pcnt_del_channel(p->chan);
        }
        pcnt_del_unit(p->unit);
        p->unit = NULL;
        p->chan = NULL;
    }
    if (p->active && p->mode == M_UART) {
        gpio_uart_stop();
    }
    if (p->strip) {
        gpio_light_t dark = {0};
        gpio_ws2812_show(p->strip, &dark, 0);
        gpio_ws2812_stop(p->strip);
        p->strip = NULL;
    }
    if (p->ow) {
        gpio_onewire_stop(p->ow);
        p->ow = NULL;
    }
    if (p->i2c) {
        sensors_set_bus2(NULL, -1, -1, 0); /* its devices first */
        i2c_del_master_bus(p->i2c);
        p->i2c = NULL;
    }
    if (p->active) {
        for (int k = 0; k < 2; k++) {
            int g = k ? p->partner : p->gpio;
            if (g >= 0) {
                gpio_reset_pin(g);
                gpio_set_direction(g, GPIO_MODE_DISABLE);
                gpio_set_pull_mode(g, GPIO_FLOATING);
            }
        }
    }
    p->active = false;
    p->channel = p->timer = -1;
}

static void pulls(const pin_t *p)
{
    gpio_set_pull_mode(p->gpio, p->pull == 1 ? GPIO_PULLUP_ONLY : p->pull == 2 ? GPIO_PULLDOWN_ONLY : GPIO_FLOATING);
}

static esp_err_t pwm_set(pin_t *p, float duty)
{
    uint32_t max = (1u << p->res_bits);
    uint32_t d = (uint32_t)lroundf(duty / 100.0f * max);
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, p->channel, d > max ? max : d);
    if (err == ESP_OK) {
        err = ledc_update_duty(LEDC_LOW_SPEED_MODE, p->channel);
    }
    if (err == ESP_OK) {
        p->duty = duty;
    }
    return err;
}

static esp_err_t start_pwm(pin_t *p, uint8_t *timers, uint8_t *channels)
{
    int t = 0, c = 0;
    while (t < 4 && (*timers & (1 << t))) {
        t++;
    }
    while (c < 8 && (*channels & (1 << c))) {
        c++;
    }
    if (t == 4 || c == 8) {
        snprintf(p->why, sizeof(p->why), "no PWM timer left (4 PWM pins at most)");
        return ESP_ERR_NOT_FOUND;
    }
    /* the finest resolution the 80 MHz clock allows at this frequency (6 bits at 1 MHz, 20 at 1 Hz) */
    uint32_t bits = ledc_find_suitable_duty_resolution(PWM_CLK_HZ, p->freq);
    p->res_bits = bits > SOC_LEDC_TIMER_BIT_WIDTH ? SOC_LEDC_TIMER_BIT_WIDTH : bits;
    ledc_timer_config_t tc = {.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = t, .duty_resolution = p->res_bits,
                              .freq_hz = p->freq, .clk_cfg = LEDC_USE_PLL_DIV_CLK};
    esp_err_t err = p->res_bits ? ledc_timer_config(&tc) : ESP_ERR_INVALID_ARG;
    if (err != ESP_OK) {
        snprintf(p->why, sizeof(p->why), "PWM: %lu Hz not possible", (unsigned long)p->freq);
        return err;
    }
    ledc_channel_config_t cc = {.gpio_num = p->gpio, .speed_mode = LEDC_LOW_SPEED_MODE, .channel = c, .timer_sel = t,
                                .duty = 0, .flags.output_invert = p->invert};
    err = ledc_channel_config(&cc);
    if (err != ESP_OK) {
        ledc_timer_config(&(ledc_timer_config_t){.speed_mode = LEDC_LOW_SPEED_MODE, .timer_num = t, .deconfigure = true});
        snprintf(p->why, sizeof(p->why), "PWM channel: %s", esp_err_to_name(err));
        return err;
    }
    p->timer = t;
    p->channel = c;
    *timers |= 1 << t;
    *channels |= 1 << c;
    float duty = p->restore == RESTORE_ON ? 100 : p->restore == RESTORE_LAST ? last_value(p->gpio, 0) : 0;
    return pwm_set(p, duty < 0 ? 0 : duty > 100 ? 100 : duty);
}

static esp_err_t start_counter(pin_t *p)
{
    pcnt_unit_config_t uc = {.low_limit = -CNT_LIMIT, .high_limit = CNT_LIMIT, .flags.accum_count = true};
    esp_err_t err = pcnt_new_unit(&uc, &p->unit);
    if (err != ESP_OK) {
        snprintf(p->why, sizeof(p->why), "no pulse counter left (4 counters at most)");
        return err;
    }
    if (p->filter_ns) {
        pcnt_glitch_filter_config_t fc = {.max_glitch_ns = p->filter_ns};
        if ((err = pcnt_unit_set_glitch_filter(p->unit, &fc)) != ESP_OK) {
            snprintf(p->why, sizeof(p->why), "counter filter %lu ns: %s", (unsigned long)p->filter_ns, esp_err_to_name(err));
            return err;
        }
    }
    pcnt_chan_config_t cc = {.edge_gpio_num = p->gpio, .level_gpio_num = -1};
    if ((err = pcnt_new_channel(p->unit, &cc, &p->chan)) != ESP_OK) {
        snprintf(p->why, sizeof(p->why), "counter channel: %s", esp_err_to_name(err));
        return err;
    }
    /* the pin's rising edge is "positive"; inverted: count the other edge */
    int rising = p->edge != EDGE_FALLING, falling = p->edge != EDGE_RISING;
    if (p->invert) {
        int t = rising;
        rising = falling;
        falling = t;
    }
    pcnt_channel_set_edge_action(p->chan, rising ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD,
                                 falling ? PCNT_CHANNEL_EDGE_ACTION_INCREASE : PCNT_CHANNEL_EDGE_ACTION_HOLD);
    pcnt_unit_add_watch_point(p->unit, CNT_LIMIT);
    pcnt_unit_add_watch_point(p->unit, -CNT_LIMIT);
    pulls(p);
    if ((err = pcnt_unit_enable(p->unit)) == ESP_OK && (err = pcnt_unit_clear_count(p->unit)) == ESP_OK) {
        err = pcnt_unit_start(p->unit);
    }
    if (err != ESP_OK) {
        snprintf(p->why, sizeof(p->why), "counter start: %s", esp_err_to_name(err));
    }
    p->count = p->win_count = 0;
    p->win_us = esp_timer_get_time();
    return err;
}

/* configures one pin; on failure it stays off with the reason in why */
static void start(pin_t *p, uint8_t *timers, uint8_t *channels)
{
    char buf[96];
    const char *r = reserved(p->gpio, buf, sizeof(buf));
    if (!r && p->partner >= 0) {
        r = reserved(p->partner, buf, sizeof(buf));
    }
    p->active = false;
    p->why[0] = '\0';
    p->channel = p->timer = -1;
    if (p->mode == M_OFF) {
        return;
    }
    if (r) {
        strlcpy(p->why, r, sizeof(p->why));
        return;
    }
    esp_err_t err = ESP_OK;
    gpio_reset_pin(p->gpio);
    switch (p->mode) {
    case M_INPUT:
        gpio_set_direction(p->gpio, GPIO_MODE_INPUT);
        pulls(p);
        p->raw = p->level = gpio_get_level(p->gpio) ^ p->invert;
        p->raw_us = esp_timer_get_time();
        break;
    case M_OUTPUT:
        p->level = p->restore == RESTORE_ON ? 1 : p->restore == RESTORE_LAST ? last_value(p->gpio, 0) != 0 : 0;
        gpio_set_level(p->gpio, p->level ^ p->invert); /* before the driver is on: no glitch */
        gpio_set_direction(p->gpio, p->open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT);
        pulls(p);
        err = esp_timer_create(&(esp_timer_create_args_t){.callback = pulse_end, .arg = p, .name = "gpio_pulse"}, &p->pulse);
        break;
    case M_PWM:
        err = start_pwm(p, timers, channels);
        break;
    case M_COUNTER:
        gpio_set_direction(p->gpio, GPIO_MODE_INPUT);
        err = start_counter(p);
        break;
    case M_UART: {
        gpio_reset_pin(p->partner);
        gpio_uart_cfg_t c = {.tx = p->gpio, .rx = p->partner, .baud = p->baud, .data_bits = p->data_bits,
                             .parity = p->parity, .stop_bits = p->stop_bits, .port = p->port};
        err = gpio_uart_start(&c, p->why, sizeof(p->why));
        break;
    }
    case M_WS2812:
        p->strip = gpio_ws2812_start(p->gpio, p->leds, p->order, p->why, sizeof(p->why));
        p->light = (gpio_light_t){.on = p->restore == RESTORE_ON, .rgb = {255, 255, 255}, .brightness = 128};
        if (p->restore == RESTORE_LAST) {
            last_light(p, &p->light);
        }
        err = p->strip ? gpio_ws2812_show(p->strip, &p->light, 0) : ESP_FAIL;
        break;
    case M_ONEWIRE:
        p->ow = gpio_onewire_start(p->gpio, p->interval_s, p->why, sizeof(p->why));
        p->ow_n = 0;
        err = p->ow ? ESP_OK : ESP_FAIL;
        break;
    case M_I2C: {
        gpio_reset_pin(p->partner);
        i2c_master_bus_config_t bc = {.i2c_port = -1, .sda_io_num = p->gpio, .scl_io_num = p->partner, .clk_source = I2C_CLK_SRC_DEFAULT,
                                      .glitch_ignore_cnt = 7, .flags.enable_internal_pullup = true};
        err = i2c_new_master_bus(&bc, &p->i2c);
        if (err != ESP_OK) {
            snprintf(p->why, sizeof(p->why), err == ESP_ERR_NOT_FOUND ? "no I2C port left" : "I2C: %s", esp_err_to_name(err));
            p->i2c = NULL;
        } else {
            sensors_set_bus2(p->i2c, p->gpio, p->partner, p->hz); /* the sensors never call back into here */
        }
        break;
    }
    }
    p->active = true; /* release() resets the pin */
    if (err != ESP_OK) {
        if (!p->why[0]) {
            snprintf(p->why, sizeof(p->why), "%s", esp_err_to_name(err));
        }
        release(p);
        return;
    }
    p->report_us = 0;
}

/* ---------------------------------------------------------------- configuration */

static const char *parse_pin(const cJSON *j, pin_t *p)
{
    memset(p, 0, sizeof(*p));
    p->channel = p->timer = p->partner = -1;
    const cJSON *g = cJSON_GetObjectItem(j, "gpio");
    if (!cJSON_IsNumber(g) || !header_pin(g->valueint)) {
        return "gpio: a free GPIO of JP1 (1, 2, 3, 45; 32, 33, 46, 47 without radio B)";
    }
    if (radio_a_use(g->valueint)) {
        return radio_a_use(g->valueint);
    }
    p->gpio = g->valueint;
    int m = find_name(s_modes, M_COUNT, GW_JSON_STR(j, "mode"));
    if (m < 0) {
        return "mode: off, input, output, pwm, counter, uart, ws2812, onewire or i2c";
    }
    p->mode = m;
    const char *name = GW_JSON_STR(j, "name");
    if (name && (utf8_chars(name) > NAME_CHARS || strlen(name) >= sizeof(p->name) || strpbrk(name, "\"\\{}"))) {
        return "name: up to 32 characters, no quotes, backslashes or braces";
    }
    strlcpy(p->name, name ? name : "", sizeof(p->name));
    const char *s = GW_JSON_STR(j, "expose");
    int expose = s ? find_name(s_exposes, 4, s) : EXPOSE_INTEGRATION;
    if (expose < 0) {
        return "expose: integration, mqtt, both or none";
    }
    p->expose = expose;
    const cJSON *v;
    p->invert = cJSON_IsTrue(cJSON_GetObjectItem(j, "invert"));
    p->open_drain = cJSON_IsTrue(cJSON_GetObjectItem(j, "open_drain"));
    s = GW_JSON_STR(j, "pull");
    int pull = s ? find_name(s_pulls, 3, s) : 0;
    s = GW_JSON_STR(j, "restore");
    int restore = s ? find_name(s_restores, 3, s) : RESTORE_OFF;
    s = GW_JSON_STR(j, "edge");
    int edge = s ? find_name(s_edges, 3, s) : EDGE_RISING;
    if (pull < 0 || restore < 0 || edge < 0) {
        return "pull: none, up or down; restore: off, on or last; edge: rising, falling or both";
    }
    p->pull = pull;
    p->restore = restore;
    p->edge = edge;
    v = cJSON_GetObjectItem(j, "debounce_ms");
    p->debounce_ms = cJSON_IsNumber(v) ? v->valueint : 20;
    if (cJSON_IsNumber(v) && (v->valueint < 0 || v->valueint > DEBOUNCE_MAX_MS)) {
        return "debounce_ms: 0..1000";
    }
    v = cJSON_GetObjectItem(j, "freq");
    p->freq = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 1000;
    if (cJSON_IsNumber(v) && (v->valuedouble < 1 || v->valuedouble > FREQ_MAX)) {
        return "freq: 1..1000000 Hz";
    }
    v = cJSON_GetObjectItem(j, "filter_ns");
    p->filter_ns = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 0;
    if (cJSON_IsNumber(v) && (v->valuedouble < 0 || v->valuedouble > FILTER_MAX_NS)) {
        return "filter_ns: 0..10000";
    }
    if (p->mode == M_UART) {
        v = cJSON_GetObjectItem(j, "rx");
        if (!cJSON_IsNumber(v) || !header_pin(v->valueint) || v->valueint == p->gpio || radio_a_use(v->valueint)) {
            return "rx: a free GPIO of JP1 for the UART's RX, not the TX pin";
        }
        p->partner = v->valueint;
        v = cJSON_GetObjectItem(j, "baud");
        p->baud = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 115200;
        v = cJSON_GetObjectItem(j, "data_bits");
        p->data_bits = cJSON_IsNumber(v) ? v->valueint : 8;
        v = cJSON_GetObjectItem(j, "stop_bits");
        p->stop_bits = cJSON_IsNumber(v) ? v->valueint : 1;
        s = GW_JSON_STR(j, "parity");
        int parity = s ? find_name(s_parities, 3, s) : 0;
        v = cJSON_GetObjectItem(j, "port");
        p->port = cJSON_IsNumber(v) ? v->valueint : UART_PORT_DEFAULT;
        if (p->baud < BAUD_MIN || p->baud > BAUD_MAX || (p->data_bits != 7 && p->data_bits != 8) || (p->stop_bits != 1 && p->stop_bits != 2) || parity < 0) {
            return "uart: baud 300..3000000, data_bits 7 or 8, parity none, even or odd, stop_bits 1 or 2";
        }
        p->parity = parity;
        if (cJSON_IsNumber(v) && (v->valueint < 1024 || v->valueint > 65535 || v->valueint == 6638 || v->valueint == 8081)) {
            return "port: 1024..65535, not 6638 (Zigbee) or 8081 (OTBR)";
        }
    }
    if (p->mode == M_I2C) {
        v = cJSON_GetObjectItem(j, "scl");
        if (!cJSON_IsNumber(v) || !header_pin(v->valueint) || v->valueint == p->gpio || radio_a_use(v->valueint)) {
            return "scl: a free GPIO of JP1 for the bus's SCL, not the SDA pin";
        }
        p->partner = v->valueint;
        v = cJSON_GetObjectItem(j, "hz");
        p->hz = cJSON_IsNumber(v) ? (uint32_t)v->valuedouble : 100000;
        if (p->hz != 100000 && p->hz != 400000) {
            return "hz: 100000 or 400000";
        }
    }
    if (p->mode == M_WS2812) {
        v = cJSON_GetObjectItem(j, "leds");
        p->leds = cJSON_IsNumber(v) ? v->valueint : 30;
        s = GW_JSON_STR(j, "order");
        int order = s ? find_name(s_orders, 4, s) : WS_GRB;
        if (p->leds < 1 || p->leds > LEDS_MAX || order < 0) {
            return "ws2812: leds 1..512, order grb, rgb, brg or rbg";
        }
        p->order = order;
    }
    if (p->mode == M_ONEWIRE) {
        v = cJSON_GetObjectItem(j, "interval_s");
        p->interval_s = cJSON_IsNumber(v) ? v->valueint : 30;
        if (p->interval_s < 5 || p->interval_s > 3600) {
            return "interval_s: 5..3600";
        }
    }
    return NULL;
}

static cJSON *pin_config_json(const pin_t *p)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "gpio", p->gpio);
    cJSON_AddStringToObject(j, "mode", s_modes[p->mode]);
    cJSON_AddStringToObject(j, "name", p->name);
    cJSON_AddStringToObject(j, "expose", s_exposes[p->expose]);
    cJSON_AddBoolToObject(j, "invert", p->invert);
    cJSON_AddStringToObject(j, "pull", s_pulls[p->pull]);
    if (p->mode == M_INPUT) {
        cJSON_AddNumberToObject(j, "debounce_ms", p->debounce_ms);
    }
    if (p->mode == M_OUTPUT) {
        cJSON_AddBoolToObject(j, "open_drain", p->open_drain);
    }
    if (p->mode == M_OUTPUT || p->mode == M_PWM || p->mode == M_WS2812) {
        cJSON_AddStringToObject(j, "restore", s_restores[p->restore]);
    }
    if (p->mode == M_UART) {
        cJSON_AddNumberToObject(j, "rx", p->partner);
        cJSON_AddNumberToObject(j, "baud", p->baud);
        cJSON_AddNumberToObject(j, "data_bits", p->data_bits);
        cJSON_AddStringToObject(j, "parity", s_parities[p->parity]);
        cJSON_AddNumberToObject(j, "stop_bits", p->stop_bits);
        cJSON_AddNumberToObject(j, "port", p->port);
    }
    if (p->mode == M_WS2812) {
        cJSON_AddNumberToObject(j, "leds", p->leds);
        cJSON_AddStringToObject(j, "order", s_orders[p->order]);
    }
    if (p->mode == M_ONEWIRE) {
        cJSON_AddNumberToObject(j, "interval_s", p->interval_s);
    }
    if (p->mode == M_I2C) {
        cJSON_AddNumberToObject(j, "scl", p->partner);
        cJSON_AddNumberToObject(j, "hz", p->hz);
    }
    if (p->mode == M_PWM) {
        cJSON_AddNumberToObject(j, "freq", p->freq);
    }
    if (p->mode == M_COUNTER) {
        cJSON_AddStringToObject(j, "edge", s_edges[p->edge]);
        cJSON_AddNumberToObject(j, "filter_ns", p->filter_ns);
    }
    return j;
}

static void save(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(root, "pins");
    for (int i = 0; i < s_npin; i++) {
        cJSON_AddItemToArray(list, pin_config_json(&s_pin[i]));
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (text) {
        gw_config_set_str(KEY_CFG, text);
        cJSON_free(text);
    }
}

static bool uses_radio_b(const pin_t *list, int n)
{
    for (int i = 0; i < n; i++) {
        if (list[i].mode != M_OFF && (radio_b_use(list[i].gpio) || (list[i].partner >= 0 && radio_b_use(list[i].partner)))) {
            return true;
        }
    }
    return false;
}

/* list checked as a whole; NULL or the error */
static const char *check_list(const cJSON *pins, pin_t *out, int *n, char *buf, size_t len)
{
    if (!cJSON_IsArray(pins) || cJSON_GetArraySize(pins) > MAX_PINS) {
        return "pins: a list of up to 8 pins";
    }
    int pwm = 0, cnt = 0, uart = 0, ws = 0, ow = 0, i2c = 0;
    *n = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, pins)
    {
        const char *e = parse_pin(j, &out[*n]);
        if (e) {
            snprintf(buf, len, "pin %d: %s", *n + 1, e);
            return buf;
        }
        const pin_t *q = &out[*n];
        for (int i = 0; i < *n; i++) {
            int a[2] = {out[i].gpio, out[i].partner}, b[2] = {q->gpio, q->partner};
            for (int x = 0; x < 2; x++) {
                for (int y = 0; y < 2; y++) {
                    if (a[x] >= 0 && a[x] == b[y]) {
                        snprintf(buf, len, "GPIO%d twice", a[x]);
                        return buf;
                    }
                }
            }
        }
        char why[96];
        for (int k = 0; k < 2 && q->mode != M_OFF; k++) {
            int g = k ? q->partner : q->gpio;
            const char *r = g >= 0 ? reserved(g, why, sizeof(why)) : NULL;
            if (r) {
                snprintf(buf, len, "GPIO%d: %s", g, r);
                return buf;
            }
        }
        pwm += q->mode == M_PWM;
        cnt += q->mode == M_COUNTER;
        uart += q->mode == M_UART;
        ws += q->mode == M_WS2812;
        ow += q->mode == M_ONEWIRE;
        i2c += q->mode == M_I2C;
        (*n)++;
    }
    if (pwm > 4 || cnt > 4 || uart > 1 || ws > 2 || ow > 2 || i2c > 1) {
        return "at most 4 PWM pins, 4 counters, 1 UART, 2 WS2812 strips, 2 1-Wire buses and 1 I2C bus";
    }
    return NULL;
}

static void apply(pin_t *list, int n)
{
    for (int i = 0; i < s_npin; i++) {
        release(&s_pin[i]);
    }
    memcpy(s_pin, list, n * sizeof(pin_t));
    s_npin = n;
    uint8_t timers = 0, channels = 0;
    for (int i = 0; i < s_npin; i++) {
        start(&s_pin[i], &timers, &channels);
        if (s_pin[i].why[0]) {
            ESP_LOGW(TAG, "GPIO%d %s off: %s", s_pin[i].gpio, s_modes[s_pin[i].mode], s_pin[i].why);
        }
    }
    s_changed = true;
}

static bool load(pin_t *list, int *n)
{
    char *text = malloc(4096);
    *n = 0;
    if (!text) {
        return false;
    }
    cJSON *root = gw_config_get_str(KEY_CFG, text, 4096) ? cJSON_Parse(text) : NULL;
    free(text);
    const cJSON *pins = cJSON_GetObjectItem(root, "pins");
    const cJSON *j;
    cJSON_ArrayForEach(j, pins)
    {
        if (*n < MAX_PINS && !parse_pin(j, &list[*n])) {
            (*n)++;
        }
    }
    cJSON_Delete(root);
    return *n > 0;
}

bool gpio_ctl_claims_radio_b(void)
{
    pin_t *list = calloc(MAX_PINS, sizeof(pin_t));
    int n = 0;
    bool claims = list && load(list, &n) && uses_radio_b(list, n) && gw_state()->desired_b == GW_ROLE_NONE;
    free(list);
    return claims;
}

/* ---------------------------------------------------------------- state */

static void rom_hex(const uint8_t rom[8], char out[17])
{
    for (int i = 0; i < 8; i++) {
        snprintf(out + i * 2, 3, "%02x", rom[i]);
    }
}

/* {"state", "color": {r, g, b}, "brightness", "effect"}: also the MQTT JSON light's state */
static cJSON *light_json(const pin_t *p)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", p->light.on ? "ON" : "OFF");
    cJSON *c = cJSON_AddObjectToObject(j, "color");
    cJSON_AddNumberToObject(c, "r", p->light.rgb[0]);
    cJSON_AddNumberToObject(c, "g", p->light.rgb[1]);
    cJSON_AddNumberToObject(c, "b", p->light.rgb[2]);
    cJSON_AddNumberToObject(j, "brightness", p->light.brightness);
    cJSON_AddStringToObject(j, "effect", s_effects[p->light.effect]);
    return j;
}

/* {"<rom>": temperature or null} */
static cJSON *onewire_json(const pin_t *p)
{
    cJSON *j = cJSON_CreateObject();
    char rom[17];
    for (int i = 0; i < p->ow_n; i++) {
        rom_hex(p->ow_dev[i].rom, rom);
        if (p->ow_dev[i].ok) {
            cJSON_AddNumberToObject(j, rom, roundf(p->ow_dev[i].temp * 100) / 100);
        } else {
            cJSON_AddNullToObject(j, rom);
        }
    }
    return j;
}

static cJSON *pin_state_json(const pin_t *p)
{
    cJSON *j = pin_config_json(p);
    cJSON_AddNumberToObject(j, "jp1", header_pin(p->gpio));
    cJSON_AddBoolToObject(j, "active", p->active);
    if (p->why[0]) {
        cJSON_AddStringToObject(j, "error", p->why);
    }
    if (!p->active) {
        return j;
    }
    switch (p->mode) {
    case M_INPUT:
        cJSON_AddNumberToObject(j, "level", p->level);
        break;
    case M_OUTPUT:
        cJSON_AddNumberToObject(j, "level", p->level);
        /* what the pad reads back: differs from the level when something holds the line */
        cJSON_AddNumberToObject(j, "pin_level", gpio_get_level(p->gpio));
        break;
    case M_PWM:
        cJSON_AddNumberToObject(j, "duty", roundf(p->duty * 10) / 10);
        cJSON_AddNumberToObject(j, "resolution_bits", p->res_bits);
        break;
    case M_COUNTER:
        cJSON_AddNumberToObject(j, "count", p->count);
        /* counts per second; with both edges two per period */
        cJSON_AddNumberToObject(j, "rate", roundf(p->rate * 10) / 10);
        cJSON_AddNumberToObject(j, "frequency", roundf((p->edge == EDGE_BOTH ? p->rate / 2 : p->rate) * 10) / 10);
        break;
    case M_UART:
        gpio_uart_state(j);
        break;
    case M_WS2812:
        cJSON_AddItemToObject(j, "light", light_json(p));
        break;
    case M_ONEWIRE:
        cJSON_AddBoolToObject(j, "present", p->ow_present);
        cJSON_AddItemToObject(j, "temperatures", onewire_json(p));
        break;
    }
    return j;
}

/* {"<gpio>": level | duty | {count, rate}} of active pins, for the push channel; under s_lock */
static cJSON *values_json(void)
{
    cJSON *j = cJSON_CreateObject();
    char key[8];
    for (int i = 0; i < s_npin; i++) {
        const pin_t *p = &s_pin[i];
        if (!p->active) {
            continue;
        }
        snprintf(key, sizeof(key), "%d", p->gpio);
        if (p->mode == M_PWM) {
            cJSON_AddNumberToObject(j, key, roundf(p->duty * 10) / 10);
        } else if (p->mode == M_COUNTER) {
            cJSON *c = cJSON_AddObjectToObject(j, key);
            cJSON_AddNumberToObject(c, "count", p->count);
            cJSON_AddNumberToObject(c, "rate", roundf(p->rate * 10) / 10);
            cJSON_AddNumberToObject(c, "frequency", roundf((p->edge == EDGE_BOTH ? p->rate / 2 : p->rate) * 10) / 10);
        } else if (p->mode == M_WS2812) {
            cJSON_AddItemToObject(j, key, light_json(p));
        } else if (p->mode == M_ONEWIRE) {
            cJSON_AddItemToObject(j, key, onewire_json(p));
        } else if (p->mode != M_UART && p->mode != M_I2C) {
            cJSON_AddNumberToObject(j, key, p->level);
        }
    }
    return j;
}

static void push(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *v = values_json();
    xSemaphoreGive(s_lock);
    char *text = cJSON_PrintUnformatted(v), topic[24];
    if (text) {
        gw_mqtt_publish("gpio", text, true); /* also the push WebSocket of the integration; retained for MQTT */
        cJSON_free(text);
    }
    /* a strip's state also on its own topic: the MQTT JSON light reads the whole payload */
    cJSON *item;
    cJSON_ArrayForEach(item, v)
    {
        if (cJSON_GetObjectItem(item, "effect")) {
            snprintf(topic, sizeof(topic), "gpio/%s/light", item->string);
            char *one = cJSON_PrintUnformatted(item);
            if (one) {
                gw_mqtt_publish(topic, one, true);
                cJSON_free(one);
            }
        }
    }
    cJSON_Delete(v);
}

static void task(void *arg)
{
    TickType_t wake = xTaskGetTickCount();
    while (true) {
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(TICK_MS));
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        for (int i = 0; i < s_npin; i++) {
            pin_t *p = &s_pin[i];
            if (!p->active) {
                continue;
            }
            if (p->mode == M_INPUT) {
                int raw = gpio_get_level(p->gpio) ^ p->invert;
                if (raw != p->raw) {
                    p->raw = raw;
                    p->raw_us = now;
                }
                if (p->raw != p->level && now - p->raw_us >= (int64_t)p->debounce_ms * 1000) {
                    p->level = p->raw;
                    s_changed = true;
                }
            } else if (p->mode == M_WS2812 && p->light.on && p->light.effect != WS_SOLID && now - p->fx_us >= FX_US) {
                p->fx_us = now;
                gpio_ws2812_show(p->strip, &p->light, p->phase += 2);
            } else if (p->mode == M_ONEWIRE && now - p->ow_us >= OW_POLL_US) {
                bool changed = false;
                int before = p->ow_n;
                p->ow_us = now;
                p->ow_n = gpio_onewire_read(p->ow, p->ow_dev, &p->ow_present, &changed);
                s_changed |= changed;
                if (p->ow_n != before) {
                    gw_mqtt_rediscover(); /* sensors found or gone */
                }
            } else if (p->mode == M_COUNTER && now - p->win_us >= RATE_WINDOW_US) {
                int count = 0;
                pcnt_unit_get_count(p->unit, &count);
                p->rate = (float)(count - p->win_count) * 1e6f / (float)(now - p->win_us);
                p->win_count = count;
                p->win_us = now;
                if ((count != p->count || p->rate != 0) && now - p->report_us >= COUNT_REPORT_US) {
                    p->report_us = now;
                    s_changed = true;
                }
                p->count = count;
            }
        }
        if (s_last_dirty_us && now - s_last_dirty_us >= LAST_SAVE_US) {
            save_last();
        }
        bool changed = s_changed;
        s_changed = false;
        xSemaphoreGive(s_lock);
        if (changed) {
            push();
        }
    }
}

/* ---------------------------------------------------------------- API */

static cJSON *status_json(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *hdr = cJSON_AddArrayToObject(root, "header");
    char buf[96];
    for (size_t i = 0; i < HDR_PINS; i++) {
        cJSON *h = cJSON_CreateObject();
        cJSON_AddNumberToObject(h, "pin", s_hdr[i].pin);
        cJSON_AddStringToObject(h, "kind", s_kinds[s_hdr[i].kind]);
        cJSON_AddStringToObject(h, "label", s_hdr[i].label);
        if (s_hdr[i].gpio >= 0) {
            cJSON_AddNumberToObject(h, "gpio", s_hdr[i].gpio);
        }
        if (s_hdr[i].kind == K_GPIO) {
            const char *r = reserved(s_hdr[i].gpio, buf, sizeof(buf));
            cJSON_AddBoolToObject(h, "free", !r);
            if (r) {
                cJSON_AddStringToObject(h, "reserved", r);
            }
            if (board_note(s_hdr[i].gpio)) {
                cJSON_AddStringToObject(h, "note", board_note(s_hdr[i].gpio));
            }
        }
        cJSON_AddItemToArray(hdr, h);
    }
    cJSON *list = cJSON_AddArrayToObject(root, "pins");
    for (int i = 0; i < s_npin; i++) {
        cJSON_AddItemToArray(list, pin_state_json(&s_pin[i]));
    }
    cJSON_AddBoolToObject(root, "radio_b_gpio", gw_state()->b_gpio);
    return root;
}

static esp_err_t gpio_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *j = status_json();
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

/* {"pins": [...]}: replaces the configuration, applied at once */
static esp_err_t gpio_put(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    pin_t *list = calloc(MAX_PINS, sizeof(pin_t));
    if (!list) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 500, "no memory");
    }
    char buf[160];
    int n = 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const char *err = check_list(cJSON_GetObjectItem(body, "pins"), list, &n, buf, sizeof(buf));
    cJSON_Delete(body);
    if (err) {
        xSemaphoreGive(s_lock);
        free(list);
        return gw_http_send_error(req, 400, err);
    }
    /* radio B's pins taken or given back: radio B is left alone while they are GPIO */
    gw_state()->b_gpio = uses_radio_b(list, n);
    apply(list, n);
    save();
    cJSON *j = status_json();
    xSemaphoreGive(s_lock);
    free(list);
    ESP_LOGI(TAG, "%d pins configured by %s", n, gw_auth_who());
    gw_mqtt_rediscover();
    return gw_http_send_json(req, j);
}

static pin_t *find(int gpio)
{
    for (int i = 0; i < s_npin; i++) {
        if (s_pin[i].gpio == gpio) {
            return &s_pin[i];
        }
    }
    return NULL;
}

/* light of a strip from {"on" | "state", "color": [r, g, b] | {r, g, b}, "brightness", "effect"} */
static const char *set_light(pin_t *p, const cJSON *b)
{
    gpio_light_t l = p->light;
    const cJSON *on = cJSON_GetObjectItem(b, "on"), *color = cJSON_GetObjectItem(b, "color"), *br = cJSON_GetObjectItem(b, "brightness");
    const char *state = GW_JSON_STR(b, "state"), *effect = GW_JSON_STR(b, "effect");
    if (cJSON_IsBool(on)) {
        l.on = cJSON_IsTrue(on);
    } else if (state) {
        l.on = !strcasecmp(state, "ON");
    }
    if (color) {
        const cJSON *c[3] = {cJSON_GetArrayItem(color, 0), cJSON_GetArrayItem(color, 1), cJSON_GetArrayItem(color, 2)};
        if (cJSON_IsObject(color)) {
            c[0] = cJSON_GetObjectItem(color, "r");
            c[1] = cJSON_GetObjectItem(color, "g");
            c[2] = cJSON_GetObjectItem(color, "b");
        }
        for (int k = 0; k < 3; k++) {
            if (!cJSON_IsNumber(c[k]) || c[k]->valueint < 0 || c[k]->valueint > 255) {
                return "color: [r, g, b] or {r, g, b}, 0..255";
            }
            l.rgb[k] = c[k]->valueint;
        }
    }
    if (br) {
        if (!cJSON_IsNumber(br) || br->valueint < 0 || br->valueint > 255) {
            return "brightness: 0..255";
        }
        l.brightness = br->valueint;
    }
    if (effect) {
        int fx = find_name(s_effects, 2, effect);
        if (fx < 0) {
            return "effect: none or rainbow";
        }
        l.effect = fx;
    }
    /* a colour or brightness alone turns the strip on, as Home Assistant expects */
    if ((color || br) && !cJSON_IsBool(on) && !state) {
        l.on = true;
    }
    if (gpio_ws2812_show(p->strip, &l, p->phase) != ESP_OK) {
        return "LED strip update failed";
    }
    p->light = l;
    remember_light(p);
    return NULL;
}

/* {"level" | "pulse_ms" | "duty" | "reset" | light fields | "rescan"} for the pin's mode; NULL or the error */
static const char *set_value(pin_t *p, const cJSON *b)
{
    const cJSON *level = cJSON_GetObjectItem(b, "level"), *duty = cJSON_GetObjectItem(b, "duty"),
                *pulse_ms = cJSON_GetObjectItem(b, "pulse_ms");
    if (!p || !p->active) {
        return "no active pin with this GPIO";
    }
    if (p->mode == M_OUTPUT && cJSON_IsNumber(pulse_ms)) {
        if (pulse_ms->valueint < 1 || pulse_ms->valueint > PULSE_MAX_MS) {
            return "pulse_ms: 1..3600000";
        }
        /* a pulse running: only its end moves */
        if (esp_timer_is_active(p->pulse)) {
            esp_timer_stop(p->pulse);
        } else {
            p->level = !p->level;
            gpio_set_level(p->gpio, p->level ^ p->invert);
        }
        esp_timer_start_once(p->pulse, (uint64_t)pulse_ms->valueint * 1000);
        return NULL;
    }
    if (p->mode == M_OUTPUT && (cJSON_IsNumber(level) || cJSON_IsBool(level))) {
        esp_timer_stop(p->pulse); /* a level ends a pulse */
        p->level = cJSON_IsBool(level) ? cJSON_IsTrue(level) : level->valueint != 0;
        gpio_set_level(p->gpio, p->level ^ p->invert);
        remember(p, p->level);
        return NULL;
    }
    if (p->mode == M_PWM && cJSON_IsNumber(duty)) {
        if (duty->valuedouble < 0 || duty->valuedouble > 100) {
            return "duty: 0..100 %";
        }
        if (pwm_set(p, (float)duty->valuedouble) != ESP_OK) {
            return "PWM update failed";
        }
        remember(p, p->duty);
        return NULL;
    }
    if (p->mode == M_COUNTER && cJSON_IsTrue(cJSON_GetObjectItem(b, "reset"))) {
        pcnt_unit_clear_count(p->unit);
        p->count = p->win_count = 0;
        return NULL;
    }
    if (p->mode == M_WS2812 && (cJSON_GetObjectItem(b, "on") || cJSON_GetObjectItem(b, "state") || cJSON_GetObjectItem(b, "color") ||
                                cJSON_GetObjectItem(b, "brightness") || cJSON_GetObjectItem(b, "effect"))) {
        return set_light(p, b);
    }
    if (p->mode == M_ONEWIRE && cJSON_IsTrue(cJSON_GetObjectItem(b, "rescan"))) {
        gpio_onewire_rescan(p->ow);
        return NULL;
    }
    return "output: level or pulse_ms; pwm: duty; counter: reset; ws2812: on, color, brightness, effect; onewire: rescan";
}

/* {"gpio": 2, "level": 1 | "pulse_ms": 500 | "duty": 50 | "reset": true | "on", "color", ... | "rescan": true} */
static esp_err_t set_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *g = cJSON_GetObjectItem(body, "gpio");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_t *p = cJSON_IsNumber(g) ? find(g->valueint) : NULL;
    const char *err = set_value(p, body);
    cJSON_Delete(body);
    if (err) {
        xSemaphoreGive(s_lock);
        return gw_http_send_error(req, p && p->active ? 400 : 404, err);
    }
    s_changed = true;
    cJSON *j = pin_state_json(p);
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

/* ---------------------------------------------------------------- MQTT */

/* gpio/<n>/set: ON, OFF or PULSE <ms> (output), ON/OFF (PWM: 100 % or 0 %); gpio/<n>/duty/set: 0-100;
 * gpio/<n>/light/set: the JSON of a light (strip) */
static void mqtt_command(const char *suffix, const char *payload)
{
    int gpio = -1;
    char what[8] = "";
    if (sscanf(suffix, "gpio/%d/%7[^/]", &gpio, what) != 2) {
        return;
    }
    cJSON *b = !strcmp(what, "light") ? cJSON_Parse(payload) : cJSON_CreateObject();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_t *p = find(gpio);
    const char *err = "unknown command";
    bool on = !strcasecmp(payload, "ON"), off = !strcasecmp(payload, "OFF");
    if (p && b) {
        if (!strcmp(what, "light")) {
            err = NULL;
        } else if (!strcmp(what, "duty")) {
            cJSON_AddNumberToObject(b, "duty", atof(payload));
            err = NULL;
        } else if (p->mode == M_PWM && (on || off)) {
            cJSON_AddNumberToObject(b, "duty", on ? 100 : 0);
            err = NULL;
        } else if (!strncasecmp(payload, "PULSE", 5)) {
            cJSON_AddNumberToObject(b, "pulse_ms", atoi(payload + 5) > 0 ? atoi(payload + 5) : 1000);
            err = NULL;
        } else if (on || off) {
            cJSON_AddNumberToObject(b, "level", on);
            err = NULL;
        }
        if (!err) {
            err = set_value(p, b);
        }
    }
    if (!err) {
        s_changed = true;
    }
    xSemaphoreGive(s_lock);
    cJSON_Delete(b);
    if (err) {
        ESP_LOGW(TAG, "MQTT %s %s: %s", suffix, payload, err);
    }
}

/* discovery entries sent, to remove them when a pin changes (also after a restart: kept in KEY_PUB) */
#define PUB_MAX (MAX_PINS * 2 + 2 * OW_MAX)
typedef struct {
    char oid[40];
    char component[16];
} pub_t;
static pub_t s_pub[PUB_MAX];
static int s_npub;

static cJSON *entity_cfg(const pin_t *p, const char *suffix, const char *base, const char *host)
{
    char buf[160], name[NAME_CHARS * 3 + 32];
    if (p->name[0]) {
        snprintf(name, sizeof(name), "%s%s", p->name, suffix);
    } else {
        snprintf(name, sizeof(name), "GPIO%d%s", p->gpio, suffix);
    }
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "name", name);
    snprintf(buf, sizeof(buf), "%s/gpio", base);
    cJSON_AddStringToObject(cfg, "state_topic", buf);
    cJSON *dev = cJSON_AddObjectToObject(cfg, "device");
    cJSON *ids = cJSON_AddArrayToObject(dev, "identifiers");
    snprintf(buf, sizeof(buf), "%s_gpio", host);
    cJSON_AddItemToArray(ids, cJSON_CreateString(buf));
    snprintf(buf, sizeof(buf), "%s GPIO", host);
    cJSON_AddStringToObject(dev, "name", buf);
    cJSON_AddStringToObject(dev, "manufacturer", "Guition");
    cJSON_AddStringToObject(dev, "model", "GPIO of header JP1");
    return cfg;
}

/* the entities of a pin into list (component, object id) with their configurations */
static int pin_entities(const pin_t *p, const char *base, const char *host, pub_t *list, cJSON **cfgs, int room)
{
    char buf[160], tpl[160], rom[17], suffix[24];
    int n = 0;
#define ADD(comp, key, cfg)                                                                                            \
    do {                                                                                                               \
        if (n < room) {                                                                                                \
            snprintf(list[n].oid, sizeof(list[n].oid), "gpio_%d_%s_%s", p->gpio, s_modes[p->mode], key);              \
            strlcpy(list[n].component, comp, sizeof(list[n].component));                                               \
            cfgs[n++] = cfg;                                                                                           \
        } else {                                                                                                       \
            cJSON_Delete(cfg);                                                                                         \
        }                                                                                                              \
    } while (0)
    cJSON *cfg;
    switch (p->mode) {
    case M_INPUT:
    case M_OUTPUT:
        cfg = entity_cfg(p, "", base, host);
        snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json['%d'] else 'OFF' }}", p->gpio);
        cJSON_AddStringToObject(cfg, "value_template", tpl);
        if (p->mode == M_OUTPUT) {
            snprintf(buf, sizeof(buf), "%s/gpio/%d/set", base, p->gpio);
            cJSON_AddStringToObject(cfg, "command_topic", buf);
        }
        ADD(p->mode == M_OUTPUT ? "switch" : "binary_sensor", "level", cfg);
        break;
    case M_PWM:
        cfg = entity_cfg(p, "", base, host);
        snprintf(tpl, sizeof(tpl), "{{ 'ON' if value_json['%d'] > 0 else 'OFF' }}", p->gpio);
        cJSON_AddStringToObject(cfg, "state_value_template", tpl);
        snprintf(buf, sizeof(buf), "%s/gpio", base);
        cJSON_AddStringToObject(cfg, "brightness_state_topic", buf);
        snprintf(tpl, sizeof(tpl), "{{ value_json['%d'] }}", p->gpio);
        cJSON_AddStringToObject(cfg, "brightness_value_template", tpl);
        cJSON_AddNumberToObject(cfg, "brightness_scale", 100);
        cJSON_AddStringToObject(cfg, "on_command_type", "brightness");
        snprintf(buf, sizeof(buf), "%s/gpio/%d/duty/set", base, p->gpio);
        cJSON_AddStringToObject(cfg, "brightness_command_topic", buf);
        snprintf(buf, sizeof(buf), "%s/gpio/%d/set", base, p->gpio);
        cJSON_AddStringToObject(cfg, "command_topic", buf);
        ADD("light", "duty", cfg);
        break;
    case M_COUNTER:
        for (int k = 0; k < 2; k++) {
            cfg = entity_cfg(p, k ? " frequency" : "", base, host);
            snprintf(tpl, sizeof(tpl), "{{ value_json['%d'].%s }}", p->gpio, k ? "frequency" : "count");
            cJSON_AddStringToObject(cfg, "value_template", tpl);
            cJSON_AddStringToObject(cfg, "state_class", k ? "measurement" : "total_increasing");
            if (k) {
                cJSON_AddStringToObject(cfg, "device_class", "frequency");
                cJSON_AddStringToObject(cfg, "unit_of_measurement", "Hz");
            }
            ADD("sensor", k ? "frequency" : "count", cfg);
        }
        break;
    case M_WS2812:
        cfg = entity_cfg(p, "", base, host);
        cJSON_AddStringToObject(cfg, "schema", "json");
        snprintf(buf, sizeof(buf), "%s/gpio/%d/light", base, p->gpio);
        cJSON_ReplaceItemInObject(cfg, "state_topic", cJSON_CreateString(buf));
        snprintf(buf, sizeof(buf), "%s/gpio/%d/light/set", base, p->gpio);
        cJSON_AddStringToObject(cfg, "command_topic", buf);
        cJSON_AddBoolToObject(cfg, "brightness", true);
        cJSON *modes = cJSON_AddArrayToObject(cfg, "supported_color_modes");
        cJSON_AddItemToArray(modes, cJSON_CreateString("rgb"));
        cJSON_AddBoolToObject(cfg, "effect", true);
        cJSON *fx = cJSON_AddArrayToObject(cfg, "effect_list");
        for (int k = 0; k < 2; k++) {
            cJSON_AddItemToArray(fx, cJSON_CreateString(s_effects[k]));
        }
        ADD("light", "light", cfg);
        break;
    case M_ONEWIRE:
        for (int i = 0; i < p->ow_n; i++) {
            rom_hex(p->ow_dev[i].rom, rom);
            snprintf(suffix, sizeof(suffix), " %.4s", rom + 12);
            cfg = entity_cfg(p, suffix, base, host);
            snprintf(tpl, sizeof(tpl), "{{ value_json['%d']['%s'] }}", p->gpio, rom);
            cJSON_AddStringToObject(cfg, "value_template", tpl);
            cJSON_AddStringToObject(cfg, "device_class", "temperature");
            cJSON_AddStringToObject(cfg, "unit_of_measurement", "°C");
            cJSON_AddStringToObject(cfg, "state_class", "measurement");
            cJSON_AddNumberToObject(cfg, "suggested_display_precision", 1);
            ADD("sensor", rom, cfg);
        }
        break;
    }
#undef ADD
    return n;
}

/* Runs in the MQTT publish task (connect, gw_mqtt_rediscover). */
static void mqtt_discovery(void)
{
    pin_t *snap = calloc(MAX_PINS, sizeof(pin_t));
    pub_t *now = calloc(PUB_MAX, sizeof(pub_t));
    cJSON **cfgs = calloc(PUB_MAX, sizeof(cJSON *));
    if (!snap || !now || !cfgs) {
        free(snap);
        free(now);
        free(cfgs);
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int npin = s_npin;
    memcpy(snap, s_pin, npin * sizeof(pin_t));
    xSemaphoreGive(s_lock);
    const char *base = gw_mqtt_base_topic(), *host = gw_state()->hostname;
    int n = 0;
    for (int i = 0; i < npin; i++) {
        if (snap[i].active && (snap[i].expose & EXPOSE_MQTT)) {
            n += pin_entities(&snap[i], base, host, now + n, cfgs + n, PUB_MAX - n);
        }
    }
    /* what was published before and is gone now */
    for (int k = 0; k < s_npub; k++) {
        bool keep = false;
        for (int i = 0; i < n && !keep; i++) {
            keep = !strcmp(now[i].oid, s_pub[k].oid) && !strcmp(now[i].component, s_pub[k].component);
        }
        if (!keep) {
            gw_mqtt_discovery(GW_MQTT_GPIO, s_pub[k].component, s_pub[k].oid, NULL);
        }
    }
    for (int i = 0; i < n; i++) {
        gw_mqtt_discovery(GW_MQTT_GPIO, now[i].component, now[i].oid, cfgs[i]);
    }
    /* lost connection: the next connect runs this again, the removals still pending */
    if (gw_mqtt_connected() && (n != s_npub || memcmp(s_pub, now, n * sizeof(pub_t)))) {
        memcpy(s_pub, now, n * sizeof(pub_t));
        s_npub = n;
        if (n) {
            gw_config_set_blob(KEY_PUB, s_pub, n * sizeof(pub_t));
        } else {
            gw_config_erase(KEY_PUB);
        }
    }
    free(snap);
    free(now);
    free(cfgs);
    push(); /* the retained state for the entities */
}

/* ---------------------------------------------------------------- Telegram */

static void bot_gpio(const char *args, bool cz, char *out, size_t len)
{
    size_t used = snprintf(out, len, "<b>GPIO (JP1)</b>");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_npin) {
        used += snprintf(out + used, used < len ? len - used : 0, cz ? "\nŽádný pin není nastavený." : "\nNo pin is configured.");
    }
    for (int i = 0; i < s_npin && used < len; i++) {
        const pin_t *p = &s_pin[i];
        char value[48] = "";
        if (!p->active) {
            snprintf(value, sizeof(value), "%s", cz ? "vypnuto" : "off");
        } else if (p->mode == M_PWM) {
            snprintf(value, sizeof(value), "%.1f %%", p->duty);
        } else if (p->mode == M_COUNTER) {
            snprintf(value, sizeof(value), "%d, %.1f/s", p->count, p->rate);
        } else {
            snprintf(value, sizeof(value), "%d", p->level);
        }
        used += snprintf(out + used, len - used, "\nGPIO%d %s %s: %s", p->gpio, p->name, s_modes[p->mode], value);
    }
    xSemaphoreGive(s_lock);
}

/* ---------------------------------------------------------------- reports */

static bool reported(const pin_t *p)
{
    return p->mode == M_INPUT || p->mode == M_OUTPUT || p->mode == M_PWM || p->mode == M_COUNTER || p->mode == M_ONEWIRE;
}

static int report_devices(gw_rdev_t *out, int max)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    int n = 0;
    for (int i = 0; i < s_npin && n < max; i++) {
        const pin_t *p = &s_pin[i];
        if (reported(p)) {
            snprintf(out[n].id, sizeof(out[n].id), "%d", p->gpio);
            snprintf(out[n].name, sizeof(out[n].name), "GPIO%d%s%s", p->gpio, p->name[0] ? " " : "", p->name);
            n++;
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

static gw_rv_t *rv(gw_rv_t *v, const char *key, const char *name, const char *unit, int prec, uint8_t kind)
{
    memset(v, 0, sizeof(*v));
    strlcpy(v->key, key, sizeof(v->key));
    strlcpy(v->name, name, sizeof(v->name));
    strlcpy(v->unit, unit, sizeof(v->unit));
    v->precision = (int8_t)prec;
    v->kind = kind;
    v->num = kind != GW_RV_STATE;
    return v;
}

static int report_values(const char *dev, bool cz, gw_rv_t *out, int max)
{
    int n = -1;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_npin && n < 0; i++) {
        const pin_t *p = &s_pin[i];
        char id[8];
        snprintf(id, sizeof(id), "%d", p->gpio);
        if (strcmp(id, dev) != 0 || !reported(p) || !p->active || max < OW_MAX) {
            continue;
        }
        n = 0;
        if (p->mode == M_INPUT || p->mode == M_OUTPUT) {
            gw_rv_t *v = rv(&out[n++], "state", cz ? "Stav" : "State", "", 0, GW_RV_STATE);
            strlcpy(v->text, p->level ? (cz ? "zap" : "on") : (cz ? "vyp" : "off"), sizeof(v->text));
        } else if (p->mode == M_PWM) {
            rv(&out[n++], "duty", cz ? "Střída" : "Duty", "%", 1, GW_RV_MEASURE)->value = p->duty;
        } else if (p->mode == M_COUNTER) {
            rv(&out[n++], "count", cz ? "Počet pulzů" : "Pulses", "", 0, GW_RV_TOTAL)->value = p->count;
            rv(&out[n++], "rate", cz ? "Frekvence" : "Frequency", "Hz", 1, GW_RV_MEASURE)->value = p->rate;
        } else {
            for (int k = 0; k < p->ow_n && k < OW_MAX; k++) {
                char rom[17], name[48];
                rom_hex(p->ow_dev[k].rom, rom);
                if (p->ow_dev[k].ok) {
                    snprintf(name, sizeof(name), "%s %.6s", cz ? "Teplota" : "Temperature", rom + 10);
                    rv(&out[n++], rom, name, "°C", 1, GW_RV_MEASURE)->value = roundf(p->ow_dev[k].temp * 10) / 10;
                }
            }
        }
    }
    xSemaphoreGive(s_lock);
    return n;
}

esp_err_t gpio_ctl_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    char *text = malloc(1024);
    s_last = text && gw_config_get_str(KEY_LAST, text, 1024) ? cJSON_Parse(text) : NULL;
    free(text);
    if (!cJSON_IsObject(s_last)) {
        cJSON_Delete(s_last);
        s_last = cJSON_CreateObject();
    }
    size_t len = 0;
    if (gw_config_blob_size(KEY_PUB, &len) == ESP_OK && len % sizeof(pub_t) == 0 && len <= sizeof(s_pub)
        && gw_config_get_blob(KEY_PUB, s_pub, len)) {
        s_npub = len / sizeof(pub_t);
    }
    pin_t *list = calloc(MAX_PINS, sizeof(pin_t));
    int n = 0;
    if (list) {
        load(list, &n);
        if (uses_radio_b(list, n) && gw_state()->desired_b != GW_ROLE_NONE) {
            gw_notify("system", GW_NOTIFY_WARNING, "GPIO on radio B's pins is off: radio B has the role %s",
                      gw_role_name(gw_state()->desired_b));
        }
        xSemaphoreTake(s_lock, portMAX_DELAY);
        apply(list, n);
        xSemaphoreGive(s_lock);
        free(list);
    }
    gw_http_register("/api/gpio", HTTP_GET, gpio_get);
    gw_http_register("/api/gpio", HTTP_PUT, gpio_put);
    gw_http_register("/api/gpio/set", HTTP_POST, set_post);
    gw_bot_command("gpio", "GPIO of JP1", "GPIO na JP1", "system/hw", 1, false, bot_gpio);
    gw_report_source("gpio", "GPIO (JP1)", "GPIO (JP1)", "system/hw", report_devices, report_values);
    gw_mqtt_set_extension(mqtt_discovery, NULL);
    gw_mqtt_add_command("gpio/+/set", mqtt_command);
    gw_mqtt_add_command("gpio/+/duty/set", mqtt_command);
    gw_mqtt_add_command("gpio/+/light/set", mqtt_command);
    ESP_LOGI(TAG, "%d pins configured", n);
    return xTaskCreate(task, "gpio_ctl", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
