/* Wiring test of a radio on its UART, reset and boot lines (docs/dual-radio.md, F2).
 *
 * 1. Reset into the ROM bootloader and sync: TX, RX, RESET and BOOT work; read chip, MAC, flash.
 * 2. Otherwise reset with BOOT released and listen for the ROM banner at 115200 Bd: power, RESET
 *    and the line radio -> P4 work.
 * 3. Reset with BOOT held low: a banner in download mode means BOOT works too, so the line
 *    P4 -> radio is the broken one; a normal boot means BOOT does not reach the chip.
 */
#include "c6_flasher.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp32_port.h"
#include "esp_loader.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "radio_probe";

#define BANNER_MS 800

static const char *chip_name(target_chip_t chip)
{
    switch (chip) {
    case ESP32C6_CHIP:
        return "ESP32-C6";
    case ESP32C5_CHIP:
        return "ESP32-C5";
    case ESP32H2_CHIP:
        return "ESP32-H2";
    case ESP32_CHIP:
        return "ESP32";
    case ESP32S3_CHIP:
        return "ESP32-S3";
    case ESP32C3_CHIP:
        return "ESP32-C3";
    default:
        return "other";
    }
}

void c6_radio_init_pins(const c6_radio_pins_t *p)
{
    gpio_config_t reset = {.pin_bit_mask = 1ULL << p->reset,
                           .mode = p->reset_open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_OUTPUT};
    gpio_set_level(p->reset, 1);
    gpio_config(&reset);
    gpio_set_level(p->reset, 1);
    gpio_reset_pin(p->boot); /* the radio side pulls BOOT up; leave it released */
    gpio_set_direction(p->boot, GPIO_MODE_INPUT);
}

static bool rom_sync(const c6_radio_pins_t *p, c6_probe_t *r)
{
    esp32_port_t port = {
        .port.ops = &esp32_uart_ops,
        .baud_rate = 115200,
        .uart_port = p->uart_port,
        .uart_rx_pin = p->rx,
        .uart_tx_pin = p->tx,
        .reset_pin = p->reset,
        .boot_pin = p->boot,
        .rx_buffer_size = 2048,
        .tx_buffer_size = 2048,
    };
    esp_loader_t loader;
    if (esp_loader_init_serial(&loader, &port.port) != ESP_LOADER_SUCCESS) {
        return false;
    }
    esp_loader_connect_args_t args = ESP_LOADER_CONNECT_DEFAULT();
    args.trials = 3;
    bool ok = esp_loader_connect(&loader, &args) == ESP_LOADER_SUCCESS;
    if (ok) {
        target_chip_t target = esp_loader_get_target(&loader);
        strlcpy(r->chip, chip_name(target), sizeof(r->chip));
        r->chip_id = target == ESP32C6_CHIP ? C6FW_CHIP_C6 : target == ESP32C5_CHIP ? C6FW_CHIP_C5 : 0;
        esp_loader_read_mac(&loader, r->mac);
        esp_loader_get_chip_revision(&loader, &r->revision);
        esp_loader_flash_detect_size(&loader, &r->flash_size);
        esp_loader_reset_target(&loader);
    }
    esp_loader_deinit(&loader);
    return ok;
}

/* Reset with BOOT released or held low and collect what the ROM prints. */
static int banner(const c6_radio_pins_t *p, bool boot_low, char *buf, size_t len)
{
    uart_config_t cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(p->uart_port, 2048, 0, 0, NULL, 0) != ESP_OK) {
        return -1;
    }
    uart_param_config(p->uart_port, &cfg);
    uart_set_pin(p->uart_port, p->tx, p->rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (boot_low) {
        gpio_set_direction(p->boot, GPIO_MODE_OUTPUT);
        gpio_set_level(p->boot, 0);
    }
    gpio_set_level(p->reset, 0);
    vTaskDelay(pdMS_TO_TICKS(50));
    uart_flush_input(p->uart_port);
    gpio_set_level(p->reset, 1);
    int n = uart_read_bytes(p->uart_port, buf, len - 1, pdMS_TO_TICKS(BANNER_MS));
    buf[n > 0 ? n : 0] = '\0';
    uart_driver_delete(p->uart_port);
    c6_radio_init_pins(p);
    if (boot_low) {
        /* leave download mode */
        gpio_set_level(p->reset, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(p->reset, 1);
    }
    return n;
}

esp_err_t c6_radio_probe(c6_radio_t radio, c6_probe_t *r)
{
    const c6_radio_pins_t *p = c6_radio_pins(radio);
    memset(r, 0, sizeof(*r));
    c6_radio_init_pins(p);
    if (rom_sync(p, r)) {
        r->wired = true;
        if (r->chip_id) {
            c6_radio_set_chip(radio, r->chip_id);
        }
        snprintf(r->verdict, sizeof(r->verdict), "%s found, all four lines work", r->chip);
        c6_radio_init_pins(p);
        ESP_LOGI(TAG, "%s", r->verdict);
        return ESP_OK;
    }
    c6_radio_init_pins(p);
    char *buf = calloc(1, 1024);
    if (!buf) {
        return ESP_ERR_NO_MEM;
    }
    banner(p, false, buf, 1024);
    r->banner = strstr(buf, "ESP-ROM") || strstr(buf, "rst:0x");
    if (!r->banner) {
        snprintf(r->verdict, sizeof(r->verdict),
                 "no answer: check power (5V, GND), RESET (P4 GPIO%d to RST) and radio TX to P4 GPIO%d; TX/RX not crossed",
                 p->reset, p->rx);
    } else {
        banner(p, true, buf, 1024);
        r->download_mode = strstr(buf, "DOWNLOAD") || strstr(buf, "waiting for download");
        if (r->download_mode) {
            snprintf(r->verdict, sizeof(r->verdict),
                     "the radio boots and answers, but does not hear the P4: check P4 GPIO%d (TX) to the radio RX", p->tx);
        } else {
            snprintf(r->verdict, sizeof(r->verdict),
                     "the radio boots, but BOOT does not reach it: check P4 GPIO%d to BOOT (and P4 GPIO%d TX to the radio RX)",
                     p->boot, p->tx);
        }
    }
    free(buf);
    ESP_LOGW(TAG, "%s", r->verdict);
    return ESP_FAIL;
}
