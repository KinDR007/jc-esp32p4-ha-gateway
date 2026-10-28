#include "zb_filter.h"

#include <string.h>

/* Header crc8 of esp-coordinator (utils::crc8), same table. */
static const uint8_t crc8_table[256] = {
    0xea, 0xd4, 0x96, 0xa8, 0x12, 0x2c, 0x6e, 0x50, 0x7f, 0x41, 0x03, 0x3d, 0x87, 0xb9, 0xfb, 0xc5, 0xa5, 0x9b, 0xd9, 0xe7, 0x5d, 0x63, 0x21, 0x1f,
    0x30, 0x0e, 0x4c, 0x72, 0xc8, 0xf6, 0xb4, 0x8a, 0x74, 0x4a, 0x08, 0x36, 0x8c, 0xb2, 0xf0, 0xce, 0xe1, 0xdf, 0x9d, 0xa3, 0x19, 0x27, 0x65, 0x5b,
    0x3b, 0x05, 0x47, 0x79, 0xc3, 0xfd, 0xbf, 0x81, 0xae, 0x90, 0xd2, 0xec, 0x56, 0x68, 0x2a, 0x14, 0xb3, 0x8d, 0xcf, 0xf1, 0x4b, 0x75, 0x37, 0x09,
    0x26, 0x18, 0x5a, 0x64, 0xde, 0xe0, 0xa2, 0x9c, 0xfc, 0xc2, 0x80, 0xbe, 0x04, 0x3a, 0x78, 0x46, 0x69, 0x57, 0x15, 0x2b, 0x91, 0xaf, 0xed, 0xd3,
    0x2d, 0x13, 0x51, 0x6f, 0xd5, 0xeb, 0xa9, 0x97, 0xb8, 0x86, 0xc4, 0xfa, 0x40, 0x7e, 0x3c, 0x02, 0x62, 0x5c, 0x1e, 0x20, 0x9a, 0xa4, 0xe6, 0xd8,
    0xf7, 0xc9, 0x8b, 0xb5, 0x0f, 0x31, 0x73, 0x4d, 0x58, 0x66, 0x24, 0x1a, 0xa0, 0x9e, 0xdc, 0xe2, 0xcd, 0xf3, 0xb1, 0x8f, 0x35, 0x0b, 0x49, 0x77,
    0x17, 0x29, 0x6b, 0x55, 0xef, 0xd1, 0x93, 0xad, 0x82, 0xbc, 0xfe, 0xc0, 0x7a, 0x44, 0x06, 0x38, 0xc6, 0xf8, 0xba, 0x84, 0x3e, 0x00, 0x42, 0x7c,
    0x53, 0x6d, 0x2f, 0x11, 0xab, 0x95, 0xd7, 0xe9, 0x89, 0xb7, 0xf5, 0xcb, 0x71, 0x4f, 0x0d, 0x33, 0x1c, 0x22, 0x60, 0x5e, 0xe4, 0xda, 0x98, 0xa6,
    0x01, 0x3f, 0x7d, 0x43, 0xf9, 0xc7, 0x85, 0xbb, 0x94, 0xaa, 0xe8, 0xd6, 0x6c, 0x52, 0x10, 0x2e, 0x4e, 0x70, 0x32, 0x0c, 0xb6, 0x88, 0xca, 0xf4,
    0xdb, 0xe5, 0xa7, 0x99, 0x23, 0x1d, 0x5f, 0x61, 0x9f, 0xa1, 0xe3, 0xdd, 0x67, 0x59, 0x1b, 0x25, 0x0a, 0x34, 0x76, 0x48, 0xf2, 0xcc, 0x8e, 0xb0,
    0xd0, 0xee, 0xac, 0x92, 0x28, 0x16, 0x54, 0x6a, 0x45, 0x7b, 0x39, 0x07, 0xbd, 0x83, 0xc1, 0xff,
};

static uint8_t crc8(const uint8_t *p, size_t n)
{
    uint8_t crc = 0;
    while (n--) {
        crc = crc8_table[crc ^ *p++];
    }
    return crc;
}

