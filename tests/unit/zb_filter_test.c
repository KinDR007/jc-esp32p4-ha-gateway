/* Host unit test of the ZBOSS telemetry frame filter (firmware/gateway/components/role_zigbee/zb_filter.c).
 *
 *   cc -std=c11 -Wall -Wextra -Werror -o /tmp/zb_filter_test tests/unit/zb_filter_test.c && /tmp/zb_filter_test
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../firmware/gateway/components/role_zigbee/zb_filter.c"

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

typedef struct { uint8_t d[8192]; size_t n; } buf_t;

static void put(buf_t *b, const void *p, size_t n) { memcpy(b->d + b->n, p, n); b->n += n; }

/* ZBOSS low-level frame as esp-coordinator writes it (protocol::send_data / the telemetry patch). */
static size_t make_frame(uint8_t *out, uint8_t type, const uint8_t *data, size_t n, int corrupt)
{
    uint16_t len = 5 + 2 + n;
    out[0] = 0xde; out[1] = 0xad; out[2] = len & 0xff; out[3] = len >> 8; out[4] = type; out[5] = 0x01;
    out[6] = crc8(out + 2, 4);
    uint16_t c = crc16(data, n) ^ (corrupt ? 0x55 : 0);
    out[7] = c & 0xff; out[8] = c >> 8;
    memcpy(out + 9, data, n);
    return 9 + n;
}

static char got[8][ZB_FILTER_JSON_MAX + 1];
static int got_n;
static void on_json(const char *json, void *ctx) { (void)ctx; if (got_n < 8) strcpy(got[got_n], json); got_n++; }

/* Runs the stream through the filter in chunks of the given sizes (cycled), flushing at the end. */
static void run(const buf_t *in, const size_t *chunks, size_t nchunks, buf_t *out, zb_filter_t *f)
{
    uint8_t tmp[8192 + ZB_FILTER_OUT_EXTRA];
    out->n = 0;
    for (size_t pos = 0, k = 0; pos < in->n; k++) {
        size_t c = chunks[k % nchunks];
        if (c > in->n - pos) c = in->n - pos;
        size_t n = zb_filter_run(f, in->d + pos, c, tmp);
        CHECK(n <= c + ZB_FILTER_OUT_EXTRA, "output %zu exceeds capacity for %zu input", n, c);
        put(out, tmp, n);
        pos += c;
    }
    put(out, tmp, zb_filter_flush(f, tmp));
}

