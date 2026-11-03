/* I2C sensor drivers against simulated chips: identification at shared addresses, byte order, CRC and
 * the conversions (datasheet examples where the datasheet has one).
 *   cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I firmware/gateway/components/sensors -o /tmp/sensor_drv_test \
 *      tests/unit/sensor_drv_test.c -lm && /tmp/sensor_drv_test
 * The driver source is included to reach its static compensation functions. */
#include "sensor_drv.c"

#include <stdio.h>
#include <stdlib.h>

static int failures;

#define CHECK(cond, ...)                                                                                               \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            printf("FAIL %s:%d ", __FILE__, __LINE__);                                                                 \
            printf(__VA_ARGS__);                                                                                       \
            printf("\n");                                                                                              \
            failures++;                                                                                                \
        }                                                                                                              \
    } while (0)

/* ---------------------------------------------------------------- simulated chips */

typedef enum { REG8, REG16BE, REG16LE, CMD } kind_t;

typedef struct {
    uint8_t cmd[4];
    uint8_t cmd_len;
    uint8_t resp[32];
    uint8_t resp_len;
} cmd_t;

typedef struct {
    kind_t kind;
    uint8_t reg8[256];
    uint16_t reg16[256];
    uint8_t ptr;
    cmd_t cmds[8];
    int ncmds;
    const cmd_t *pending;
    uint8_t idle[32]; /* CMD: what a read without a command returns */
    uint8_t idle_len;
} sim_t;

static int sim_xfer(void *ctx, uint8_t addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl)
{
    sim_t *s = ctx;
    (void)addr;
    if (s->kind == CMD) {
        if (wl) {
            s->pending = NULL;
            for (int i = 0; i < s->ncmds; i++) {
                if (s->cmds[i].cmd_len == wl && memcmp(s->cmds[i].cmd, w, wl) == 0) {
                    s->pending = &s->cmds[i];
                }
            }
            if (!s->pending) {
                return -1; /* unknown command: NACK */
            }
        }
        if (rl) {
            /* a command without an answer leaves what a plain read returns */
            bool own = s->pending && s->pending->resp_len;
            const uint8_t *src = own ? s->pending->resp : s->idle;
            size_t have = own ? s->pending->resp_len : s->idle_len;
            if (rl > have) {
                return -1;
            }
            memcpy(r, src, rl);
            s->pending = NULL;
        }
        return 0;
    }
    if (wl) {
        s->ptr = w[0];
        if (s->kind == REG8) {
            for (size_t i = 1; i < wl; i++) {
                s->reg8[(uint8_t)(s->ptr + i - 1)] = w[i];
            }
        } else if (wl == 3) {
            s->reg16[s->ptr] = s->kind == REG16BE ? (uint16_t)(w[1] << 8 | w[2]) : (uint16_t)(w[2] << 8 | w[1]);
        }
    }
    for (size_t i = 0; i < rl; i++) {
        if (s->kind == REG8) {
            r[i] = s->reg8[(uint8_t)(s->ptr + i)];
        } else {
            uint16_t v = s->reg16[(uint8_t)(s->ptr + i / 2)];
            r[i] = (s->kind == REG16BE) == (i % 2 == 0) ? v >> 8 : v & 0xff;
        }
    }
    return 0;
}

static void sim_delay(void *ctx, int ms)
{
    (void)ctx;
    (void)ms;
}

static sbus_t bus(sim_t *s, uint8_t addr)
{
    return (sbus_t){.ctx = s, .addr = addr, .xfer = sim_xfer, .delay_ms = sim_delay};
}

static sim_t *sim_new(kind_t kind)
{
    sim_t *s = calloc(1, sizeof(sim_t));
    s->kind = kind;
    return s;
}

/* a command answering words, each with its CRC (Sensirion: init 0xff, Si70xx: init 0x00) */
static void sim_cmd(sim_t *s, const uint8_t *cmd, int cmd_len, const uint16_t *words, int n, uint8_t crc_init)
{
    cmd_t *c = &s->cmds[s->ncmds++];
    memcpy(c->cmd, cmd, cmd_len);
    c->cmd_len = (uint8_t)cmd_len;
    for (int i = 0; i < n; i++) {
        c->resp[i * 3] = words[i] >> 8;
        c->resp[i * 3 + 1] = words[i] & 0xff;
        c->resp[i * 3 + 2] = sdrv_crc8(c->resp + i * 3, 2, crc_init);
    }
    c->resp_len = (uint8_t)(n * 3);
}

