/* Prints what a driver reads from a 256-byte register map (file), for sensor_xcheck.py. */
#include "sensor_drv.c"
#include <stdio.h>
#include <stdlib.h>
static uint8_t map[256];
static uint8_t ptr;
static int x(void *c, uint8_t a, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    if (wl) { ptr = w[0]; for (size_t i = 1; i < wl; i++) if (w[0] != 0xe0 && w[0] != 0x7e) map[(uint8_t)(ptr + i - 1)] = w[i]; }
    for (size_t i = 0; i < rl; i++) r[i] = map[(uint8_t)(ptr + i)];
    return 0;
}
static void dl(void *c, int ms) {}
int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb"); fread(map, 1, 256, f); fclose(f);
    const sdrv_t *d = sdrv_find(argv[2]);
    sbus_t b = {.addr = 0x77, .xfer = x, .delay_ms = dl};
    void *st = calloc(1, 512); sval_t v = {0};
    printf("init %d ", d->init(&b, st));
    printf("read %d\n", d->read(&b, st, &v));
    for (int q = 0; q < SQ_COUNT; q++) if (v.have & (1u << q)) printf("%s %.6f\n", sdrv_quantity[q].key, v.v[q]);
    printf("res_heat_0 %d\n", map[0x5a]);
}
