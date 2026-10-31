#pragma once

#include "c6_flasher.h"
#include "esp_err.h"

/*
 * ESPHome Bluetooth proxy as the role of a radio (docs/dual-radio.md, F6). The radio runs the
 * ESPHome image of the release (firmware/esphome/radio_proxy) on its own, over Wi-Fi. The gateway
 * sets its Wi-Fi network over Improv serial on the radio's UART, then releases the UART; Home
 * Assistant adopts the device and sets its API encryption key.
 */
/* Wi-Fi settings API (/api/esphome/wifi), registered whatever the roles. */
void role_esphome_init(void);
/* The radio already runs the ESPHome bundle: provisions its Wi-Fi when needed. */
esp_err_t role_esphome_start(c6_radio_t radio);
