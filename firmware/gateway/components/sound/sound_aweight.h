#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A-weighting (IEC 61672) at 16 kHz: the poles below Nyquist by the bilinear transform, the
 * 12.2 kHz double pole (above Nyquist) replaced by a fitted 2-tap FIR. Within 0.2 dB of the
 * standard curve from 31.5 Hz to 7 kHz, 0 dB at 1 kHz.
 */
typedef struct {
    float z[3][2]; /* transposed direct form II state per section */
} aweight_t;

void aweight_reset(aweight_t *f);
float aweight_step(aweight_t *f, float x);

#ifdef __cplusplus
}
#endif