static void sim_cmd16(sim_t *s, uint16_t cmd, const uint16_t *words, int n)
{
    uint8_t c[2] = {cmd >> 8, cmd & 0xff};
    sim_cmd(s, c, 2, words, n, 0xff);
}

static void sim_raw(sim_t *s, const uint8_t *cmd, int cmd_len, const uint8_t *resp, int resp_len)
{
    cmd_t *c = &s->cmds[s->ncmds++];
    memcpy(c->cmd, cmd, cmd_len);
    c->cmd_len = (uint8_t)cmd_len;
    memcpy(c->resp, resp, resp_len);
    c->resp_len = (uint8_t)resp_len;
}

static void le16(uint8_t *p, int v)
{
    p[0] = v & 0xff;
    p[1] = (v >> 8) & 0xff;
}

static void be16(uint8_t *p, int v)
{
    p[0] = (v >> 8) & 0xff;
    p[1] = v & 0xff;
}

/* ---------------------------------------------------------------- helpers */

static bool near(float a, float b, float tol)
{
    return fabsf(a - b) <= tol;
}

/* identify at addr: exactly the driver want (id) comes out, with the score */
static void expect_identified(sim_t *s, uint8_t addr, const char *want, int score, const char *label)
{
    sbus_t b = bus(s, addr);
    sdrv_cand_t c[8];
    size_t n = sdrv_identify_all(&b, c, 8);
    const sdrv_cand_t *pick = sdrv_pick(c, n);
    CHECK(n >= 1 && strcmp(c[0].drv->id, want) == 0, "0x%02x: want %s first, got %zu candidates (%s)", addr, want, n,
          n ? c[0].drv->id : "-");
    if (n) {
        CHECK(c[0].score == score, "0x%02x %s: score %d, want %d", addr, want, c[0].score, score);
        CHECK(!label || strcmp(c[0].label, label) == 0, "0x%02x %s: label %s, want %s", addr, want, c[0].label, label);
    }
    for (size_t i = 1; i < n; i++) {
        CHECK(c[i].score < SDRV_SURE, "0x%02x: second sure candidate %s next to %s", addr, c[i].drv->id, want);
    }
    CHECK(pick && pick->drv == c[0].drv, "0x%02x %s: not picked automatically", addr, want);
}

/* init and one read through the driver */
static sval_t measure(sim_t *s, uint8_t addr, const char *id, int want_err)
{
    const sdrv_t *d = sdrv_find(id);
    sbus_t b = bus(s, addr);
    sval_t v = {0};
    void *st = calloc(1, d->state_size ? d->state_size : 1);
    int err = d->init(&b, st);
    CHECK(err == SDRV_OK, "%s init: %d", id, err);
    err = d->read(&b, st, &v);
    CHECK(err == want_err, "%s read: %d, want %d", id, err, want_err);
    free(st);
    return v;
}

#define VAL(val, q, want, tol)                                                                                         \
    CHECK(((val).have & (1u << (q))) && near((val).v[q], (want), (tol)), #q " = %f, want %f", (double)(val).v[q],  \
          (double)(want))

/* ---------------------------------------------------------------- Bosch */

/* BMP280 datasheet 3.12: compensation example (T = 25.08 °C, P = 100653.27 Pa) */
static sim_t *bmp280_sim(uint8_t chip_id)
{
    static const int cal[12] = {27504, 26435, -1000, 36477, -10685, 3024, 2855, 140, -7, 15500, -14600, 6000};
    sim_t *s = sim_new(REG8);
    s->reg8[0xd0] = chip_id;
    for (int i = 0; i < 12; i++) {
        le16(&s->reg8[0x88 + 2 * i], cal[i]);
    }
    /* adc_P = 415148, adc_T = 519888 */
    uint8_t data[6] = {415148 >> 12, (415148 >> 4) & 0xff, (415148 & 0x0f) << 4,
                       519888 >> 12, (519888 >> 4) & 0xff, (519888 & 0x0f) << 4};
    memcpy(&s->reg8[0xf7], data, 6);
    return s;
}

