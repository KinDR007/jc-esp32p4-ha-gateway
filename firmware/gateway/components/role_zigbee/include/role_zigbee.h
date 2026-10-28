#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Coordinator mode bridges the ZBOSS NCP on the C6 UART to one TCP client
 * (Zigbee2MQTT adapter "zboss", or ZHA with a ZBOSS radio library).
 * Router mode talks to the standalone router firmware on the C6 over a line protocol
 * ("@ZR {json}" status lines, plain text commands).
 */
esp_err_t role_zigbee_start(bool coordinator);

#ifdef __cplusplus
}
#endif
