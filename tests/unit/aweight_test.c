/* A-weighting filter of the sound level meter against IEC 61672 values (dB, class 1 nominal).
 *   cc -std=c11 -Wall -Wextra -Werror -I firmware/gateway/components/sound -o /tmp/aweight_test \
 *      tests/unit/aweight_test.c firmware/gateway/components/sound/sound_aweight.c -lm && /tmp/aweight_test
 * Coefficients: bilinear transform of the A poles below Nyquist, 2-tap FIR fitted for the 12.2 kHz pair. */
#include <math.h>
#include <stdio.h>

#include "sound_aweight.h"

#define RATE 16000

static double gain_db(double hz)
{
    aweight_t f;
    aweight_reset(&f);
    double in = 0, out = 0;
    /* 20 s: the 20.6 Hz poles settle (float32 state, as on the gateway) */
    for (long n = 0; n < 20L * RATE; n++) {
        float x = (float)sin(2 * M_PI * hz * n / RATE);
        float y = aweight_step(&f, x);
        if (n >= 10L * RATE) {
            in += (double)x * x;
            out += (double)y * y;
        }
    }
    return 10 * log10(out / in);
}

int main(void)
{
    static const struct {
        double hz, db;
    } ref[] = {{31.5, -39.4}, {63, -26.2}, {125, -16.1}, {250, -8.6}, {500, -3.2}, {1000, 0.0},
               {2000, 1.2},   {4000, 1.0}, {6300, -0.1}, {7000, -0.5}};
    int fails = 0;
    for (size_t i = 0; i < sizeof(ref) / sizeof(ref[0]); i++) {
        double g = gain_db(ref[i].hz), err = g - ref[i].db;
        int ok = fabs(err) <= 0.5;
        fails += !ok;
        printf("%7.1f Hz  %+6.2f dB  (IEC %+5.1f, error %+5.2f) %s\n", ref[i].hz, g, ref[i].db, err, ok ? "ok" : "FAIL");
    }
    /* no drift: silence after a loud burst decays to nothing */
    aweight_t f;
    aweight_reset(&f);
    for (int n = 0; n < RATE; n++) {
        aweight_step(&f, n % 2 ? 1.0f : -1.0f);
    }
    float tail = 0;
    for (int n = 0; n < 30 * RATE; n++) {
        tail = aweight_step(&f, 0.0f);
    }
    int stable = fabsf(tail) < 1e-6f;
    fails += !stable;
    printf("decay after a burst: %g %s\n", tail, stable ? "ok" : "FAIL");
    printf(fails ? "%d FAILED\n" : "all passed\n", fails);
    return fails != 0;
}
