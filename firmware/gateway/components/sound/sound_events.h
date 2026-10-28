#pragma once

#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Sound events: user-defined groups of YAMNet classes with their own threshold and timing,
 * published to Home Assistant over MQTT (event entity, binary_sensor, device trigger).
 * Configuration in NVS, default preset "household" (sound_presets.json).
 */
esp_err_t sound_events_init(const char *(*class_name)(int index));
/* Called once per 0.48 s window; scores NULL when the window was below the level gate. */
void sound_events_update(const float *scores, float level_db, int64_t now_us);
/* MQTT discovery for all enabled events (on connect). */
void sound_events_mqtt_discovery(void);

#ifdef __cplusplus
}
#endif
