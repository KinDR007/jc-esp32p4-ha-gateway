#include "sensor_drv.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/*
 * Chip facts (addresses, ID registers, commands, formulas) and their sources: docs/sensors.md.
 * Drivers measure on demand (forced / single shot) where the chip allows it, so a read gives a fresh
 * value and the chip idles in between.
 */

/* units spelled as Home Assistant does (μ is the Greek letter, U+03BC) */
const sdrv_quantity_t sdrv_quantity[SQ_COUNT] = {
    [SQ_TEMPERATURE] = {"temperature", "Temperature", "°C", "temperature", 1},
    [SQ_HUMIDITY] = {"humidity", "Humidity", "%", "humidity", 1},
    [SQ_PRESSURE] = {"pressure", "Pressure", "hPa", "atmospheric_pressure", 1},
    [SQ_ILLUMINANCE] = {"illuminance", "Illuminance", "lx", "illuminance", 1},
    [SQ_UV_INDEX] = {"uv_index", "UV index", "UV index", NULL, 2},
    [SQ_CO2] = {"co2", "CO2", "ppm", "carbon_dioxide", 0},
    [SQ_ECO2] = {"eco2", "eCO2", "ppm", "carbon_dioxide", 0},
    [SQ_TVOC] = {"tvoc", "TVOC", "ppb", "volatile_organic_compounds_parts", 0},
    [SQ_AQI] = {"aqi", "Air quality index", NULL, "aqi", 0},
    [SQ_GAS] = {"gas_resistance", "Gas resistance", "Ω", NULL, 0},
    [SQ_PM1] = {"pm1", "PM1", "μg/m³", "pm1", 0},
    [SQ_PM25] = {"pm25", "PM2.5", "μg/m³", "pm25", 0},
    [SQ_PM10] = {"pm10", "PM10", "μg/m³", "pm10", 0},
};

#define Q(x) (1u << (x))

/* ---------------------------------------------------------------- bus helpers */

static int xfer(sbus_t *b, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    return b->xfer(b->ctx, b->addr, w, wl, r, rl) == 0 ? SDRV_OK : SDRV_EBUS;
}

static void wait_ms(sbus_t *b, int ms)
{
    b->delay_ms(b->ctx, ms);
}

/* register pointer, then n bytes */
static int rd(sbus_t *b, uint8_t reg, uint8_t *out, size_t n)
{
    return xfer(b, &reg, 1, out, n);
}

static int wr8(sbus_t *b, uint8_t reg, uint8_t val)
{
    uint8_t w[2] = {reg, val};
    return xfer(b, w, 2, NULL, 0);
}

static int wr16be(sbus_t *b, uint8_t reg, uint16_t val)
{
    uint8_t w[3] = {reg, val >> 8, val & 0xff};
    return xfer(b, w, 3, NULL, 0);
}

static int wr16le(sbus_t *b, uint8_t reg, uint16_t val)
{
    uint8_t w[3] = {reg, val & 0xff, val >> 8};
    return xfer(b, w, 3, NULL, 0);
}

static int rd16be(sbus_t *b, uint8_t reg, uint16_t *val)
{
    uint8_t d[2];
    int err = rd(b, reg, d, 2);
    *val = (uint16_t)(d[0] << 8 | d[1]);
    return err;
}

static int rd16le(sbus_t *b, uint8_t reg, uint16_t *val)
{
    uint8_t d[2];
    int err = rd(b, reg, d, 2);
    *val = (uint16_t)(d[1] << 8 | d[0]);
    return err;
}

static int send(sbus_t *b, const uint8_t *w, size_t n)
{
    return xfer(b, w, n, NULL, 0);
}

static int recv(sbus_t *b, uint8_t *r, size_t n)
{
    return xfer(b, NULL, 0, r, n);
}

