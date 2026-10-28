#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#define SOUND_CLASSES 521
#define SOUND_EMBEDDING 1024

#ifdef __cplusplus
extern "C" {
#endif

/* Checks the model partition header (ESP-DL "EDL2"). Returns the model size or 0. */
uint32_t sound_model_stored_size(void);
/* Loads the model from the "model" partition into PSRAM. */
esp_err_t sound_model_load(void);
bool sound_model_ready(void);
/* patch: MEL_PATCH_FRAMES x MEL_BANDS log-mel values. scores: sigmoid per class,
 * embedding may be NULL. */
esp_err_t sound_model_run(const float *patch, float *scores, float *embedding);

#ifdef __cplusplus
}
#endif
