#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * Removes the C6 telemetry frames from the ZBOSS NCP byte stream before it goes to the host.
 * A telemetry frame is a ZBOSS low-level frame of packet type ZB_FILTER_TYPE (see the
 * esp-coordinator patch): DE AD, packet_len (LE), type, flags, header crc8, crc16, JSON.
 * Everything else passes unchanged and in order. A possible frame start (0xDE...) is held back
 * until its 7-byte header is known; zb_filter_flush() releases it when the line goes idle.
 * No ESP-IDF dependencies: unit tested on the host (tests/unit/zb_filter_test.c).
 */
#define ZB_FILTER_TYPE 0x7e
#define ZB_FILTER_HEADER 7
#define ZB_FILTER_JSON_MAX 256
/* Output buffer for zb_filter_run(): input length plus this many held bytes. */
#define ZB_FILTER_OUT_EXTRA ZB_FILTER_HEADER

typedef void (*zb_filter_cb_t)(const char *json, void *ctx);

typedef struct {
    uint8_t hold[ZB_FILTER_HEADER];
    size_t hold_len;
    uint8_t frame[ZB_FILTER_HEADER + 2 + ZB_FILTER_JSON_MAX + 1];
    size_t frame_len, frame_total; /* frame_total > 0 while a telemetry frame is being collected */
    uint32_t frames;               /* telemetry frames removed */
    uint32_t bad_frames;           /* telemetry frames with a bad crc16, dropped */
    zb_filter_cb_t cb;
    void *ctx;
} zb_filter_t;

void zb_filter_init(zb_filter_t *f, zb_filter_cb_t cb, void *ctx);
/* Filters len bytes of in into out (capacity len + ZB_FILTER_OUT_EXTRA), returns the bytes to forward. */
size_t zb_filter_run(zb_filter_t *f, const uint8_t *in, size_t len, uint8_t *out);
/* The line went idle: forwards held bytes and drops an unfinished telemetry frame. Returns bytes in out. */
size_t zb_filter_flush(zb_filter_t *f, uint8_t *out);
/* True while bytes are held back (the caller should flush after a short idle time). */
static inline int zb_filter_pending(const zb_filter_t *f)
{
    return f->hold_len || f->frame_total;
}