uint8_t sdrv_crc8(const uint8_t *data, size_t len, uint8_t init)
{
    uint8_t crc = init;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = crc & 0x80 ? (uint8_t)(crc << 1) ^ 0x31 : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/* Sensirion: 16-bit command, wait, n words each followed by its CRC */
static int sensirion(sbus_t *b, uint16_t cmd, int wait, uint16_t *words, size_t n)
{
    uint8_t w[2] = {cmd >> 8, cmd & 0xff}, d[18];
    if (send(b, w, 2) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (!n) {
        return SDRV_OK;
    }
    wait_ms(b, wait);
    if (recv(b, d, n * 3) != SDRV_OK) {
        return SDRV_EBUS;
    }
    for (size_t i = 0; i < n; i++) {
        if (sdrv_crc8(d + i * 3, 2, 0xff) != d[i * 3 + 2]) {
            return SDRV_EDATA;
        }
        words[i] = (uint16_t)(d[i * 3] << 8 | d[i * 3 + 1]);
    }
    return SDRV_OK;
}

static uint16_t u16le(const uint8_t *p)
{
    return (uint16_t)(p[1] << 8 | p[0]);
}

static int16_t s16le(const uint8_t *p)
{
    return (int16_t)u16le(p);
}

static uint16_t u16be(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static int16_t s16be(const uint8_t *p)
{
    return (int16_t)u16be(p);
}

static int id8(sbus_t *b, uint8_t reg, uint8_t want)
{
    uint8_t v;
    return rd(b, reg, &v, 1) == SDRV_OK && v == want ? SDRV_SURE : SDRV_NO;
}

static void set_label(char *label, size_t len, const char *text)
{
    if (label) {
        snprintf(label, len, "%s", text);
    }
}

/* ---------------------------------------------------------------- Bosch BMP280 / BME280 */

typedef struct {
    uint16_t t1, p1;
    int16_t t2, t3, p2, p3, p4, p5, p6, p7, p8, p9;
    uint8_t h1, h3;
    int16_t h2, h4, h5;
    int8_t h6;
    bool humidity;
} bmx280_t;

static int bmx280_identify(sbus_t *b, uint8_t want)
{
    uint8_t id;
    if (rd(b, 0xd0, &id, 1) != SDRV_OK) {
        return SDRV_NO;
    }
    /* 0x56/0x57: BMP280 samples */
    return id == want || (want == 0x58 && (id == 0x56 || id == 0x57)) ? SDRV_SURE : SDRV_NO;
}

static int bmp280_identify(sbus_t *b, char *label, size_t len)
{
    return bmx280_identify(b, 0x58);
}

static int bme280_identify(sbus_t *b, char *label, size_t len)
{
    return bmx280_identify(b, 0x60);
}

static int bmx280_init(sbus_t *b, bmx280_t *s, bool humidity)
{
    uint8_t c[26];
    if (wr8(b, 0xe0, 0xb6) != SDRV_OK) { /* soft reset */
        return SDRV_EBUS;
    }
    wait_ms(b, 5);
    if (rd(b, 0x88, c, 24) != SDRV_OK) {
        return SDRV_EBUS;
    }
    s->t1 = u16le(c), s->t2 = s16le(c + 2), s->t3 = s16le(c + 4);
    s->p1 = u16le(c + 6), s->p2 = s16le(c + 8), s->p3 = s16le(c + 10), s->p4 = s16le(c + 12);
    s->p5 = s16le(c + 14), s->p6 = s16le(c + 16), s->p7 = s16le(c + 18), s->p8 = s16le(c + 20), s->p9 = s16le(c + 22);
    s->humidity = humidity;
    if (humidity) {
        uint8_t h[7];
        if (rd(b, 0xa1, &s->h1, 1) != SDRV_OK || rd(b, 0xe1, h, 7) != SDRV_OK) {
            return SDRV_EBUS;
        }
        s->h2 = s16le(h);
        s->h3 = h[2];
        s->h4 = (int16_t)((int8_t)h[3] * 16 | (h[4] & 0x0f));
        s->h5 = (int16_t)((int8_t)h[5] * 16 | h[4] >> 4);
        s->h6 = (int8_t)h[6];
        if (wr8(b, 0xf2, 0x01) != SDRV_OK) { /* humidity x1, applies with the next ctrl_meas write */
            return SDRV_EBUS;
        }
    }
    return wr8(b, 0xf5, 0x00); /* no IIR filter: forced mode reads are independent */
}

static int bmp280_init(sbus_t *b, void *st)
{
    return bmx280_init(b, st, false);
}

static int bme280_init(sbus_t *b, void *st)
{
    return bmx280_init(b, st, true);
}

/* Floating point compensation of the datasheets (BMP280 3.11.3, BME280 8.1) */
static int bmx280_read(sbus_t *b, void *st, sval_t *out)
{
    bmx280_t *s = st;
    uint8_t d[8];
    /* temperature x2, pressure x16, forced: at most 1.25 + 2.3 * 18 + 0.575 * 2 + 2.875 ms */
    if (wr8(b, 0xf4, 0x55) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 50);
    if (rd(b, 0xf7, d, s->humidity ? 8 : 6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    int32_t adc_p = d[0] << 12 | d[1] << 4 | d[2] >> 4;
    int32_t adc_t = d[3] << 12 | d[4] << 4 | d[5] >> 4;
    if (adc_t == 0x80000) {
        return SDRV_EDATA; /* skipped: no measurement ran */
    }
    double v1 = (adc_t / 16384.0 - s->t1 / 1024.0) * s->t2;
    double v2 = (adc_t / 131072.0 - s->t1 / 8192.0) * (adc_t / 131072.0 - s->t1 / 8192.0) * s->t3;
    double t_fine = v1 + v2;
    sval_set(out, SQ_TEMPERATURE, (float)(t_fine / 5120.0));

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * s->p6 / 32768.0;
    v2 = v2 + v1 * s->p5 * 2.0;
    v2 = v2 / 4.0 + s->p4 * 65536.0;
    v1 = (s->p3 * v1 * v1 / 524288.0 + s->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * s->p1;
    if (v1 != 0 && adc_p != 0x80000) {
        double p = 1048576.0 - adc_p;
        p = (p - v2 / 4096.0) * 6250.0 / v1;
        v1 = s->p9 * p * p / 2147483648.0;
        v2 = p * s->p8 / 32768.0;
        p = p + (v1 + v2 + s->p7) / 16.0;
        sval_set(out, SQ_PRESSURE, (float)(p / 100.0));
    }
    if (s->humidity) {
        int32_t adc_h = d[6] << 8 | d[7];
        if (adc_h != 0x8000) {
            double h = t_fine - 76800.0;
            h = (adc_h - (s->h4 * 64.0 + s->h5 / 16384.0 * h)) *
                (s->h2 / 65536.0 * (1.0 + s->h6 / 67108864.0 * h * (1.0 + s->h3 / 67108864.0 * h)));
            h = h * (1.0 - s->h1 * h / 524288.0);
            sval_set(out, SQ_HUMIDITY, (float)(h < 0 ? 0 : h > 100 ? 100 : h));
        }
    }
    return SDRV_OK;
}

static const sdrv_t drv_bmp280 = {
    .id = "bmp280", .label = "BMP280", .vendor = "Bosch",
    .note = "GY-BMP280 (0x76), Adafruit (0x77); many boards sold as BME280 carry this chip (no humidity)",
    .addrs = {0x76, 0x77}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_PRESSURE), .state_size = sizeof(bmx280_t),
    .identify = bmp280_identify, .init = bmp280_init, .read = bmx280_read,
};

static const sdrv_t drv_bme280 = {
    .id = "bme280", .label = "BME280", .vendor = "Bosch",
    .note = "GY-BME280 (0x76), Adafruit and SparkFun (0x77)",
    .addrs = {0x76, 0x77}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY) | Q(SQ_PRESSURE), .state_size = sizeof(bmx280_t),
    .identify = bme280_identify, .init = bme280_init, .read = bmx280_read,
};

/* ---------------------------------------------------------------- Bosch BMP180 / BMP085 */

typedef struct {
    int16_t ac1, ac2, ac3, b1, b2, mb, mc, md;
    uint16_t ac4, ac5, ac6;
} bmp180_t;

static int bmp180_identify(sbus_t *b, char *label, size_t len)
{
    return id8(b, 0xd0, 0x55);
}

static int bmp180_init(sbus_t *b, void *st)
{
    bmp180_t *s = st;
    uint8_t c[22];
    if (rd(b, 0xaa, c, 22) != SDRV_OK) {
        return SDRV_EBUS;
    }
    s->ac1 = s16be(c), s->ac2 = s16be(c + 2), s->ac3 = s16be(c + 4);
    s->ac4 = u16be(c + 6), s->ac5 = u16be(c + 8), s->ac6 = u16be(c + 10);
    s->b1 = s16be(c + 12), s->b2 = s16be(c + 14), s->mb = s16be(c + 16), s->mc = s16be(c + 18), s->md = s16be(c + 20);
    return s->ac5 && s->ac6 && s->ac4 != 0xffff ? SDRV_OK : SDRV_EDATA;
}

#define BMP180_OSS 3

/* integer compensation of the datasheet (3.5) */
static void bmp180_compensate(const bmp180_t *s, int32_t ut, int32_t up, int oss, float *celsius, float *pa)
{
    int32_t x1 = (ut - s->ac6) * s->ac5 >> 15;
    int32_t x2 = (s->mc * 2048) / (x1 + s->md);
    int32_t b5 = x1 + x2;
    *celsius = ((b5 + 8) >> 4) / 10.0f;
    int32_t b6 = b5 - 4000;
    x1 = (s->b2 * (b6 * b6 >> 12)) >> 11;
    x2 = s->ac2 * b6 >> 11;
    int32_t x3 = x1 + x2;
    int32_t b3 = ((((int32_t)s->ac1 * 4 + x3) << oss) + 2) / 4;
    x1 = s->ac3 * b6 >> 13;
    x2 = (s->b1 * (b6 * b6 >> 12)) >> 16;
    x3 = (x1 + x2 + 2) >> 2;
    uint32_t b4 = s->ac4 * (uint32_t)(x3 + 32768) >> 15;
    uint32_t b7 = ((uint32_t)up - b3) * (uint32_t)(50000 >> oss);
    int32_t p = b7 < 0x80000000u ? (int32_t)(b7 * 2 / b4) : (int32_t)(b7 / b4 * 2);
    x1 = (p >> 8) * (p >> 8);
    x1 = (x1 * 3038) >> 16;
    x2 = (-7357 * p) >> 16;
    *pa = (float)(p + ((x1 + x2 + 3791) >> 4));
}

static int bmp180_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t d[3];
    if (wr8(b, 0xf4, 0x2e) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 5);
    if (rd(b, 0xf6, d, 2) != SDRV_OK) {
        return SDRV_EBUS;
    }
    int32_t ut = d[0] << 8 | d[1];
    if (wr8(b, 0xf4, 0x34 + (BMP180_OSS << 6)) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 26);
    if (rd(b, 0xf6, d, 3) != SDRV_OK) {
        return SDRV_EBUS;
    }
    int32_t up = (d[0] << 16 | d[1] << 8 | d[2]) >> (8 - BMP180_OSS);
    float t, p;
    bmp180_compensate(st, ut, up, BMP180_OSS, &t, &p);
    sval_set(out, SQ_TEMPERATURE, t);
    sval_set(out, SQ_PRESSURE, p / 100.0f);
    return SDRV_OK;
}

static const sdrv_t drv_bmp180 = {
    .id = "bmp180", .label = "BMP180", .vendor = "Bosch", .note = "GY-68; the older BMP085 answers the same",
    .addrs = {0x77}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_PRESSURE), .state_size = sizeof(bmp180_t),
    .identify = bmp180_identify, .init = bmp180_init, .read = bmp180_read,
};

/* ---------------------------------------------------------------- Bosch BME680 / BME688 */

typedef struct {
    uint16_t t1, p1, h1, h2;
    int16_t t2, p2, p4, p5, p8, p9, gh2;
    int8_t t3, p3, p6, p7, h3, h4, h5, h7, gh1, gh3, sw_err;
    uint8_t p10, h6, heat_range, heat_val;
    bool bme688;
    float ambient; /* last temperature, for the heater target */
} bme680_t;

#define BME680_HEATER_C 320
#define BME680_HEATER_MS 150

static int bme680_identify(sbus_t *b, char *label, size_t len)
{
    if (id8(b, 0xd0, 0x61) != SDRV_SURE) {
        return SDRV_NO;
    }
    uint8_t variant = 0;
    rd(b, 0xf0, &variant, 1);
    set_label(label, len, variant == 0x01 ? "BME688" : "BME680");
    return SDRV_SURE;
}

static int bme680_init(sbus_t *b, void *st)
{
    bme680_t *s = st;
    uint8_t c1[23], c2[14], c3[5], variant;
    if (wr8(b, 0xe0, 0xb6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 10);
    if (rd(b, 0x8a, c1, 23) != SDRV_OK || rd(b, 0xe1, c2, 14) != SDRV_OK || rd(b, 0x00, c3, 5) != SDRV_OK ||
        rd(b, 0xf0, &variant, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    /* c1 starts at 0x8a, c2 at 0xe1, c3 at 0x00 */
    s->t2 = s16le(c1 + 0), s->t3 = (int8_t)c1[2];
    s->p1 = u16le(c1 + 4), s->p2 = s16le(c1 + 6), s->p3 = (int8_t)c1[8];
    s->p4 = s16le(c1 + 10), s->p5 = s16le(c1 + 12), s->p7 = (int8_t)c1[14], s->p6 = (int8_t)c1[15];
    s->p8 = s16le(c1 + 18), s->p9 = s16le(c1 + 20), s->p10 = c1[22];
    s->h2 = (uint16_t)(c2[0] << 4 | c2[1] >> 4);
    s->h1 = (uint16_t)(c2[2] << 4 | (c2[1] & 0x0f));
    s->h3 = (int8_t)c2[3], s->h4 = (int8_t)c2[4], s->h5 = (int8_t)c2[5], s->h6 = c2[6], s->h7 = (int8_t)c2[7];
    s->t1 = u16le(c2 + 8);
    s->gh2 = s16le(c2 + 10), s->gh1 = (int8_t)c2[12], s->gh3 = (int8_t)c2[13];
    s->heat_val = c3[0];
    s->heat_range = (c3[2] >> 4) & 0x03;
    s->sw_err = (int8_t)c3[4] / 16;
    s->bme688 = variant == 0x01;
    s->ambient = 25.0f;
    /* humidity x1, no filter, heater step 0: 150 ms (37 * 4 ms) */
    if (wr8(b, 0x72, 0x01) != SDRV_OK || wr8(b, 0x75, 0x00) != SDRV_OK || wr8(b, 0x64, 0x40 | 37) != SDRV_OK) {
        return SDRV_EBUS;
    }
    return SDRV_OK;
}

/* Floating point compensation of the Bosch BME68x API (bme68x.c, BME68X_USE_FPU) */
static int bme680_read(sbus_t *b, void *st, sval_t *out)
{
    bme680_t *s = st;
    uint8_t d[17];
    float amb = s->ambient;
    double v1 = s->gh1 / 16.0 + 49.0;
    double v2 = s->gh2 / 32768.0 * 0.0005 + 0.00235;
    double v3 = s->gh3 / 1024.0;
    double v4 = v1 * (1.0 + v2 * BME680_HEATER_C);
    double v5 = v4 + v3 * amb;
    double res_heat = 3.4 * (v5 * (4.0 / (4.0 + s->heat_range)) * (1.0 / (1.0 + s->heat_val * 0.002)) - 25.0);
    if (wr8(b, 0x5a, (uint8_t)(res_heat < 0 ? 0 : res_heat > 255 ? 255 : res_heat)) != SDRV_OK ||
        wr8(b, 0x71, s->bme688 ? 0x20 : 0x10) != SDRV_OK || /* run_gas, heater step 0 */
        wr8(b, 0x74, 2 << 5 | 5 << 2 | 1) != SDRV_OK) {    /* temperature x2, pressure x16, forced */
        return SDRV_EBUS;
    }
    wait_ms(b, 60 + BME680_HEATER_MS);
    for (int i = 0;; i++) {
        if (rd(b, 0x1d, d, sizeof(d)) != SDRV_OK) {
            return SDRV_EBUS;
        }
        if (d[0] & 0x80) {
            break;
        }
        if (i == 5) {
            return SDRV_BUSY;
        }
        wait_ms(b, 20);
    }
    uint32_t adc_p = d[2] << 12 | d[3] << 4 | d[4] >> 4;
    uint32_t adc_t = d[5] << 12 | d[6] << 4 | d[7] >> 4;
    uint16_t adc_h = (uint16_t)(d[8] << 8 | d[9]);

    v1 = (adc_t / 16384.0 - s->t1 / 1024.0) * s->t2;
    v2 = (adc_t / 131072.0 - s->t1 / 8192.0) * (adc_t / 131072.0 - s->t1 / 8192.0) * (s->t3 * 16.0);
    double t_fine = v1 + v2;
    double t = t_fine / 5120.0;
    sval_set(out, SQ_TEMPERATURE, (float)t);
    s->ambient = (float)t;

    v1 = t_fine / 2.0 - 64000.0;
    v2 = v1 * v1 * (s->p6 / 131072.0);
    v2 = v2 + v1 * s->p5 * 2.0;
    v2 = v2 / 4.0 + s->p4 * 65536.0;
    v1 = (s->p3 * v1 * v1 / 16384.0 + s->p2 * v1) / 524288.0;
    v1 = (1.0 + v1 / 32768.0) * s->p1;
    if (v1 != 0) {
        double p = 1048576.0 - adc_p;
        p = (p - v2 / 4096.0) * 6250.0 / v1;
        v1 = s->p9 * p * p / 2147483648.0;
        v2 = p * (s->p8 / 32768.0);
        v3 = (p / 256.0) * (p / 256.0) * (p / 256.0) * (s->p10 / 131072.0);
        p = p + (v1 + v2 + v3 + s->p7 * 128.0) / 16.0;
        sval_set(out, SQ_PRESSURE, (float)(p / 100.0));
    }

    v1 = adc_h - (s->h1 * 16.0 + s->h3 / 2.0 * t);
    v2 = v1 * (s->h2 / 262144.0 * (1.0 + s->h4 / 16384.0 * t + s->h5 / 1048576.0 * t * t));
    v3 = s->h6 / 16384.0;
    v4 = s->h7 / 2097152.0;
    double h = v2 + (v3 + v4 * t) * v2 * v2;
    sval_set(out, SQ_HUMIDITY, (float)(h < 0 ? 0 : h > 100 ? 100 : h));

    const uint8_t *g = s->bme688 ? d + 15 : d + 13; /* gas_r at 0x2c (BME688) or 0x2a (BME680) */
    uint16_t adc_gas = (uint16_t)(g[0] << 2 | g[1] >> 6);
    uint8_t range = g[1] & 0x0f;
    if ((g[1] & 0x30) == 0x30) { /* gas_valid and heat_stab */
        double r;
        if (s->bme688) {
            r = 1000000.0 * (double)(262144u >> range) / (4096.0 + ((double)adc_gas - 512.0) * 3.0);
        } else {
            static const float k1[16] = {0, 0, 0, 0, 0, -1, 0, -0.8f, 0, 0, -0.2f, -0.5f, 0, -1, 0, 0};
            static const float k2[16] = {0, 0, 0, 0, 0.1f, 0.7f, 0, -0.8f, -0.1f, 0, 0, 0, 0, 0, 0, 0};
            double g1 = 1340.0 + 5.0 * s->sw_err;
            double g2 = g1 * (1.0 + k1[range] / 100.0);
            double g3 = 1.0 + k2[range] / 100.0;
            r = 1.0 / (g3 * 0.000000125 * (double)(1u << range) * ((adc_gas - 512.0) / g2 + 1.0));
        }
        sval_set(out, SQ_GAS, (float)r);
    }
    return SDRV_OK;
}

static const sdrv_t drv_bme680 = {
    .id = "bme680", .label = "BME680", .vendor = "Bosch",
    .note = "also BME688; gas resistance only (the air quality index needs Bosch BSEC, not included)",
    .addrs = {0x77, 0x76}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY) | Q(SQ_PRESSURE) | Q(SQ_GAS),
    .state_size = sizeof(bme680_t), .identify = bme680_identify, .init = bme680_init, .read = bme680_read,
};

/* ---------------------------------------------------------------- Bosch BMP388 / BMP390 */

typedef struct {
    double t1, t2, t3, p1, p2, p3, p4, p5, p6, p7, p8, p9, p10, p11;
} bmp3_t;

static int bmp3_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t id;
    if (rd(b, 0x00, &id, 1) != SDRV_OK || (id != 0x50 && id != 0x60)) {
        return SDRV_NO;
    }
    set_label(label, len, id == 0x60 ? "BMP390" : "BMP388");
    return SDRV_SURE;
}

static int bmp3_init(sbus_t *b, void *st)
{
    bmp3_t *s = st;
    uint8_t c[21];
    if (wr8(b, 0x7e, 0xb6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 10);
    if (rd(b, 0x31, c, 21) != SDRV_OK) {
        return SDRV_EBUS;
    }
    /* datasheet 9.1: quantised coefficients */
    s->t1 = u16le(c) / pow(2, -8);
    s->t2 = u16le(c + 2) / pow(2, 30);
    s->t3 = (int8_t)c[4] / pow(2, 48);
    s->p1 = (s16le(c + 5) - 16384) / pow(2, 20);
    s->p2 = (s16le(c + 7) - 16384) / pow(2, 29);
    s->p3 = (int8_t)c[9] / pow(2, 32);
    s->p4 = (int8_t)c[10] / pow(2, 37);
    s->p5 = u16le(c + 11) / pow(2, -3);
    s->p6 = u16le(c + 13) / pow(2, 6);
    s->p7 = (int8_t)c[15] / pow(2, 8);
    s->p8 = (int8_t)c[16] / pow(2, 15);
    s->p9 = s16le(c + 17) / pow(2, 48);
    s->p10 = (int8_t)c[19] / pow(2, 48);
    s->p11 = (int8_t)c[20] / pow(2, 65);
    /* pressure x8, temperature x1, no filter */
    if (wr8(b, 0x1c, 0x03) != SDRV_OK || wr8(b, 0x1f, 0x00) != SDRV_OK) {
        return SDRV_EBUS;
    }
    return SDRV_OK;
}

static int bmp3_read(sbus_t *b, void *st, sval_t *out)
{
    bmp3_t *s = st;
    uint8_t d[6];
    if (wr8(b, 0x1b, 0x13) != SDRV_OK) { /* pressure and temperature on, forced */
        return SDRV_EBUS;
    }
    wait_ms(b, 30);
    if (rd(b, 0x04, d, 6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    double up = d[2] << 16 | d[1] << 8 | d[0];
    double ut = d[5] << 16 | d[4] << 8 | d[3];
    /* datasheet 9.2, 9.3 */
    double pd1 = ut - s->t1;
    double pd2 = pd1 * s->t2;
    double t = pd2 + pd1 * pd1 * s->t3;
    sval_set(out, SQ_TEMPERATURE, (float)t);
    double o1 = s->p5 + s->p6 * t + s->p7 * t * t + s->p8 * t * t * t;
    double o2 = up * (s->p1 + s->p2 * t + s->p3 * t * t + s->p4 * t * t * t);
    double o3 = up * up * (s->p9 + s->p10 * t) + up * up * up * s->p11;
    sval_set(out, SQ_PRESSURE, (float)((o1 + o2 + o3) / 100.0));
    return SDRV_OK;
}

static const sdrv_t drv_bmp3 = {
    .id = "bmp3xx", .label = "BMP390", .vendor = "Bosch", .note = "BMP388 and BMP390 (Adafruit 0x77)",
    .addrs = {0x77, 0x76}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_PRESSURE), .state_size = sizeof(bmp3_t),
    .identify = bmp3_identify, .init = bmp3_init, .read = bmp3_read,
};

/* ---------------------------------------------------------------- Bosch BMP581 / BMP585 */

static int bmp5_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t id;
    if (rd(b, 0x01, &id, 1) != SDRV_OK || (id != 0x50 && id != 0x51)) {
        return SDRV_NO;
    }
    set_label(label, len, id == 0x51 ? "BMP585" : "BMP581");
    return SDRV_SURE;
}

static int bmp5_init(sbus_t *b, void *st)
{
    if (wr8(b, 0x7e, 0xb6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 5);
    return wr8(b, 0x36, 0x40 | 3 << 3); /* pressure on, pressure x8, temperature x1 */
}

static int bmp5_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t d[6];
    if (wr8(b, 0x37, 0x80 | 0x02) != SDRV_OK) { /* deep standby off, forced */
        return SDRV_EBUS;
    }
    wait_ms(b, 20);
    if (rd(b, 0x1d, d, 6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    int32_t t = (int32_t)((uint32_t)d[2] << 24 | (uint32_t)d[1] << 16 | (uint32_t)d[0] << 8) >> 8;
    uint32_t p = (uint32_t)d[5] << 16 | d[4] << 8 | d[3];
    sval_set(out, SQ_TEMPERATURE, t / 65536.0f);
    if (p) {
        sval_set(out, SQ_PRESSURE, p / 64.0f / 100.0f);
    }
    return SDRV_OK;
}

static const sdrv_t drv_bmp5 = {
    .id = "bmp5xx", .label = "BMP581", .vendor = "Bosch", .note = "BMP581 and BMP585 (SparkFun, Adafruit 0x47)",
    .addrs = {0x47, 0x46}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_PRESSURE),
    .identify = bmp5_identify, .init = bmp5_init, .read = bmp5_read,
};

/* ---------------------------------------------------------------- Sensirion SHT3x */

static int sht3x_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t w;
    return sensirion(b, 0xf32d, 1, &w, 1) == SDRV_OK ? SDRV_SURE : SDRV_NO; /* status register with CRC */
}

static int sht3x_init(sbus_t *b, void *st)
{
    uint16_t w;
    return sensirion(b, 0xf32d, 1, &w, 1);
}

static int sht3x_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t w[2];
    int err = sensirion(b, 0x2400, 16, w, 2); /* high repeatability, no clock stretching, 15.5 ms */
    if (err != SDRV_OK) {
        return err;
    }
    sval_set(out, SQ_TEMPERATURE, -45.0f + 175.0f * w[0] / 65535.0f);
    sval_set(out, SQ_HUMIDITY, 100.0f * w[1] / 65535.0f);
    return SDRV_OK;
}

static const sdrv_t drv_sht3x = {
    .id = "sht3x", .label = "SHT3x", .vendor = "Sensirion", .note = "SHT30, SHT31, SHT35 (Adafruit 0x44)",
    .addrs = {0x44, 0x45}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = sht3x_identify, .init = sht3x_init, .read = sht3x_read,
};

/* ---------------------------------------------------------------- Sensirion SHT4x */

static int sht4x_cmd(sbus_t *b, uint8_t cmd, int wait, uint16_t *words)
{
    uint8_t d[6];
    if (send(b, &cmd, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, wait);
    if (recv(b, d, 6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (sdrv_crc8(d, 2, 0xff) != d[2] || sdrv_crc8(d + 3, 2, 0xff) != d[5]) {
        return SDRV_EDATA;
    }
    words[0] = u16be(d);
    words[1] = u16be(d + 3);
    return SDRV_OK;
}

static int sht4x_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t w[2];
    return sht4x_cmd(b, 0x89, 2, w) == SDRV_OK ? SDRV_SURE : SDRV_NO; /* serial number with CRC */
}

static int sht4x_init(sbus_t *b, void *st)
{
    uint16_t w[2];
    return sht4x_cmd(b, 0x89, 2, w);
}

static int sht4x_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t w[2];
    int err = sht4x_cmd(b, 0xfd, 10, w); /* high precision, 8.3 ms */
    if (err != SDRV_OK) {
        return err;
    }
    float rh = -6.0f + 125.0f * w[1] / 65535.0f;
    sval_set(out, SQ_TEMPERATURE, -45.0f + 175.0f * w[0] / 65535.0f);
    sval_set(out, SQ_HUMIDITY, rh < 0 ? 0 : rh > 100 ? 100 : rh);
    return SDRV_OK;
}

static const sdrv_t drv_sht4x = {
    .id = "sht4x", .label = "SHT4x", .vendor = "Sensirion", .note = "SHT40, SHT41, SHT45 (Adafruit 0x44)",
    .addrs = {0x44, 0x45, 0x46}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = sht4x_identify, .init = sht4x_init, .read = sht4x_read,
};

/* ---------------------------------------------------------------- Aosong AHT10 / AHT2x / DHT20 */

/* Read only: 0x38-0x3f is also the PCF8574A port expander, whose pins a write would change. */
static int aht_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t st;
    if (recv(b, &st, 1) != SDRV_OK || st == 0xff || st == 0x00) {
        return SDRV_NO;
    }
    /* status: bit 7 busy, bit 3 calibrated, bits 6:5 mode; the rest stays 0 on an idle AHT */
    return (st & 0x80) == 0 && (st & 0x67) == 0 ? SDRV_MAYBE : SDRV_NO;
}

static int aht_init(sbus_t *b, void *st)
{
    uint8_t status;
    wait_ms(b, 40);
    if (recv(b, &status, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (!(status & 0x08)) {
        /* not calibrated: AHT2x take 0xbe, the AHT10 0xe1 */
        static const uint8_t init20[3] = {0xbe, 0x08, 0x00}, init10[3] = {0xe1, 0x08, 0x00};
        if (send(b, init20, 3) != SDRV_OK && send(b, init10, 3) != SDRV_OK) {
            return SDRV_EBUS;
        }
        wait_ms(b, 10);
    }
    return SDRV_OK;
}

static int aht_read(sbus_t *b, void *st, sval_t *out)
{
    static const uint8_t trigger[3] = {0xac, 0x33, 0x00};
    uint8_t d[7];
    if (send(b, trigger, 3) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 80);
    if (recv(b, d, 7) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (d[0] & 0x80) {
        return SDRV_BUSY;
    }
    /* AHT2x send a CRC as the 7th byte; the AHT10 sends none (its 7th byte reads as 0xff) */
    if (d[6] != 0xff && sdrv_crc8(d, 6, 0xff) != d[6]) {
        return SDRV_EDATA;
    }
    uint32_t h = (uint32_t)d[1] << 12 | d[2] << 4 | d[3] >> 4;
    uint32_t t = (uint32_t)(d[3] & 0x0f) << 16 | d[4] << 8 | d[5];
    sval_set(out, SQ_HUMIDITY, h * 100.0f / 1048576.0f);
    sval_set(out, SQ_TEMPERATURE, t * 200.0f / 1048576.0f - 50.0f);
    return SDRV_OK;
}

static const sdrv_t drv_aht = {
    .id = "aht2x", .label = "AHT20", .vendor = "Aosong (ASAIR)",
    .note = "AHT10, AHT20, AHT21, AHT25, AHT30, DHT20; often on ENS160 + AHT21 boards. No ID register: "
            "a PCF8574A port expander at 0x38 looks alike",
    .addrs = {0x38, 0x39}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = aht_identify, .init = aht_init, .read = aht_read,
};

/* ---------------------------------------------------------------- Si7021 / HTU21D / SHT21 */

static int si70xx_measure(sbus_t *b, uint8_t cmd, int wait, uint16_t *raw)
{
    uint8_t d[3];
    if (send(b, &cmd, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, wait);
    if (recv(b, d, 3) != SDRV_OK) {
        return SDRV_BUSY;
    }
    if (sdrv_crc8(d, 2, 0x00) != d[2]) {
        return SDRV_EDATA;
    }
    *raw = u16be(d) & 0xfffc;
    return SDRV_OK;
}

static int si70xx_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t cmd[2] = {0xfc, 0xc9}, d[6];
    /* second part of the electronic serial number: SNB_3 names the chip */
    if (send(b, cmd, 2) == SDRV_OK && recv(b, d, 6) == SDRV_OK && sdrv_crc8(d, 2, 0x00) == d[2] &&
        sdrv_crc8(d + 3, 2, 0x00) == d[5] && (d[0] | d[1]) != 0x00) { /* zeros pass the CRC: another chip */
        set_label(label, len, d[0] == 0x15 ? "Si7021" : d[0] == 0x14 ? "Si7020" : d[0] == 0x0d ? "Si7013" : "SHT21");
        return SDRV_SURE;
    }
    /* no serial number (HTU21D, SHT21): the user register has "disable OTP reload" set after a reset
     * (0x02 HTU21D, 0x3a Si7021) */
    uint8_t reg;
    if (rd(b, 0xe7, &reg, 1) != SDRV_OK || !(reg & 0x02)) {
        return SDRV_NO;
    }
    set_label(label, len, "HTU21D");
    return SDRV_MAYBE;
}

static int si70xx_init(sbus_t *b, void *st)
{
    uint8_t reset = 0xfe;
    if (send(b, &reset, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 20);
    return SDRV_OK;
}

static int si70xx_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t rh, t;
    int err = si70xx_measure(b, 0xf5, 25, &rh); /* humidity, no hold master */
    if (err == SDRV_OK) {
        err = si70xx_measure(b, 0xf3, 55, &t); /* temperature, 14 bit, up to 50 ms on the HTU21D */
    }
    if (err != SDRV_OK) {
        return err;
    }
    float h = -6.0f + 125.0f * rh / 65536.0f;
    sval_set(out, SQ_HUMIDITY, h < 0 ? 0 : h > 100 ? 100 : h);
    sval_set(out, SQ_TEMPERATURE, -46.85f + 175.72f * t / 65536.0f);
    return SDRV_OK;
}

static const sdrv_t drv_si70xx = {
    .id = "si7021", .label = "Si7021", .vendor = "Silicon Labs / TE",
    .note = "Si7021, Si7013, HTU21D (GY-21), SHT21",
    .addrs = {0x40}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = si70xx_identify, .init = si70xx_init, .read = si70xx_read,
};

/* ---------------------------------------------------------------- TI HDC1080 */

static int ti_ids(sbus_t *b, uint8_t mreg, uint16_t manuf, uint8_t dreg, uint16_t dev, uint16_t mask)
{
    uint16_t m, d;
    return rd16be(b, mreg, &m) == SDRV_OK && m == manuf && rd16be(b, dreg, &d) == SDRV_OK && (d & mask) == dev
               ? SDRV_SURE : SDRV_NO;
}

static int hdc1080_identify(sbus_t *b, char *label, size_t len)
{
    return ti_ids(b, 0xfe, 0x5449, 0xff, 0x1050, 0xffff);
}

static int hdc1080_init(sbus_t *b, void *st)
{
    return wr16be(b, 0x02, 0x1000); /* temperature and humidity in one go, 14 bit each */
}

static int hdc1080_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t reg = 0x00, d[4];
    if (send(b, &reg, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 20);
    if (recv(b, d, 4) != SDRV_OK) {
        return SDRV_BUSY;
    }
    sval_set(out, SQ_TEMPERATURE, u16be(d) / 65536.0f * 165.0f - 40.0f);
    sval_set(out, SQ_HUMIDITY, u16be(d + 2) / 65536.0f * 100.0f);
    return SDRV_OK;
}

static const sdrv_t drv_hdc1080 = {
    .id = "hdc1080", .label = "HDC1080", .vendor = "Texas Instruments", .note = "CJMCU-1080",
    .addrs = {0x40}, .quantities = Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = hdc1080_identify, .init = hdc1080_init, .read = hdc1080_read,
};

/* ---------------------------------------------------------------- Sensirion SCD40 / SCD41 */

static int scd4x_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t w[3];
    sensirion(b, 0x3f86, 0, NULL, 0); /* stop periodic measurement: it blocks the other commands */
    wait_ms(b, 500);
    return sensirion(b, 0x3682, 1, w, 3) == SDRV_OK ? SDRV_SURE : SDRV_NO;
}

static int scd4x_init(sbus_t *b, void *st)
{
    sensirion(b, 0x3f86, 0, NULL, 0);
    wait_ms(b, 500);
    return sensirion(b, 0x21b1, 0, NULL, 0); /* periodic measurement, every 5 s */
}

static int scd4x_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t ready, w[3];
    int err = sensirion(b, 0xe4b8, 1, &ready, 1);
    if (err != SDRV_OK) {
        return err;
    }
    if (!(ready & 0x07ff)) {
        return SDRV_BUSY;
    }
    if ((err = sensirion(b, 0xec05, 1, w, 3)) != SDRV_OK) {
        return err;
    }
    sval_set(out, SQ_CO2, w[0]);
    sval_set(out, SQ_TEMPERATURE, -45.0f + 175.0f * w[1] / 65535.0f);
    sval_set(out, SQ_HUMIDITY, 100.0f * w[2] / 65535.0f);
    return SDRV_OK;
}

static const sdrv_t drv_scd4x = {
    .id = "scd4x", .label = "SCD4x", .vendor = "Sensirion", .note = "SCD40, SCD41; a value every 5 s",
    .addrs = {0x62}, .quantities = Q(SQ_CO2) | Q(SQ_TEMPERATURE) | Q(SQ_HUMIDITY),
    .identify = scd4x_identify, .init = scd4x_init, .read = scd4x_read,
};

/* ---------------------------------------------------------------- Sensirion SGP30 */

static int sgp30_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t feature;
    return sensirion(b, 0x202f, 10, &feature, 1) == SDRV_OK && (feature & 0xf000) == 0 ? SDRV_SURE : SDRV_NO;
}

static int sgp30_init(sbus_t *b, void *st)
{
    int err = sensirion(b, 0x2003, 0, NULL, 0); /* iaq_init */
    wait_ms(b, 10);
    return err;
}

/* measure_iaq must run every second for the chip's baseline compensation (read_s = 1) */
static int sgp30_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t w[2];
    int err = sensirion(b, 0x2008, 12, w, 2);
    if (err != SDRV_OK) {
        return err;
    }
    sval_set(out, SQ_ECO2, w[0]);
    sval_set(out, SQ_TVOC, w[1]);
    return SDRV_OK;
}

static const sdrv_t drv_sgp30 = {
    .id = "sgp30", .label = "SGP30", .vendor = "Sensirion", .note = "eCO2 and TVOC estimates; 400 ppm / 0 ppb for the first 15 s",
    .addrs = {0x58}, .quantities = Q(SQ_ECO2) | Q(SQ_TVOC), .read_s = 1,
    .identify = sgp30_identify, .init = sgp30_init, .read = sgp30_read,
};

/* ---------------------------------------------------------------- ScioSense ENS160 */

static int ens160_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t id;
    return rd16le(b, 0x00, &id) == SDRV_OK && id == 0x0160 ? SDRV_SURE : SDRV_NO;
}

static int ens160_init(sbus_t *b, void *st)
{
    if (wr8(b, 0x10, 0x02) != SDRV_OK) { /* standard operation */
        return SDRV_EBUS;
    }
    wait_ms(b, 20);
    return SDRV_OK;
}

static int ens160_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t d[6];
    if (rd(b, 0x20, d, 6) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (((d[0] >> 2) & 0x03) == 3) {
        return SDRV_BUSY; /* invalid output */
    }
    if ((d[1] & 0x07) == 0) {
        return SDRV_BUSY; /* no result yet */
    }
    sval_set(out, SQ_AQI, d[1] & 0x07);
    sval_set(out, SQ_TVOC, u16le(d + 2));
    sval_set(out, SQ_ECO2, u16le(d + 4));
    return SDRV_OK;
}

static const sdrv_t drv_ens160 = {
    .id = "ens160", .label = "ENS160", .vendor = "ScioSense",
    .note = "AQI 1-5, TVOC, eCO2; warm-up 3 min, full accuracy after about an hour (ENS160 + AHT21 boards: 0x53)",
    .addrs = {0x53, 0x52}, .quantities = Q(SQ_AQI) | Q(SQ_TVOC) | Q(SQ_ECO2),
    .identify = ens160_identify, .init = ens160_init, .read = ens160_read,
};

/* ---------------------------------------------------------------- ROHM BH1750 */

static int bh1750_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t d[2];
    /* no ID register, and read only: 0x23 can also be a PCF8574 port expander */
    return recv(b, d, 2) == SDRV_OK ? SDRV_MAYBE : SDRV_NO;
}