/* CRC-16/KERMIT, the data crc of ZBOSS NCP frames (utils::crc16). */
static uint16_t crc16(const uint8_t *p, size_t n)
{
    uint16_t crc = 0;
    while (n--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : crc >> 1;
        }
    }
    return crc;
}

void zb_filter_init(zb_filter_t *f, zb_filter_cb_t cb, void *ctx)
{
    memset(f, 0, sizeof(*f));
    f->cb = cb;
    f->ctx = ctx;
}

static void frame_done(zb_filter_t *f)
{
    size_t json_len = f->frame_total - ZB_FILTER_HEADER - 2;
    uint8_t *json = f->frame + ZB_FILTER_HEADER + 2;
    uint16_t want = f->frame[ZB_FILTER_HEADER] | f->frame[ZB_FILTER_HEADER + 1] << 8;
    if (crc16(json, json_len) == want) {
        f->frames++;
        json[json_len] = '\0';
        if (f->cb) {
            f->cb((const char *)json, f->ctx);
        }
    } else {
        f->bad_frames++;
    }
    f->frame_total = f->frame_len = 0;
}

static size_t feed(zb_filter_t *f, uint8_t b, uint8_t *out);

/* The held bytes are not a telemetry header: the first goes out, the rest is scanned again. */
static size_t release_hold(zb_filter_t *f, uint8_t *out, int valid_header)
{
    size_t n = 0;
    if (valid_header) {
        /* a real ZBOSS frame header: no other frame can start inside it */
        memcpy(out, f->hold, f->hold_len);
        n = f->hold_len;
        f->hold_len = 0;
        return n;
    }
    uint8_t rest[ZB_FILTER_HEADER];
    size_t rest_len = f->hold_len - 1;
    memcpy(rest, f->hold + 1, rest_len);
    out[n++] = f->hold[0];
    f->hold_len = 0;
    for (size_t i = 0; i < rest_len; i++) {
        n += feed(f, rest[i], out + n);
    }
    return n;
}

static size_t feed(zb_filter_t *f, uint8_t b, uint8_t *out)
{
    if (f->frame_total) {
        f->frame[f->frame_len++] = b;
        if (f->frame_len == f->frame_total) {
            frame_done(f);
        }
        return 0;
    }
    if (!f->hold_len) {
        if (b == 0xde) {
            f->hold[f->hold_len++] = b;
            return 0;
        }
        out[0] = b;
        return 1;
    }
    f->hold[f->hold_len++] = b;
    if (f->hold_len == 2 && b != 0xad) {
        return release_hold(f, out, 0);
    }
    if (f->hold_len < ZB_FILTER_HEADER) {
        return 0;
    }
    int valid = crc8(f->hold + 2, 4) == f->hold[6];
    size_t total = (size_t)(f->hold[2] | f->hold[3] << 8) + 2;
    if (valid && f->hold[4] == ZB_FILTER_TYPE && total > ZB_FILTER_HEADER + 2 &&
        total <= ZB_FILTER_HEADER + 2 + ZB_FILTER_JSON_MAX) {
        memcpy(f->frame, f->hold, ZB_FILTER_HEADER);
        f->frame_len = ZB_FILTER_HEADER;
        f->frame_total = total;
        f->hold_len = 0;
        return 0;
    }
    return release_hold(f, out, valid);
}

size_t zb_filter_run(zb_filter_t *f, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t n = 0;
    for (size_t i = 0; i < len; i++) {
        n += feed(f, in[i], out + n);
    }
    return n;
}

size_t zb_filter_flush(zb_filter_t *f, uint8_t *out)
{
    size_t n = f->hold_len;
    memcpy(out, f->hold, n);
    f->hold_len = 0;
    if (f->frame_total) {
        /* the C6 writes a telemetry frame in one go: a gap inside one means it was cut off */
        f->bad_frames++;
        f->frame_total = f->frame_len = 0;
    }
    return n;
}
