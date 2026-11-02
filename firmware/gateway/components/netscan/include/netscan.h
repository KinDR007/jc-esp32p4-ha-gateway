#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "c6_flasher.h"
#include "cJSON.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Radio surroundings (System > Network surroundings).
 *   quick: IEEE 802.15.4 only, done by the running Thread role through OpenThread (no outage)
 *   full:  Wi-Fi and 802.15.4 by the scanner image of radio A or B, run at boot before the roles start
 *          (radio B, an ESP32-C5, also sees 5 GHz Wi-Fi)
 * The last result is kept in NVS.
 */

/* Quick scan of the active role: fills result with "channels", "beacons" and "thread" arrays. */
typedef esp_err_t (*netscan_quick_fn)(cJSON *result, char *err, size_t err_len);

/* Loads the last result, registers /api/net/scan, starts SNTP (scan time). */
void netscan_init(void);
/* The Thread roles provide the quick scan. */
void netscan_set_quick(netscan_quick_fn fn);
/* Radio of a full scan requested before this restart, -1 for none (the request is consumed). */
int netscan_full_pending(void);
/* Flashes the scanner image to the radio, collects its results and stores them. The caller then
 * starts the role, which flashes the role image back. */
void netscan_run_full(c6_radio_t radio);

#ifdef __cplusplus
}
#endif