static int bh1750_init(sbus_t *b, void *st)
{
    uint8_t on = 0x01;
    return send(b, &on, 1);
}

static int bh1750_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t cmd = 0x20, d[2]; /* one time, high resolution (1 lx) */
    if (send(b, &cmd, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 180);
    if (recv(b, d, 2) != SDRV_OK) {
        return SDRV_EBUS;
    }
    sval_set(out, SQ_ILLUMINANCE, u16be(d) / 1.2f);
    return SDRV_OK;
}

static const sdrv_t drv_bh1750 = {
    .id = "bh1750", .label = "BH1750", .vendor = "ROHM", .note = "GY-302, GY-30 (0x23; 0x5c with ADDR high)",
    .addrs = {0x23, 0x5c}, .quantities = Q(SQ_ILLUMINANCE),
    .identify = bh1750_identify, .init = bh1750_init, .read = bh1750_read,
};

/* ---------------------------------------------------------------- Vishay VEML7700 */

static int veml7700_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t id, uv_id;
    if (rd16le(b, 0x07, &id) != SDRV_OK) {
        return SDRV_NO;
    }
    if ((id & 0xff) == 0x81) {
        return SDRV_SURE; /* device ID of the newer datasheets; libraries do not check it */
    }
    /* parts without it: anything at 0x10 that is not a VEML6075 (ID 0x0026 at 0x0c) */
    return rd16le(b, 0x0c, &uv_id) == SDRV_OK && uv_id != 0x0026 ? SDRV_MAYBE : SDRV_NO;
}

