#pragma once

#include <stdbool.h>

#include "c6_flasher.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Starts OpenThread with the C6 as RCP. Border router mode creates or reuses a network,
 * advertises it on the Ethernet backbone and serves the OTBR REST API. Router mode joins
 * the network from the dataset stored in NVS and stays idle until one is set.
 */
esp_err_t role_thread_start(bool border_router, c6_radio_t radio);

#ifdef __cplusplus
}
#endif
