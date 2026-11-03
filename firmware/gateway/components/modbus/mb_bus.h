#pragma once

#include "mb_proto.h"

/* Transports of the modbus task: RS485 (UART3 on J4) and TCP connections. Not thread safe: one task. */

typedef struct {
    uint32_t baud;
    char parity; /* 'N', 'E', 'O' */
    uint8_t stop;
} mb_serial_t;

/* (Re)configures the RS485 UART; false without it (driver error). */
bool mb_rtu_open(const mb_serial_t *cfg);
bool mb_rtu_ready(void);

/* One request on the RS485 bus: waits gap_ms (at least 3.5 characters) after the last frame on the bus,
 * sends, and reads the answer within timeout_ms. MB_OK with the read data in data (up to *len bytes,
 * *len set), a Modbus exception (> 0) or an error (< 0). */
int mb_rtu_request(uint8_t slave, const mb_req_t *req, uint16_t timeout_ms, uint16_t gap_ms, uint8_t *data, size_t *len);

/* A TCP connection to a Modbus TCP device (or gateway), opened on demand and kept. */
typedef struct {
    int sock;
    uint16_t transaction;
    int64_t retry_us; /* after a failed connect: not before */
} mb_tcp_t;

void mb_tcp_init(mb_tcp_t *c);
void mb_tcp_close(mb_tcp_t *c);
int mb_tcp_request(mb_tcp_t *c, const char *host, uint16_t port, uint8_t unit, const mb_req_t *req, uint16_t timeout_ms,
                   uint8_t *data, size_t *len);

/* counters for the UI */
typedef struct {
    uint32_t requests, timeouts, crc_errors, exceptions;
} mb_bus_stats_t;
mb_bus_stats_t mb_rtu_stats(void);