/* gain 1/4, 100 ms: 0.2304 lx per count, up to about 15 000 lx */
static int veml7700_init(sbus_t *b, void *st)
{
    if (wr16le(b, 0x00, 3 << 11) != SDRV_OK) {
        return SDRV_EBUS;
    }
    wait_ms(b, 5);
    return SDRV_OK;
}

static int veml7700_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t raw;
    if (rd16le(b, 0x04, &raw) != SDRV_OK) {
        return SDRV_EBUS;
    }
    double x = raw * 0.2304;
    /* non-linearity at gain 1/4 and 1/8 (Vishay application note "Designing the VEML7700 ...") */
    double lux = 6.0135e-13 * x * x * x * x - 9.3924e-9 * x * x * x + 8.1488e-5 * x * x + 1.0023 * x;
    sval_set(out, SQ_ILLUMINANCE, (float)lux);
    return SDRV_OK;
}

static const sdrv_t drv_veml7700 = {
    .id = "veml7700", .label = "VEML7700", .vendor = "Vishay", .note = "Adafruit VEML7700; VEML6030 behaves alike",
    .addrs = {0x10}, .quantities = Q(SQ_ILLUMINANCE),
    .identify = veml7700_identify, .init = veml7700_init, .read = veml7700_read,
};

/* ---------------------------------------------------------------- ams TSL2561 */