static void test_bosch(void)
{
    sim_t *s = bmp280_sim(0x58);
    expect_identified(s, 0x76, "bmp280", SDRV_SURE, "BMP280");
    sval_t v = measure(s, 0x76, "bmp280", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.08f, 0.01f);
    VAL(v, SQ_PRESSURE, 1006.5327f, 0.01f);
    CHECK(!(v.have & (1u << SQ_HUMIDITY)), "BMP280 reports humidity");

    /* a "BME280" board with a BMP280 on it is recognised as such; samples 0x56/0x57 count as BMP280 */
    s->reg8[0xd0] = 0x57;
    expect_identified(s, 0x77, "bmp280", SDRV_SURE, NULL);

    /* BME280: same T/P calibration; humidity H1..H6 of a real part; adc_H for about 50 % */
    s->reg8[0xd0] = 0x60;
    s->reg8[0xa1] = 75;
    uint8_t h[7] = {0x6d, 0x01, 0x00, 0x13, 0x2a, 0x03, 0x1e}; /* H2 365, H3 0, H4 314, H5 50, H6 30 */
    memcpy(&s->reg8[0xe1], h, 7);
    be16(&s->reg8[0xfd], 26000);
    expect_identified(s, 0x76, "bme280", SDRV_SURE, NULL);
    v = measure(s, 0x76, "bme280", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.08f, 0.01f);
    CHECK((v.have & (1u << SQ_HUMIDITY)) && v.v[SQ_HUMIDITY] > 0 && v.v[SQ_HUMIDITY] < 100, "BME280 humidity %f",
          (double)v.v[SQ_HUMIDITY]);
    CHECK(s->reg8[0xf2] == 0x01, "BME280 ctrl_hum not written");

    /* BME680 / BME688 */
    s->reg8[0xd0] = 0x61;
    s->reg8[0xf0] = 0x01;
    expect_identified(s, 0x77, "bme680", SDRV_SURE, "BME688");
    s->reg8[0xf0] = 0x00;
    expect_identified(s, 0x77, "bme680", SDRV_SURE, "BME680");

    /* BMP390 / BMP388 (chip id at 0x00, nothing at 0xd0) */
    sim_t *p3 = sim_new(REG8);
    p3->reg8[0x00] = 0x60;
    expect_identified(p3, 0x77, "bmp3xx", SDRV_SURE, "BMP390");
    p3->reg8[0x00] = 0x50;
    expect_identified(p3, 0x76, "bmp3xx", SDRV_SURE, "BMP388");

    /* BMP581: 24-bit temperature / 65536, pressure / 64 */
    sim_t *p5 = sim_new(REG8);
    p5->reg8[0x01] = 0x50;
    int32_t t = (int32_t)(21.5 * 65536), p = (int32_t)(98765.0 * 64);
    uint8_t d5[6] = {t & 0xff, (t >> 8) & 0xff, (t >> 16) & 0xff, p & 0xff, (p >> 8) & 0xff, (p >> 16) & 0xff};
    memcpy(&p5->reg8[0x1d], d5, 6);
    expect_identified(p5, 0x47, "bmp5xx", SDRV_SURE, "BMP581");
    v = measure(p5, 0x47, "bmp5xx", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 21.5f, 0.001f);
    VAL(v, SQ_PRESSURE, 987.65f, 0.001f);
    t = (int32_t)(-5.25 * 65536);
    p5->reg8[0x1d] = t & 0xff, p5->reg8[0x1e] = (t >> 8) & 0xff, p5->reg8[0x1f] = (t >> 16) & 0xff;
    v = measure(p5, 0x47, "bmp5xx", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, -5.25f, 0.001f);

    /* BMP180 datasheet 3.5 example (oss 0): 15.0 °C, 69964 Pa */
    bmp180_t c = {.ac1 = 408, .ac2 = -72, .ac3 = -14383, .ac4 = 32741, .ac5 = 32757, .ac6 = 23153,
                  .b1 = 6190, .b2 = 4, .mb = -32768, .mc = -8711, .md = 2868};
    float tc, pa;
    bmp180_compensate(&c, 27898, 23843, 0, &tc, &pa);
    CHECK(near(tc, 15.0f, 0.001f), "BMP180 T %f", (double)tc);
    CHECK(near(pa, 69964.0f, 0.5f), "BMP180 P %f", (double)pa);
    sim_t *p180 = sim_new(REG8);
    p180->reg8[0xd0] = 0x55;
    expect_identified(p180, 0x77, "bmp180", SDRV_SURE, NULL);
}

