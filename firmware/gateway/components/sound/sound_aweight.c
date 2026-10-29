#include "sound_aweight.h"

#include <string.h>

/* b0, b1, b2, a1, a2 per section (scipy bilinear_zpk + zpk2sos, fit in tests/unit/aweight_test.c) */
static const float SOS[3][5] = {
    {9.340517163401e-01f, -1.868103432680e+00f, 9.340517163401e-01f, -1.705509633705e+00f, 7.159875753454e-01f},
    {1.000000000000e+00f, -2.000000000000e+00f, 1.000000000000e+00f, -1.983886757268e+00f, 9.839516664158e-01f},
    {1.000000000000e+00f, 1.400000000000e-01f, 0.0f, 0.0f, 0.0f},
};

void aweight_reset(aweight_t *f)
{
    memset(f, 0, sizeof(*f));
}

float aweight_step(aweight_t *f, float x)
{
    for (int i = 0; i < 3; i++) {
        const float *c = SOS[i];
        float y = c[0] * x + f->z[i][0];
        f->z[i][0] = c[1] * x - c[3] * y + f->z[i][1];
        f->z[i][1] = c[2] * x - c[4] * y;
        x = y;
    }
    return x;
}