static int tsl2561_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t id;
    if (rd(b, 0x8a, &id, 1) != SDRV_OK || (id >> 4 != 0x5 && id >> 4 != 0x1)) {
        return SDRV_NO;
    }
    return SDRV_SURE;
}

static int tsl2561_init(sbus_t *b, void *st)
{
    if (wr8(b, 0x80, 0x03) != SDRV_OK || wr8(b, 0x81, 0x02) != SDRV_OK) { /* power on; gain 1x, 402 ms */
        return SDRV_EBUS;
    }
    wait_ms(b, 410);
    return SDRV_OK;
}

static int tsl2561_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t c0, c1;
    if (rd16le(b, 0xac, &c0) != SDRV_OK || rd16le(b, 0xae, &c1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (c0 >= 65000 || c1 >= 65000) {
        return SDRV_EDATA; /* saturated */
    }
    /* T/FN/CL package formula, counts scaled from gain 1x to 16x (datasheet: 402 ms, 16x) */
    double ch0 = c0 * 16.0, ch1 = c1 * 16.0, r = ch0 ? ch1 / ch0 : 0, lux;
    if (r <= 0.50) {
        lux = 0.0304 * ch0 - 0.062 * ch0 * pow(r, 1.4);
    } else if (r <= 0.61) {
        lux = 0.0224 * ch0 - 0.031 * ch1;
    } else if (r <= 0.80) {
        lux = 0.0128 * ch0 - 0.0153 * ch1;
    } else if (r <= 1.30) {
        lux = 0.00146 * ch0 - 0.00112 * ch1;
    } else {
        lux = 0;
    }
    sval_set(out, SQ_ILLUMINANCE, (float)(lux < 0 ? 0 : lux));
    return SDRV_OK;
}

static const sdrv_t drv_tsl2561 = {
    .id = "tsl2561", .label = "TSL2561", .vendor = "ams", .note = "0x39 with ADDR floating (default), 0x29 low, 0x49 high",
    .addrs = {0x39, 0x29, 0x49}, .quantities = Q(SQ_ILLUMINANCE),
    .identify = tsl2561_identify, .init = tsl2561_init, .read = tsl2561_read,
};

/* ---------------------------------------------------------------- ams TSL2591 */

typedef struct {
    uint8_t gain; /* index into tsl2591 gains */
} tsl2591_t;

static const float tsl2591_gain[2] = {1.0f, 25.0f};

static int tsl2591_identify(sbus_t *b, char *label, size_t len)
{
    return id8(b, 0xa0 | 0x12, 0x50);
}

static int tsl2591_config(sbus_t *b, tsl2591_t *s)
{
    return wr8(b, 0xa0 | 0x01, (uint8_t)(s->gain << 4 | 0x01)); /* gain, 200 ms */
}

static int tsl2591_init(sbus_t *b, void *st)
{
    tsl2591_t *s = st;
    s->gain = 1;
    if (wr8(b, 0xa0 | 0x00, 0x03) != SDRV_OK || tsl2591_config(b, s) != SDRV_OK) { /* power on, ALS on */
        return SDRV_EBUS;
    }
    wait_ms(b, 220);
    return SDRV_OK;
}

static int tsl2591_read(sbus_t *b, void *st, sval_t *out)
{
    tsl2591_t *s = st;
    uint8_t d[4];
    if (rd(b, 0xa0 | 0x14, d, 4) != SDRV_OK) {
        return SDRV_EBUS;
    }
    float c0 = u16le(d), c1 = u16le(d + 2);
    /* the full scale at 200 ms is 65535; switch the gain for the next read when out of range */
    if (c0 > 60000 && s->gain == 1) {
        s->gain = 0;
        tsl2591_config(b, s);
        return SDRV_BUSY;
    }
    if (c0 < 100 && s->gain == 0) {
        s->gain = 1;
        tsl2591_config(b, s);
        return SDRV_BUSY;
    }
    if (c0 >= 65535) {
        return SDRV_EDATA; /* saturated even at gain 1 */
    }
    if (c0 == 0) {
        sval_set(out, SQ_ILLUMINANCE, 0);
        return SDRV_OK;
    }
    float cpl = 200.0f * tsl2591_gain[s->gain] / 408.0f; /* Adafruit_TSL2591: lux coefficient 408 */
    float lux = (c0 - c1) * (1.0f - c1 / c0) / cpl;
    sval_set(out, SQ_ILLUMINANCE, lux < 0 ? 0 : lux);
    return SDRV_OK;
}

static const sdrv_t drv_tsl2591 = {
    .id = "tsl2591", .label = "TSL2591", .vendor = "ams", .note = "Adafruit TSL2591",
    .addrs = {0x29}, .quantities = Q(SQ_ILLUMINANCE), .state_size = sizeof(tsl2591_t),
    .identify = tsl2591_identify, .init = tsl2591_init, .read = tsl2591_read,
};

/* ---------------------------------------------------------------- Lite-On LTR390 */

typedef struct {
    bool uv; /* the mode measuring now */
} ltr390_t;

static int ltr390_identify(sbus_t *b, char *label, size_t len)
{
    return id8(b, 0x06, 0xb2);
}

/* gain 18 (UV) / 3 (light), 18 bit, 100 ms */
static int ltr390_mode(sbus_t *b, ltr390_t *s, bool uv)
{
    s->uv = uv;
    if (wr8(b, 0x05, uv ? 0x04 : 0x01) != SDRV_OK || wr8(b, 0x00, uv ? 0x0a : 0x02) != SDRV_OK) {
        return SDRV_EBUS;
    }
    return SDRV_OK;
}

static int ltr390_init(sbus_t *b, void *st)
{
    if (wr8(b, 0x04, 0x22) != SDRV_OK) {
        return SDRV_EBUS;
    }
    return ltr390_mode(b, st, false);
}

/* UV and light take turns: each read gives the one measured since the last read */
static int ltr390_read(sbus_t *b, void *st, sval_t *out)
{
    ltr390_t *s = st;
    uint8_t status, d[3];
    if (rd(b, 0x07, &status, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (!(status & 0x08)) {
        return SDRV_BUSY;
    }
    if (rd(b, s->uv ? 0x10 : 0x0d, d, 3) != SDRV_OK) {
        return SDRV_EBUS;
    }
    uint32_t raw = (uint32_t)(d[2] & 0x0f) << 16 | d[1] << 8 | d[0];
    if (s->uv) {
        /* 2300 counts per UV index at gain 18, 20 bit (400 ms); 18 bit is a quarter */
        sval_set(out, SQ_UV_INDEX, raw / (2300.0f / 4.0f));
    } else {
        sval_set(out, SQ_ILLUMINANCE, 0.6f * raw / 3.0f); /* gain 3, integration factor 1 at 100 ms */
    }
    return ltr390_mode(b, s, !s->uv) == SDRV_OK ? SDRV_OK : SDRV_EBUS;
}

static const sdrv_t drv_ltr390 = {
    .id = "ltr390", .label = "LTR390", .vendor = "Lite-On", .note = "LTR390-UV-01 (Adafruit, DFRobot); UV and light alternate",
    .addrs = {0x53}, .quantities = Q(SQ_UV_INDEX) | Q(SQ_ILLUMINANCE), .state_size = sizeof(ltr390_t),
    .identify = ltr390_identify, .init = ltr390_init, .read = ltr390_read,
};

/* ---------------------------------------------------------------- TI OPT3001 */

static int opt3001_identify(sbus_t *b, char *label, size_t len)
{
    return ti_ids(b, 0x7e, 0x5449, 0x7f, 0x3001, 0xffff);
}

static int opt3001_init(sbus_t *b, void *st)
{
    return wr16be(b, 0x01, 0xcc10); /* automatic range, 800 ms, continuous */
}

static int opt3001_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t r;
    if (rd16be(b, 0x00, &r) != SDRV_OK) {
        return SDRV_EBUS;
    }
    sval_set(out, SQ_ILLUMINANCE, 0.01f * (float)(1u << (r >> 12)) * (r & 0x0fff));
    return SDRV_OK;
}

static const sdrv_t drv_opt3001 = {
    .id = "opt3001", .label = "OPT3001", .vendor = "Texas Instruments", .note = "CJMCU-3001, SparkFun (0x44)",
    .addrs = {0x44, 0x45, 0x46, 0x47}, .quantities = Q(SQ_ILLUMINANCE),
    .identify = opt3001_identify, .init = opt3001_init, .read = opt3001_read,
};

/* ---------------------------------------------------------------- Maxim MAX44009 */

static int max44009_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t th_hi, th_lo;
    /* no ID register; threshold registers keep their reset values unless someone set them */
    return rd(b, 0x05, &th_hi, 1) == SDRV_OK && rd(b, 0x06, &th_lo, 1) == SDRV_OK && (th_hi == 0xff || th_hi == 0xef) && th_lo == 0x00
               ? SDRV_MAYBE : SDRV_NO;
}

static int max44009_init(sbus_t *b, void *st)
{
    return wr8(b, 0x02, 0x03); /* default: automatic range, 800 ms cycle */
}

static int max44009_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t hi, lo;
    /* two transfers with a repeated start each, the chip does not auto-increment */
    if (rd(b, 0x03, &hi, 1) != SDRV_OK || rd(b, 0x04, &lo, 1) != SDRV_OK) {
        return SDRV_EBUS;
    }
    int exp = hi >> 4;
    if (exp == 0x0f) {
        return SDRV_EDATA;
    }
    sval_set(out, SQ_ILLUMINANCE, (float)(1u << exp) * ((hi & 0x0f) << 4 | (lo & 0x0f)) * 0.045f);
    return SDRV_OK;
}