/* ---------------------------------------------------------------- humidity */

static void test_humidity(void)
{
    /* 0x44: SHT3x, SHT4x and OPT3001 share it, exactly one is recognised each time */
    sim_t *sht3 = sim_new(CMD);
    sim_cmd16(sht3, 0xf32d, (uint16_t[]){0x0000}, 1);
    sim_cmd16(sht3, 0x2400, (uint16_t[]){0x6666, 0x8000}, 2);
    expect_identified(sht3, 0x44, "sht3x", SDRV_SURE, NULL);
    sval_t v = measure(sht3, 0x44, "sht3x", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.0f, 0.01f);
    VAL(v, SQ_HUMIDITY, 50.0f, 0.01f);
    sht3->cmds[1].resp[2] ^= 1; /* bad CRC */
    measure(sht3, 0x44, "sht3x", SDRV_EDATA);

    sim_t *sht4 = sim_new(CMD);
    sim_cmd(sht4, (uint8_t[]){0x89}, 1, (uint16_t[]){0x1234, 0x5678}, 2, 0xff);
    sim_cmd(sht4, (uint8_t[]){0xfd}, 1, (uint16_t[]){0x6666, 0x8000}, 2, 0xff);
    expect_identified(sht4, 0x44, "sht4x", SDRV_SURE, NULL);
    v = measure(sht4, 0x44, "sht4x", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.0f, 0.01f);
    VAL(v, SQ_HUMIDITY, 56.5f, 0.01f);

    sim_t *opt = sim_new(REG16BE);
    opt->reg16[0x7e] = 0x5449;
    opt->reg16[0x7f] = 0x3001;
    opt->reg16[0x00] = 0x3123;
    expect_identified(opt, 0x44, "opt3001", SDRV_SURE, NULL);
    v = measure(opt, 0x44, "opt3001", SDRV_OK);
    VAL(v, SQ_ILLUMINANCE, 23.28f, 0.001f);
    CHECK(opt->reg16[0x01] == 0xcc10, "OPT3001 config 0x%04x", opt->reg16[0x01]);

    /* 0x38: AHT2x, read-only identification, CRC in the 7th byte */
    sim_t *aht = sim_new(CMD);
    aht->idle[0] = 0x18;
    aht->idle_len = 1;
    uint8_t m[7] = {0x18, 0x80, 0x00, 0x06, 0x00, 0x00, 0};
    m[6] = sdrv_crc8(m, 6, 0xff);
    sim_raw(aht, (uint8_t[]){0xac, 0x33, 0x00}, 3, m, 7);
    expect_identified(aht, 0x38, "aht2x", SDRV_MAYBE, NULL);
    v = measure(aht, 0x38, "aht2x", SDRV_OK);
    VAL(v, SQ_HUMIDITY, 50.0f, 0.001f);
    VAL(v, SQ_TEMPERATURE, 25.0f, 0.001f);
    aht->cmds[0].resp[6] ^= 1;
    measure(aht, 0x38, "aht2x", SDRV_EDATA);
    aht->idle[0] = 0xff; /* a PCF8574A with its pins high */
    sbus_t b = bus(aht, 0x38);
    sdrv_cand_t c[4];
    CHECK(sdrv_identify_all(&b, c, 4) == 0, "PCF8574A taken for an AHT");

    /* 0x40: HDC1080 and Si7021 */
    sim_t *hdc = sim_new(REG16BE);
    hdc->reg16[0xfe] = 0x5449;
    hdc->reg16[0xff] = 0x1050;
    hdc->reg16[0x00] = 0x6000;
    hdc->reg16[0x01] = 0x8000;
    expect_identified(hdc, 0x40, "hdc1080", SDRV_SURE, NULL);
    v = measure(hdc, 0x40, "hdc1080", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 21.875f, 0.001f);
    VAL(v, SQ_HUMIDITY, 50.0f, 0.001f);

    sim_t *si = sim_new(CMD);
    uint8_t snb[6] = {0x15, 0xff, 0, 0xff, 0xff, 0};
    snb[2] = sdrv_crc8(snb, 2, 0x00);
    snb[5] = sdrv_crc8(snb + 3, 2, 0x00);
    sim_raw(si, (uint8_t[]){0xfc, 0xc9}, 2, snb, 6);
    sim_raw(si, (uint8_t[]){0xfe}, 1, NULL, 0);
    sim_cmd(si, (uint8_t[]){0xf5}, 1, (uint16_t[]){0x7c82}, 1, 0x00); /* status bits set: masked */
    sim_cmd(si, (uint8_t[]){0xf3}, 1, (uint16_t[]){0x6680}, 1, 0x00);
    expect_identified(si, 0x40, "si7021", SDRV_SURE, "Si7021");
    v = measure(si, 0x40, "si7021", SDRV_OK);
    VAL(v, SQ_HUMIDITY, 125.0f * 0x7c80 / 65536.0f - 6.0f, 0.001f);
    VAL(v, SQ_TEMPERATURE, 175.72f * 0x6680 / 65536.0f - 46.85f, 0.001f);

    /* HTU21D: no serial number, user register 0x02 */
    sim_t *htu = sim_new(CMD);
    sim_raw(htu, (uint8_t[]){0xe7}, 1, (uint8_t[]){0x02}, 1);
    expect_identified(htu, 0x40, "si7021", SDRV_MAYBE, "HTU21D");
}

