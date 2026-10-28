#include "sound_mel.h"

#include <math.h>
#include <stdlib.h>

#define FFT_N 512
#define FFT_BINS (FFT_N / 2 + 1)
#define MEL_MIN_HZ 125.0
#define MEL_MAX_HZ 7500.0
#define LOG_OFFSET 0.001f

static float s_window[MEL_WINDOW];
static float s_cos[FFT_N / 2], s_sin[FFT_N / 2];
static unsigned short s_bitrev[FFT_N];
/* Sparse mel matrix: each band covers a contiguous bin range. */
static struct {
    int first, count;
    float weights[FFT_BINS];
} s_bands[MEL_BANDS];

static double hz_to_mel(double hz)
{
    return 1127.0 * log1p(hz / 700.0);
}

int mel_init(void)
{
    for (int i = 0; i < MEL_WINDOW; i++) {
        s_window[i] = (float)(0.5 - 0.5 * cos(2.0 * M_PI * i / MEL_WINDOW));
    }
    for (int i = 0; i < FFT_N / 2; i++) {
        s_cos[i] = (float)cos(2.0 * M_PI * i / FFT_N);
        s_sin[i] = (float)-sin(2.0 * M_PI * i / FFT_N);
    }
    for (int i = 0, bits = 9; i < FFT_N; i++) {
        unsigned r = 0;
        for (int b = 0; b < bits; b++) {
            r |= ((i >> b) & 1) << (bits - 1 - b);
        }
        s_bitrev[i] = r;
    }
    /* Same construction as tf.signal.linear_to_mel_weight_matrix: bin 0 (DC) gets no weight. */
    double lo = hz_to_mel(MEL_MIN_HZ), hi = hz_to_mel(MEL_MAX_HZ);
    for (int m = 0; m < MEL_BANDS; m++) {
        double left = lo + (hi - lo) * m / (MEL_BANDS + 1);
        double center = lo + (hi - lo) * (m + 1) / (MEL_BANDS + 1);
        double right = lo + (hi - lo) * (m + 2) / (MEL_BANDS + 1);
        s_bands[m].first = -1;
        s_bands[m].count = 0;
        for (int k = 1; k < FFT_BINS; k++) {
            double mel = hz_to_mel((double)k * (MEL_SAMPLE_RATE / 2.0) / (FFT_BINS - 1));
            double w = fmin((mel - left) / (center - left), (right - mel) / (right - center));
            if (w <= 0) {
                continue;
            }
            if (s_bands[m].first < 0) {
                s_bands[m].first = k;
            }
            s_bands[m].weights[k - s_bands[m].first] = (float)w;
            s_bands[m].count = k - s_bands[m].first + 1;
        }
    }
    return 0;
}

void mel_frame(const float *samples, float *out)
{
    float re[FFT_N], im[FFT_N];
    for (int i = 0; i < FFT_N; i++) {
        int j = s_bitrev[i];
        re[j] = i < MEL_WINDOW ? samples[i] * s_window[i] : 0.0f;
        im[j] = 0.0f;
    }
    /* Iterative radix-2 FFT. */
    for (int len = 2; len <= FFT_N; len <<= 1) {
        int half = len >> 1, step = FFT_N / len;
        for (int start = 0; start < FFT_N; start += len) {
            for (int k = 0; k < half; k++) {
                float wr = s_cos[k * step], wi = s_sin[k * step];
                int a = start + k, b = a + half;
                float tr = re[b] * wr - im[b] * wi;
                float ti = re[b] * wi + im[b] * wr;
                re[b] = re[a] - tr;
                im[b] = im[a] - ti;
                re[a] += tr;
                im[a] += ti;
            }
        }
    }
    float mag[FFT_BINS];
    for (int k = 0; k < FFT_BINS; k++) {
        mag[k] = sqrtf(re[k] * re[k] + im[k] * im[k]);
    }
    for (int m = 0; m < MEL_BANDS; m++) {
        float acc = 0.0f;
        for (int i = 0; i < s_bands[m].count; i++) {
            acc += mag[s_bands[m].first + i] * s_bands[m].weights[i];
        }
        out[m] = logf(acc + LOG_OFFSET);
    }
}