static const sdrv_t drv_max44009 = {
    .id = "max44009", .label = "MAX44009", .vendor = "Maxim", .note = "GY-49; no ID register",
    .addrs = {0x4a, 0x4b}, .quantities = Q(SQ_ILLUMINANCE),
    .identify = max44009_identify, .init = max44009_init, .read = max44009_read,
};

/* ---------------------------------------------------------------- TI TMP117 */

static int tmp117_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t id;
    return rd16be(b, 0x0f, &id) == SDRV_OK && (id & 0x0fff) == 0x117 ? SDRV_SURE : SDRV_NO;
}

static int tmp117_init(sbus_t *b, void *st)
{
    return SDRV_OK; /* continuous conversion by default, 1 s cycle with averaging */
}

static int tmp117_read(sbus_t *b, void *st, sval_t *out)
{
    uint16_t raw;
    if (rd16be(b, 0x00, &raw) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (raw == 0x8000) {
        return SDRV_BUSY;
    }
    sval_set(out, SQ_TEMPERATURE, (int16_t)raw / 128.0f);
    return SDRV_OK;
}

static const sdrv_t drv_tmp117 = {
    .id = "tmp117", .label = "TMP117", .vendor = "Texas Instruments", .note = "±0.1 °C; SparkFun, Adafruit (0x48)",
    .addrs = {0x48, 0x49, 0x4a, 0x4b}, .quantities = Q(SQ_TEMPERATURE),
    .identify = tmp117_identify, .init = tmp117_init, .read = tmp117_read,
};

/* ---------------------------------------------------------------- Microchip MCP9808 */

static int mcp9808_identify(sbus_t *b, char *label, size_t len)
{
    uint16_t m, d;
    return rd16be(b, 0x06, &m) == SDRV_OK && m == 0x0054 && rd16be(b, 0x07, &d) == SDRV_OK && d >> 8 == 0x04
               ? SDRV_SURE : SDRV_NO;
}

static int mcp9808_init(sbus_t *b, void *st)
{
    return wr8(b, 0x08, 0x03); /* 0.0625 °C */
}

static int mcp9808_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t d[2];
    if (rd(b, 0x05, d, 2) != SDRV_OK) {
        return SDRV_EBUS;
    }
    float t = (d[0] & 0x0f) * 16.0f + d[1] / 16.0f;
    sval_set(out, SQ_TEMPERATURE, d[0] & 0x10 ? t - 256.0f : t);
    return SDRV_OK;
}