int main(void)
{
    /* crc16 is CRC-16/KERMIT: check value from the catalogue */
    CHECK(crc16((const uint8_t *)"123456789", 9) == 0x2189, "crc16 check value %04x", crc16((const uint8_t *)"123456789", 9));

    uint8_t frame[600];
    const char *json1 = "{\"fw\":\"zigbee_ncp\",\"temp_c\":41.5,\"heap_free\":123456}";
    const char *json2 = "{\"fw\":\"zigbee_ncp\",\"temp_c\":42.0}";
    /* host-bound API frames whose payload contains DE, DE AD and a fake telemetry-looking header */
    uint8_t p1[] = {0x00, 0xde, 0xad, 0x10, 0x00, 0x7e, 0x00, 0x12, 0x34, 0xde};
    uint8_t p2[300];
    for (size_t i = 0; i < sizeof(p2); i++) p2[i] = (uint8_t)(i * 37 + 0xde);
    uint8_t p3[] = {0xde};

    buf_t in = {0}, want = {0};
    size_t n;
    n = make_frame(frame, 0x06, p1, sizeof(p1), 0); put(&in, frame, n); put(&want, frame, n);
    n = make_frame(frame, ZB_FILTER_TYPE, (const uint8_t *)json1, strlen(json1), 0); put(&in, frame, n);
    n = make_frame(frame, 0x06, p2, sizeof(p2), 0); put(&in, frame, n); put(&want, frame, n);
    uint8_t garbage[] = {0xde, 0x01, 0xde, 0xde, 0xad, 0x00};  /* lone signatures, invalid header */
    put(&in, garbage, sizeof(garbage)); put(&want, garbage, sizeof(garbage));
    n = make_frame(frame, ZB_FILTER_TYPE, (const uint8_t *)json2, strlen(json2), 0); put(&in, frame, n);
    /* type 0x7e but a wrong header crc: not ours, must pass */
    n = make_frame(frame, ZB_FILTER_TYPE, (const uint8_t *)"x", 1, 0); frame[6] ^= 0xff; put(&in, frame, n); put(&want, frame, n);
    n = make_frame(frame, 0x06, p3, sizeof(p3), 0); put(&in, frame, n); put(&want, frame, n);  /* ends with 0xDE */

    /* every chunking pattern: single bytes, small primes, whole stream */
    size_t patterns[][3] = {{1, 1, 1}, {2, 3, 5}, {7, 7, 7}, {13, 1, 64}, {1024, 1024, 1024}, {9, 300, 4}};
    for (size_t p = 0; p < sizeof(patterns) / sizeof(patterns[0]); p++) {
        zb_filter_t f;
        buf_t out;
        got_n = 0;
        zb_filter_init(&f, on_json, NULL);
        run(&in, patterns[p], 3, &out, &f);
        CHECK(out.n == want.n && memcmp(out.d, want.d, want.n) == 0, "pattern %zu: stream differs (%zu vs %zu bytes)", p, out.n, want.n);
        CHECK(got_n == 2 && !strcmp(got[0], json1) && !strcmp(got[1], json2), "pattern %zu: telemetry %d", p, got_n);
        CHECK(f.frames == 2 && f.bad_frames == 0, "pattern %zu: counters %u/%u", p, f.frames, f.bad_frames);
    }

    /* random chunkings */
    srand(1);
    for (int r = 0; r < 2000; r++) {
        size_t ch[3] = {1 + rand() % 40, 1 + rand() % 400, 1 + rand() % 3};
        zb_filter_t f;
        buf_t out;
        got_n = 0;
        zb_filter_init(&f, on_json, NULL);
        run(&in, ch, 3, &out, &f);
        CHECK(out.n == want.n && memcmp(out.d, want.d, want.n) == 0 && got_n == 2, "random %d differs", r);
    }

    /* a telemetry frame with a bad crc16 is removed and counted, the stream continues */
    {
        buf_t s = {0}, w = {0}, out;
        n = make_frame(frame, ZB_FILTER_TYPE, (const uint8_t *)json1, strlen(json1), 1); put(&s, frame, n);
        n = make_frame(frame, 0x06, p1, sizeof(p1), 0); put(&s, frame, n); put(&w, frame, n);
        zb_filter_t f;
        size_t one = 5;
        got_n = 0;
        zb_filter_init(&f, on_json, NULL);
        run(&s, &one, 1, &out, &f);
        CHECK(out.n == w.n && !memcmp(out.d, w.d, w.n) && got_n == 0 && f.bad_frames == 1, "bad crc16 handling");
    }

    /* a cut-off telemetry frame: the idle flush drops it, the next frame passes complete */
    {
        zb_filter_t f;
        uint8_t tmp[1024];
        got_n = 0;
        zb_filter_init(&f, on_json, NULL);
        n = make_frame(frame, ZB_FILTER_TYPE, (const uint8_t *)json1, strlen(json1), 0);
        size_t o = zb_filter_run(&f, frame, n / 2, tmp);
        CHECK(o == 0 && zb_filter_pending(&f), "cut frame held");
        o = zb_filter_flush(&f, tmp);
        CHECK(o == 0 && !zb_filter_pending(&f) && f.bad_frames == 1, "cut frame dropped on idle");
        n = make_frame(frame, 0x06, p1, sizeof(p1), 0);
        o = zb_filter_run(&f, frame, n, tmp);
        o += zb_filter_flush(&f, tmp + o);
        CHECK(o == n && !memcmp(tmp, frame, n), "frame after the cut passes");
    }

    /* a held 0xDE at the end of a chunk comes out with the idle flush, unchanged */
    {
        zb_filter_t f;
        uint8_t tmp[16], in2[] = {0x41, 0xde};
        zb_filter_init(&f, NULL, NULL);
        size_t o = zb_filter_run(&f, in2, 2, tmp);
        CHECK(o == 1 && tmp[0] == 0x41 && zb_filter_pending(&f), "trailing DE held");
        o = zb_filter_flush(&f, tmp);
        CHECK(o == 1 && tmp[0] == 0xde, "trailing DE released");
    }

    printf(failures ? "zb_filter_test: %d FAILED\n" : "zb_filter_test: all passed\n", failures);
    return failures != 0;
}
