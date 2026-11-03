#pragma once

/*
 * Drivers of I2C sensor chips (docs/sensors.md). Plain C over a bus abstraction, so they build and
 * run on the host too (tests/unit/sensor_drv_test.c with simulated chips).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Measured quantities; the order is the order of sdrv_quantity[] and of the API. */
typedef enum {
    SQ_TEMPERATURE, /* °C */
    SQ_HUMIDITY,    /* % */
    SQ_PRESSURE,    /* hPa */
    SQ_ILLUMINANCE, /* lx */
    SQ_UV_INDEX,
    SQ_CO2,         /* ppm, measured (NDIR, photoacoustic) */
    SQ_ECO2,        /* ppm, estimated from VOC */
    SQ_TVOC,        /* ppb */
    SQ_AQI,         /* 1..5 (UBA) */
    SQ_GAS,         /* Ω, gas sensor resistance */
    SQ_PM1,         /* µg/m³ */
    SQ_PM25,
    SQ_PM10,
    SQ_COUNT,
} sq_t;

typedef struct {
    const char *key, *name, *unit;
    const char *device_class; /* Home Assistant sensor device class, NULL for none */
    int precision;            /* decimals published */
} sdrv_quantity_t;
extern const sdrv_quantity_t sdrv_quantity[SQ_COUNT];

typedef struct {
    float v[SQ_COUNT];
    uint32_t have; /* bit per sq_t */
} sval_t;

static inline void sval_set(sval_t *out, sq_t q, float v)
{
    out->v[q] = v;
    out->have |= 1u << q;
}

/* One chip on the bus. xfer writes w (if wl), then reads r (if rl) with a repeated start; 0 = ACKed. */
typedef struct {
    void *ctx;
    uint8_t addr;
    int (*xfer)(void *ctx, uint8_t addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl);
    void (*delay_ms)(void *ctx, int ms);
} sbus_t;

#define SDRV_OK 0
#define SDRV_BUSY 1     /* no new data yet, keep the last values */
#define SDRV_EBUS (-1)  /* no ACK */
#define SDRV_EDATA (-2) /* CRC, checksum or value out of range */

/* identify(): how sure the chip at b->addr is this one */
#define SDRV_NO 0
#define SDRV_MAYBE 1 /* answers like it, but the chip has no ID to check */
#define SDRV_SURE 2  /* ID register or ID command matched */

typedef struct sdrv {
    const char *id;     /* stored in the configuration, never change */
    const char *label;  /* chip, the default friendly name */
    const char *vendor;
    const char *note;   /* boards and lookalikes, for the UI */
    uint8_t addrs[8];   /* selectable addresses, 0-terminated; the first is the usual default */
    uint32_t quantities;
    int read_s;         /* the chip needs a read this often (s), 0 = at the configured interval */
    size_t state_size;
    /* label: the exact chip when the family has variants (may stay ""); may be NULL */
    int (*identify)(sbus_t *b, char *label, size_t len);
    int (*init)(sbus_t *b, void *state);
    int (*read)(sbus_t *b, void *state, sval_t *out);
} sdrv_t;

extern const sdrv_t *const sdrv_list[];
extern const size_t sdrv_count;
const sdrv_t *sdrv_find(const char *id);
bool sdrv_has_addr(const sdrv_t *d, uint8_t addr);

/* Every driver that lists b->addr tries to identify the chip; the candidates come out with the sure ones
 * first. Returns how many. */
typedef struct {
    const sdrv_t *drv;
    int score;       /* SDRV_SURE or SDRV_MAYBE */
    char label[16];  /* exact chip, else the driver's label */
} sdrv_cand_t;
size_t sdrv_identify_all(sbus_t *b, sdrv_cand_t *out, size_t max);
/* The candidate to add without asking: the only sure one, or the only one at all. NULL: the user picks. */
const sdrv_cand_t *sdrv_pick(const sdrv_cand_t *c, size_t n);

/* Devices the scan may meet that are no supported sensor (display, RTC, ADC ...), by address. */
const char *sdrv_hint(uint8_t addr);

/* CRC-8 of Sensirion and Aosong chips: polynomial 0x31, init 0xff */
uint8_t sdrv_crc8(const uint8_t *data, size_t len, uint8_t init);

#ifdef __cplusplus
}
#endif
