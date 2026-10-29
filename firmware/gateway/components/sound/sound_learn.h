#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "sound_mel.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Learned sounds: samples of a sound recorded on the board (0.96 s audio and its YAMNet embedding)
 * kept in the "sounds" partition, grouped into profiles. A profile scores every model window:
 * "embedding" profiles by cosine similarity to the mean of their samples, "tone" profiles by the
 * share of energy at the dominant frequency of their samples (appliance beeps, piezo alarms).
 * Background recordings of the home calibrate the threshold. Sound events use a profile like a
 * group of classes ("profile" instead of "classes").
 */

/* Audio of one model window: 96 log-mel frames of MEL_WINDOW samples, MEL_HOP apart. */
#define LEARN_AUDIO ((MEL_PATCH_FRAMES - 1) * MEL_HOP + MEL_WINDOW)

typedef struct {
    bool (*ready)(void);    /* the model runs: recording needs embeddings */
    float (*gate_db)(void); /* a sample quieter than this is silence */
    /* The model on one window of audio (LEARN_AUDIO samples). Inference task only. */
    esp_err_t (*embed)(const int16_t *audio, float *embedding);
} sound_learn_hooks_t;

esp_err_t sound_learn_init(const sound_learn_hooks_t *hooks);
/* On the inference task between windows: processes an uploaded recording (WAV). */
void sound_learn_poll(void);
/* A recording waits for windows: the model has to run on quiet ones too. */
bool sound_learn_wants_all(void);
/* After every model run, on the inference task: records when asked and scores the profiles. */
void sound_learn_window(const float *embedding, const int16_t *audio, float level_db, int64_t now_us);
/* Score of a profile in the last model window (0..1), negative when the profile does not exist. */
float sound_learn_score(const char *profile);

#ifdef __cplusplus
}
#endif
