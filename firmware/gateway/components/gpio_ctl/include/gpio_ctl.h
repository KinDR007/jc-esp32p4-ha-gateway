#pragma once
#include <stdbool.h>
#include "esp_err.h"

/* Free GPIO of header JP1 (docs/gpio.md): input, output, PWM, pulse counter; API /api/gpio. */

/* Before the radios start: the stored configuration uses radio B's pins and radio B has no role, so
 * the gateway leaves radio B alone (no pin setup, no wiring test). */
bool gpio_ctl_claims_radio_b(void);
esp_err_t gpio_ctl_init(void);
