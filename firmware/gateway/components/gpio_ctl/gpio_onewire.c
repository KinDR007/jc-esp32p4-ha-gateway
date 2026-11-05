#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gpio_ctl_priv.h"

static const char *TAG = "gpio_onewire";

/*
 * 1-Wire on one pin, bit-banged with the standard timings (Maxim AN126) as open drain; the bus needs
 * an external pull-up (4.7 kOhm to 3.3 V), the internal one only helps short wires. All DS18B20
 * (also DS1822, DS18S20) found by a ROM search convert at once (skip ROM), then each is read with its
 * ROM and a CRC check. A search runs at start, every 10 cycles and on request.
 */

#define CONVERT_MS 750
#define SEARCH_EVERY 10

struct gpio_ow {
    int gpio;
    int interval_s;
    TaskHandle_t task;
    SemaphoreHandle_t lock, done;
    volatile bool stop, rescan, changed;
    int n;
    gpio_ow_sensor_t dev[OW_MAX];
    bool present;
    uint32_t crc_errors;
};

static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

static inline void low(const gpio_ow_t *o)
{
    gpio_set_level(o->gpio, 0);
}

static inline void release(const gpio_ow_t *o)
{
    gpio_set_level(o->gpio, 1);
}

static bool reset(const gpio_ow_t *o)
{
    low(o);
    esp_rom_delay_us(480);
    portENTER_CRITICAL(&s_mux);
    release(o);
    esp_rom_delay_us(70);
    bool present = gpio_get_level(o->gpio) == 0;
    portEXIT_CRITICAL(&s_mux);
    esp_rom_delay_us(410);
    return present;
}

static void write_bit(const gpio_ow_t *o, int bit)
{
    portENTER_CRITICAL(&s_mux);
    low(o);
    esp_rom_delay_us(bit ? 6 : 60);
    release(o);
    esp_rom_delay_us(bit ? 64 : 10);
    portEXIT_CRITICAL(&s_mux);
}

static int read_bit(const gpio_ow_t *o)
{
    portENTER_CRITICAL(&s_mux);
    low(o);
    esp_rom_delay_us(6);
    release(o);
    esp_rom_delay_us(9);
    int bit = gpio_get_level(o->gpio);
    portEXIT_CRITICAL(&s_mux);
    esp_rom_delay_us(55);
    return bit;
}

static void write_byte(const gpio_ow_t *o, uint8_t b)
{
    for (int i = 0; i < 8; i++) {
        write_bit(o, (b >> i) & 1);
    }
}

static uint8_t read_byte(const gpio_ow_t *o)
{
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) {
        b |= read_bit(o) << i;
    }
    return b;
}

static uint8_t crc8(const uint8_t *d, int n)
{
    uint8_t crc = 0;
    while (n--) {
        uint8_t b = *d++;
        for (int i = 0; i < 8; i++) {
            uint8_t mix = (crc ^ b) & 1;
            crc >>= 1;
            if (mix) {
                crc ^= 0x8c;
            }
            b >>= 1;
        }
    }
    return crc;
}

/* ROM search (Maxim AN187): every device on the bus, up to OW_MAX */
static int search(gpio_ow_t *o, uint8_t roms[][8])
{
    uint8_t rom[8] = {0};
    int last_discrepancy = 0, n = 0;
    bool last_device = false;
    while (!last_device && n < OW_MAX) {
        if (!reset(o)) {
            break;
        }
        write_byte(o, 0xf0);
        int last_zero = 0;
        for (int bit = 1; bit <= 64; bit++) {
            int a = read_bit(o), b = read_bit(o);
            if (a && b) {
                return n; /* nobody answered */
            }
            int dir;
            if (a != b) {
                dir = a;
            } else {
                dir = bit < last_discrepancy ? (rom[(bit - 1) / 8] >> ((bit - 1) % 8)) & 1 : bit == last_discrepancy;
                if (!dir) {
                    last_zero = bit;
                }
            }
            if (dir) {
                rom[(bit - 1) / 8] |= 1 << ((bit - 1) % 8);
            } else {
                rom[(bit - 1) / 8] &= ~(1 << ((bit - 1) % 8));
            }
            write_bit(o, dir);
        }
        last_discrepancy = last_zero;
        last_device = last_discrepancy == 0;
        if (crc8(rom, 7) == rom[7] && (rom[0] == 0x28 || rom[0] == 0x22 || rom[0] == 0x10)) {
            memcpy(roms[n++], rom, 8);
        } else if (crc8(rom, 7) != rom[7]) {
            o->crc_errors++;
        }
    }
    return n;
}

