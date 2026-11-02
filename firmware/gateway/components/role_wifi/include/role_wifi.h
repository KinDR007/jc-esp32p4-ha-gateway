#pragma once

#include "c6_flasher.h"
#include "esp_err.h"

/*
 * Wi-Fi uplink as the role of a radio (docs/wifi-uplink.md). The radio runs the ESP-Hosted
 * co-processor (bundle wifi_uplink), the gateway the host over the radio's UART: an ordinary Wi-Fi
 * station interface next to Ethernet, with a lower route priority, so the LAN carries the traffic
 * while its link is up.
 */
/* Wi-Fi settings API (/api/net/wifi), registered whatever the roles. */
void role_wifi_init(void);
/* The radio already runs the wifi_uplink bundle: brings up the host and joins the network. */
esp_err_t role_wifi_start(c6_radio_t radio);
