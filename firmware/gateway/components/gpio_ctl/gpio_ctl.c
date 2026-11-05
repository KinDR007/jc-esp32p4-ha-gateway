#include "gpio_ctl.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/pulse_cnt.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "sdkconfig.h"
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
#define EXPOSE_INTEGRATION 1
#define EXPOSE_MQTT 2
static const char *const s_exposes[] = {"none", "integration", "mqtt", "both"}; /* by the bits */

typedef enum { M_OFF, M_INPUT, M_OUTPUT, M_PWM, M_COUNTER, M_COUNT } pin_mode_t;
static const char *const s_modes[M_COUNT] = {"off", "input", "output", "pwm", "counter"};
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
    if (p->active) {
        gpio_reset_pin(p->gpio);
        gpio_set_direction(p->gpio, GPIO_MODE_DISABLE);
        gpio_set_pull_mode(p->gpio, GPIO_FLOATING);
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
    p->channel = p->timer = -1;
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
        return "mode: off, input, output, pwm or counter";
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
    if (p->mode == M_OUTPUT || p->mode == M_PWM) {
        cJSON_AddStringToObject(j, "restore", s_restores[p->restore]);
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
        if (list[i].mode != M_OFF && radio_b_use(list[i].gpio)) {
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
    int pwm = 0, cnt = 0;
    *n = 0;
    const cJSON *j;
    cJSON_ArrayForEach(j, pins)
    {
        const char *e = parse_pin(j, &out[*n]);
        if (e) {
            snprintf(buf, len, "pin %d: %s", *n + 1, e);
            return buf;
        }
        for (int i = 0; i < *n; i++) {
            if (out[i].gpio == out[*n].gpio) {
                snprintf(buf, len, "GPIO%d twice", out[i].gpio);
                return buf;
            }
        }
        char why[96];
        const char *r = out[*n].mode != M_OFF ? reserved(out[*n].gpio, why, sizeof(why)) : NULL;
        if (r) {
            snprintf(buf, len, "GPIO%d: %s", out[*n].gpio, r);
            return buf;
        }
        pwm += out[*n].mode == M_PWM;
        cnt += out[*n].mode == M_COUNTER;
        (*n)++;
    }
    if (pwm > 4 || cnt > 4) {
        return "4 PWM pins and 4 counters at most";
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
        } else {
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
    char *text = cJSON_PrintUnformatted(v);
    cJSON_Delete(v);
    if (text) {
        gw_mqtt_publish("gpio", text, false); /* also the push WebSocket of the integration */
        cJSON_free(text);
    }
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

/* level (0/1) of an output, its pulse or the duty of a PWM pin; NULL or the error */
static const char *set_value(pin_t *p, const cJSON *level, const cJSON *duty, const cJSON *pulse_ms, bool reset)
{
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
    if (p->mode == M_COUNTER && reset) {
        pcnt_unit_clear_count(p->unit);
        p->count = p->win_count = 0;
        return NULL;
    }
    return "output: level or pulse_ms; pwm: duty; counter: reset";
}

/* {"gpio": 2, "level": 1 | "pulse_ms": 500 | "duty": 50 | "reset": true} */
static esp_err_t set_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *g = cJSON_GetObjectItem(body, "gpio");
    xSemaphoreTake(s_lock, portMAX_DELAY);
    pin_t *p = cJSON_IsNumber(g) ? find(g->valueint) : NULL;
    const char *err = set_value(p, cJSON_GetObjectItem(body, "level"), cJSON_GetObjectItem(body, "duty"),
                                cJSON_GetObjectItem(body, "pulse_ms"), cJSON_IsTrue(cJSON_GetObjectItem(body, "reset")));
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
    ESP_LOGI(TAG, "%d pins configured", n);
    return xTaskCreate(task, "gpio_ctl", 4096, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