/* ---------------------------------------------------------------- air */

static void test_air(void)
{
    sim_t *scd = sim_new(CMD);
    sim_cmd16(scd, 0x3f86, NULL, 0);
    sim_cmd16(scd, 0x21b1, NULL, 0);
    sim_cmd16(scd, 0x3682, (uint16_t[]){1, 2, 3}, 3);
    sim_cmd16(scd, 0xe4b8, (uint16_t[]){0x8006}, 1);
    sim_cmd16(scd, 0xec05, (uint16_t[]){800, 0x6666, 0x8000}, 3);
    expect_identified(scd, 0x62, "scd4x", SDRV_SURE, NULL);
    sval_t v = measure(scd, 0x62, "scd4x", SDRV_OK);
    VAL(v, SQ_CO2, 800.0f, 0.001f);
    VAL(v, SQ_TEMPERATURE, 25.0f, 0.01f);
    VAL(v, SQ_HUMIDITY, 50.0f, 0.01f);
    scd->cmds[3].resp[0] = 0x80, scd->cmds[3].resp[1] = 0x00; /* not ready */
    scd->cmds[3].resp[2] = sdrv_crc8(scd->cmds[3].resp, 2, 0xff);
    measure(scd, 0x62, "scd4x", SDRV_BUSY);

    sim_t *sgp = sim_new(CMD);
    sim_cmd16(sgp, 0x202f, (uint16_t[]){0x0022}, 1);
    sim_cmd16(sgp, 0x2003, NULL, 0);
    sim_cmd16(sgp, 0x2008, (uint16_t[]){450, 12}, 2);
    expect_identified(sgp, 0x58, "sgp30", SDRV_SURE, NULL);
    v = measure(sgp, 0x58, "sgp30", SDRV_OK);
    VAL(v, SQ_ECO2, 450.0f, 0.001f);
    VAL(v, SQ_TVOC, 12.0f, 0.001f);

    /* 0x53: ENS160 and LTR390 */
    sim_t *ens = sim_new(REG8);
    ens->reg8[0x00] = 0x60, ens->reg8[0x01] = 0x01;
    uint8_t d[6] = {0x02, 2, 0, 0, 0, 0};
    le16(d + 2, 120);
    le16(d + 4, 600);
    memcpy(&ens->reg8[0x20], d, 6);
    expect_identified(ens, 0x53, "ens160", SDRV_SURE, NULL);
    v = measure(ens, 0x53, "ens160", SDRV_OK);
    VAL(v, SQ_AQI, 2.0f, 0.001f);
    VAL(v, SQ_TVOC, 120.0f, 0.001f);
    VAL(v, SQ_ECO2, 600.0f, 0.001f);
    CHECK(ens->reg8[0x10] == 0x02, "ENS160 not in standard mode");

    sim_t *ltr = sim_new(REG8);
    ltr->reg8[0x06] = 0xb2;
    expect_identified(ltr, 0x53, "ltr390", SDRV_SURE, NULL);

    /* PMSA003I frame with checksum */
    sim_t *pm = sim_new(CMD);
    uint8_t f[32] = {0x42, 0x4d, 0, 28};
    be16(f + 10, 3), be16(f + 12, 7), be16(f + 14, 11);
    uint16_t sum = 0;
    for (int i = 0; i < 30; i++) {
        sum += f[i];
    }
    be16(f + 30, sum);
    memcpy(pm->idle, f, 32);
    pm->idle_len = 32;
    expect_identified(pm, 0x12, "pmsa003i", SDRV_SURE, NULL);
    v = measure(pm, 0x12, "pmsa003i", SDRV_OK);
    VAL(v, SQ_PM1, 3.0f, 0.001f);
    VAL(v, SQ_PM25, 7.0f, 0.001f);
    VAL(v, SQ_PM10, 11.0f, 0.001f);
    pm->idle[31] ^= 1;
    measure(pm, 0x12, "pmsa003i", SDRV_EDATA);
}

