#pragma once

/*
 * YAMNet log-mel frontend (tools/yamnet/yamnet_torch.py: log_mel): 25 ms periodic Hann window,
 * 512-point FFT magnitude, 64 HTK mel bands 125-7500 Hz, log(mel + 0.001). Plain C, also built
 * on the host by tools/yamnet/test_mel.py to compare with the Python reference.
 */
#define MEL_SAMPLE_RATE 16000
#define MEL_WINDOW 400
#define MEL_HOP 160
#define MEL_BANDS 64
#define MEL_PATCH_FRAMES 96
#define MEL_PATCH_HOP 48

#ifdef __cplusplus
extern "C" {
#endif

/* Builds the window, twiddles and mel matrix. Returns 0 on success. */
int mel_init(void);
/* One frame of MEL_WINDOW samples in [-1, 1] -> MEL_BANDS log-mel values. */
void mel_frame(const float *samples, float *out);

#ifdef __cplusplus
}
#endif