static const sdrv_t drv_mcp9808 = {
    .id = "mcp9808", .label = "MCP9808", .vendor = "Microchip",
    .note = "0x18 is taken by the board's audio codec: set A0-A2 for 0x19-0x1f",
    .addrs = {0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f}, .quantities = Q(SQ_TEMPERATURE),
    .identify = mcp9808_identify, .init = mcp9808_init, .read = mcp9808_read,
};

/* ---------------------------------------------------------------- Plantower PMSA003I */

static int pmsa_frame(sbus_t *b, uint8_t *d)
{
    if (recv(b, d, 32) != SDRV_OK) {
        return SDRV_EBUS;
    }
    if (d[0] != 0x42 || d[1] != 0x4d) {
        return SDRV_EDATA;
    }
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) {
        sum += d[i];
    }
    return sum == u16be(d + 30) ? SDRV_OK : SDRV_EDATA;
}

static int pmsa_identify(sbus_t *b, char *label, size_t len)
{
    uint8_t d[32];
    return pmsa_frame(b, d) == SDRV_OK ? SDRV_SURE : SDRV_NO;
}

static int pmsa_init(sbus_t *b, void *st)
{
    return SDRV_OK;
}

static int pmsa_read(sbus_t *b, void *st, sval_t *out)
{
    uint8_t d[32];
    int err = pmsa_frame(b, d);
    if (err != SDRV_OK) {
        return err;
    }
    /* atmospheric environment values */
    sval_set(out, SQ_PM1, u16be(d + 10));
    sval_set(out, SQ_PM25, u16be(d + 12));
    sval_set(out, SQ_PM10, u16be(d + 14));
    return SDRV_OK;
}