static bool read_temp(gpio_ow_t *o, const uint8_t rom[8], float *t)
{
    uint8_t sp[9];
    if (!reset(o)) {
        return false;
    }
    write_byte(o, 0x55);
    for (int i = 0; i < 8; i++) {
        write_byte(o, rom[i]);
    }
    write_byte(o, 0xbe);
    for (int i = 0; i < 9; i++) {
        sp[i] = read_byte(o);
    }
    if (crc8(sp, 8) != sp[8]) {
        o->crc_errors++;
        return false;
    }
    int16_t raw = (int16_t)(sp[1] << 8 | sp[0]);
    *t = rom[0] == 0x10 ? raw / 2.0f : raw / 16.0f;
    return !(rom[0] != 0x10 && raw == 0x0550); /* 85 °C: the power-on value, no conversion happened */
}

static void task(void *arg)
{
    gpio_ow_t *o = arg;
    uint8_t (*roms)[8] = calloc(OW_MAX, 8);
    int cycle = 0;
    while (roms && !o->stop) {
        if (o->rescan || cycle % SEARCH_EVERY == 0) {
            o->rescan = false;
            int n = search(o, roms);
            xSemaphoreTake(o->lock, portMAX_DELAY);
            /* keep the values of the devices still there */
            gpio_ow_sensor_t old[OW_MAX];
            int nold = o->n;
            memcpy(old, o->dev, sizeof(old));
            for (int i = 0; i < n; i++) {
                memcpy(o->dev[i].rom, roms[i], 8);
                o->dev[i].ok = false;
                for (int k = 0; k < nold; k++) {
                    if (!memcmp(old[k].rom, roms[i], 8)) {
                        o->dev[i] = old[k];
                    }
                }
            }
            o->changed |= n != o->n;
            o->n = n;
            xSemaphoreGive(o->lock);
            if (n != nold) {
                ESP_LOGI(TAG, "GPIO%d: %d sensors", o->gpio, n);
            }
        }
        cycle++;
        o->present = reset(o);
        if (o->present && o->n) {
            write_byte(o, 0xcc); /* all at once */
            write_byte(o, 0x44);
            vTaskDelay(pdMS_TO_TICKS(CONVERT_MS));
            for (int i = 0; i < o->n && !o->stop; i++) {
                float t;
                bool ok = read_temp(o, o->dev[i].rom, &t);
                xSemaphoreTake(o->lock, portMAX_DELAY);
                o->changed |= ok != o->dev[i].ok || (ok && t != o->dev[i].temp);
                o->dev[i].ok = ok;
                if (ok) {
                    o->dev[i].temp = t;
                }
                xSemaphoreGive(o->lock);
            }
        } else if (o->n) {
            xSemaphoreTake(o->lock, portMAX_DELAY);
            for (int i = 0; i < o->n; i++) {
                o->changed |= o->dev[i].ok;
                o->dev[i].ok = false;
            }
            xSemaphoreGive(o->lock);
        }
        for (int s = 0; s < o->interval_s * 10 && !o->stop && !o->rescan; s++) {
            vTaskDelay(pdMS_TO_TICKS(100));
        }
    }
    free(roms);
    xSemaphoreGive(o->done);
    vTaskDelete(NULL);
}

gpio_ow_t *gpio_onewire_start(int gpio, int interval_s, char *why, size_t len)
{
    gpio_ow_t *o = calloc(1, sizeof(*o));
    if (!o) {
        snprintf(why, len, "no memory");
        return NULL;
    }
    o->gpio = gpio;
    o->interval_s = interval_s;
    o->lock = xSemaphoreCreateMutex();
    o->done = xSemaphoreCreateBinary();
    gpio_set_level(gpio, 1);
    gpio_set_direction(gpio, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(gpio, GPIO_PULLUP_ONLY);
    if (xTaskCreate(task, "gpio_1wire", 3072, o, 4, &o->task) != pdPASS) {
        snprintf(why, len, "no memory for the 1-Wire task");
        vSemaphoreDelete(o->lock);
        vSemaphoreDelete(o->done);
        free(o);
        return NULL;
    }
    return o;
}

void gpio_onewire_stop(gpio_ow_t *o)
{
    if (!o) {
        return;
    }
    o->stop = true;
    xSemaphoreTake(o->done, pdMS_TO_TICKS(3000));
    vSemaphoreDelete(o->lock);
    vSemaphoreDelete(o->done);
    free(o);
}

void gpio_onewire_rescan(gpio_ow_t *o)
{
    o->rescan = true;
}

int gpio_onewire_read(gpio_ow_t *o, gpio_ow_sensor_t *out, bool *present, bool *changed)
{
    xSemaphoreTake(o->lock, portMAX_DELAY);
    int n = o->n;
    memcpy(out, o->dev, n * sizeof(*out));
    *present = o->present;
    if (changed) {
        *changed = o->changed;
        o->changed = false;
    }
    xSemaphoreGive(o->lock);
    return n;
}
