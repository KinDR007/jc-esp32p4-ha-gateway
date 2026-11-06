#pragma once
/* Modbus TCP server (mb_server.c) and what it needs of the device side (modbus.c), docs/modbus.md. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mb_proto.h"

/* A request passed through to a device; the modbus task runs it and sets done (the server looks every 5 ms). */
typedef struct {
    char dev[8];
    mb_req_t req;
    uint16_t regs[MB_PDU_MAX / 2];
    uint8_t bits[MB_PDU_MAX];
    uint8_t data[MB_PDU_MAX];
    size_t len;
    int result;
    bool cache; /* keep the answer of a read for the next clients */
    volatile bool done;
} mb_srv_job_t;

/* ---- modbus.c ---- */
/* Answers a read from the registers the gateway polled within the device's poll interval and from answers
 * to clients within ttl_ms: MB_OK with data, the exception the same read got (2), or MB_ERR_ARG when the
 * cache does not hold all of it (always with ttl_ms 0). */
int modbus_srv_cache(const char *dev, const mb_req_t *req, uint16_t ttl_ms, uint8_t *data, size_t *len);
/* Queues a request for the device: MB_OK, or the exception to answer at once (0x0a no such device or
 * off, 0x0b offline). */
int modbus_srv_submit(mb_srv_job_t *job);
/* Whether a device with this id is configured; name gets its name. */
bool modbus_srv_device(const char *dev, char *name, size_t len);

/* ---- mb_server.c ---- */
void mb_server_init(void);