/* ---------------------------------------------------------------- light and temperature */

static void test_light(void)
{
    sim_t *bh = sim_new(CMD);
    bh->idle[0] = 0x01, bh->idle[1] = 0xe0, bh->idle_len = 2;
    sim_raw(bh, (uint8_t[]){0x01}, 1, NULL, 0);
    sim_raw(bh, (uint8_t[]){0x20}, 1, NULL, 0);
    expect_identified(bh, 0x23, "bh1750", SDRV_MAYBE, NULL);
    sval_t v = measure(bh, 0x23, "bh1750", SDRV_OK);
    VAL(v, SQ_ILLUMINANCE, 400.0f, 0.001f);

    sim_t *veml = sim_new(REG16LE);
    veml->reg16[0x07] = 0xc481;
    veml->reg16[0x04] = 1000;
    expect_identified(veml, 0x10, "veml7700", SDRV_SURE, NULL);
    v = measure(veml, 0x10, "veml7700", SDRV_OK);
    double x = 230.4;
    VAL(v, SQ_ILLUMINANCE, (float)(6.0135e-13 * x * x * x * x - 9.3924e-9 * x * x * x + 8.1488e-5 * x * x + 1.0023 * x), 0.01f);
    CHECK(veml->reg16[0x00] == 0x1800, "VEML7700 config 0x%04x", veml->reg16[0x00]);
    veml->reg16[0x07] = 0x0000; /* a part without the ID register: likely */
    expect_identified(veml, 0x10, "veml7700", SDRV_MAYBE, NULL);
    veml->reg16[0x0c] = 0x0026; /* a VEML6075 */
    sbus_t vb = bus(veml, 0x10);
    sdrv_cand_t vc[2];
    CHECK(sdrv_identify_all(&vb, vc, 2) == 0, "VEML6075 taken for a VEML7700");

    /* 0x29: TSL2591 and TSL2561 */
    sim_t *t91 = sim_new(REG8);
    t91->reg8[0xb2] = 0x50;
    le16(&t91->reg8[0xb4], 1000);
    le16(&t91->reg8[0xb6], 200);
    expect_identified(t91, 0x29, "tsl2591", SDRV_SURE, NULL);
    v = measure(t91, 0x29, "tsl2591", SDRV_OK);
    VAL(v, SQ_ILLUMINANCE, 800.0f * 0.8f / (200.0f * 25.0f / 408.0f), 0.01f);

    sim_t *t61 = sim_new(REG8);
    t61->reg8[0x8a] = 0x50;
    le16(&t61->reg8[0xac], 1000);
    le16(&t61->reg8[0xae], 200);
    expect_identified(t61, 0x29, "tsl2561", SDRV_SURE, NULL);
    v = measure(t61, 0x39, "tsl2561", SDRV_OK);
    VAL(v, SQ_ILLUMINANCE, (float)(0.0304 * 16000 - 0.062 * 16000 * pow(0.2, 1.4)), 0.01f);

    sim_t *mx = sim_new(REG8);
    mx->reg8[0x05] = 0xff;
    mx->reg8[0x03] = 0x5a, mx->reg8[0x04] = 0x07;
    v = measure(mx, 0x4a, "max44009", SDRV_OK);
    VAL(v, SQ_ILLUMINANCE, 240.48f, 0.01f);

    sim_t *tmp = sim_new(REG16BE);
    tmp->reg16[0x0f] = 0x1117;
    tmp->reg16[0x00] = 0x0c80;
    expect_identified(tmp, 0x48, "tmp117", SDRV_SURE, NULL);
    v = measure(tmp, 0x48, "tmp117", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.0f, 0.001f);
    tmp->reg16[0x00] = 0xff80;
    v = measure(tmp, 0x48, "tmp117", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, -1.0f, 0.001f);

    sim_t *mcp = sim_new(REG16BE);
    mcp->reg16[0x06] = 0x0054;
    mcp->reg16[0x07] = 0x0400;
    mcp->reg16[0x05] = 0x0194;
    expect_identified(mcp, 0x19, "mcp9808", SDRV_SURE, NULL);
    v = measure(mcp, 0x19, "mcp9808", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, 25.25f, 0.001f);
    mcp->reg16[0x05] = 0x1ff0;
    v = measure(mcp, 0x19, "mcp9808", SDRV_OK);
    VAL(v, SQ_TEMPERATURE, -1.0f, 0.001f);
}