static const sdrv_t drv_pmsa003i = {
    .id = "pmsa003i", .label = "PMSA003I", .vendor = "Plantower", .note = "Adafruit PMSA003I (the I2C version; the UART PMS sensors are not I2C)",
    .addrs = {0x12}, .quantities = Q(SQ_PM1) | Q(SQ_PM25) | Q(SQ_PM10),
    .identify = pmsa_identify, .init = pmsa_init, .read = pmsa_read,
};

/* ---------------------------------------------------------------- list */

/* Identification order matters only where a probe could disturb another chip: OPT3001 (register read)
 * before the Sensirion commands at 0x44/0x45, HDC1080 before Si70xx at 0x40 (0xfe resets a Si70xx). */
const sdrv_t *const sdrv_list[] = {
    &drv_bme280, &drv_bmp280, &drv_bme680, &drv_bmp3, &drv_bmp180, &drv_bmp5,
    &drv_opt3001, &drv_sht4x, &drv_sht3x, &drv_hdc1080, &drv_si70xx, &drv_aht,
    &drv_scd4x, &drv_sgp30, &drv_ens160, &drv_pmsa003i,
    &drv_bh1750, &drv_veml7700, &drv_tsl2591, &drv_tsl2561, &drv_ltr390, &drv_max44009,
    &drv_tmp117, &drv_mcp9808,
};
const size_t sdrv_count = sizeof(sdrv_list) / sizeof(sdrv_list[0]);

const sdrv_t *sdrv_find(const char *id)
{
    for (size_t i = 0; i < sdrv_count; i++) {
        if (strcmp(sdrv_list[i]->id, id) == 0) {
            return sdrv_list[i];
        }
    }
    return NULL;
}

bool sdrv_has_addr(const sdrv_t *d, uint8_t addr)
{
    for (size_t i = 0; i < sizeof(d->addrs) && d->addrs[i]; i++) {
        if (d->addrs[i] == addr) {
            return true;
        }
    }
    return false;
}

size_t sdrv_identify_all(sbus_t *b, sdrv_cand_t *out, size_t max)
{
    size_t n = 0;
    for (size_t i = 0; i < sdrv_count && n < max; i++) {
        const sdrv_t *d = sdrv_list[i];
        if (!sdrv_has_addr(d, b->addr)) {
            continue;
        }
        char label[16] = "";
        int score = d->identify(b, label, sizeof(label));
        if (score == SDRV_NO) {
            continue;
        }
        out[n].drv = d;
        out[n].score = score;
        snprintf(out[n].label, sizeof(out[n].label), "%s", label[0] ? label : d->label);
        n++;
    }
    /* sure ones first, otherwise in list order (stable) */
    for (size_t i = 1; i < n; i++) {
        for (size_t j = i; j > 0 && out[j].score > out[j - 1].score; j--) {
            sdrv_cand_t t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    }
    return n;
}

const sdrv_cand_t *sdrv_pick(const sdrv_cand_t *c, size_t n)
{
    size_t sure = 0;
    for (size_t i = 0; i < n; i++) {
        sure += c[i].score == SDRV_SURE;
    }
    return sure == 1 || (sure == 0 && n == 1) ? &c[0] : NULL;
}

const char *sdrv_hint(uint8_t addr)
{
    static const struct {
        uint8_t from, to;
        const char *what;
    } hints[] = {
        /* wide ranges first: a narrower one listed later wins */
        {0x20, 0x27, "port expander (PCF8574, MCP23017)"},
        {0x38, 0x3f, "port expander (PCF8574A)"},
        {0x40, 0x4f, "current sensor (INA219, INA226) or PWM driver (PCA9685)"},
        {0x50, 0x57, "EEPROM (AT24C)"},
        {0x68, 0x69, "RTC (DS3231) or IMU (MPU6050), SEN5x (0x69)"},
        {0x14, 0x14, "display touch controller (GT911)"},
        {0x5d, 0x5d, "display touch controller (GT911)"},
        {0x36, 0x36, "camera (OV5647)"},
        {0x70, 0x77, "I2C multiplexer (TCA9548A)"},
        {0x18, 0x18, "ES8311 audio codec (on the board)"},
        {0x3c, 0x3d, "OLED display (SSD1306, SH1106) or camera (OV5640)"},
        {0x45, 0x45, "display backlight controller (Waveshare DSI panels)"},
        {0x48, 0x4b, "ADC (ADS1115) or temperature sensor (LM75, TMP102)"},
        {0x59, 0x59, "SGP40 / SGP41 (not supported)"},
        {0x5a, 0x5b, "CCS811 or MLX90614 (not supported)"},
        {0x5c, 0x5c, "AM2320 (not supported)"},
        {0x61, 0x61, "SCD30 (not supported)"},
        {0x76, 0x77, "DPS310, SPL06, MS5611 (not supported)"},
    };
    const char *found = NULL;
    for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
        if (addr >= hints[i].from && addr <= hints[i].to) {
            found = hints[i].what;
        }
    }
    return found;
}