/* ---------------------------------------------------------------- the list */

static void test_list(void)
{
    for (size_t i = 0; i < sdrv_count; i++) {
        const sdrv_t *d = sdrv_list[i];
        CHECK(sdrv_find(d->id) == d, "%s listed twice", d->id);
        CHECK(d->addrs[0] && d->quantities && d->identify && d->init && d->read, "%s incomplete", d->id);
        CHECK(!sdrv_has_addr(d, 0x18), "%s on the codec address", d->id);
        CHECK(strlen(d->label) < 16, "%s label too long", d->id);
    }
    CHECK(sdrv_hint(0x18) && strstr(sdrv_hint(0x18), "ES8311"), "codec hint");
    CHECK(sdrv_hint(0x3c) && strstr(sdrv_hint(0x3c), "OLED"), "display hint (narrow range wins)");
    CHECK(sdrv_hint(0x5d) && strstr(sdrv_hint(0x5d), "GT911"), "touch controller hint");
    CHECK(sdrv_hint(0x48) && strstr(sdrv_hint(0x48), "ADS1115"), "ADC hint");
    CHECK(sdrv_hint(0x11) == NULL, "no hint");
    /* two sure candidates: the user picks */
    sdrv_cand_t c[2] = {{.drv = &drv_sht3x, .score = SDRV_SURE}, {.drv = &drv_sht4x, .score = SDRV_SURE}};
    CHECK(sdrv_pick(c, 2) == NULL, "two sure candidates picked");
    c[1].score = SDRV_MAYBE;
    CHECK(sdrv_pick(c, 2) == &c[0], "sure before maybe");
    c[0].score = SDRV_MAYBE;
    CHECK(sdrv_pick(c, 2) == NULL, "two maybes picked");
    /* a bus without the chip */
    sim_t *none = sim_new(CMD);
    sbus_t b = bus(none, 0x44);
    sdrv_cand_t cc[4];
    CHECK(sdrv_identify_all(&b, cc, 4) == 0, "something identified on an empty address");
}

int main(void)
{
    test_bosch();
    test_humidity();
    test_air();
    test_light();
    test_list();
    printf("%zu drivers, %s\n", sdrv_count, failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
