#pragma once

/*
 * Modbus application protocol (PDU), RTU and TCP framing, value coding. Plain C without ESP-IDF, so it
 * builds on the host too (tests/unit/modbus_test.c). Spec: Modbus Application Protocol V1.1b3,
 * Modbus over Serial Line V1.02 (CRC-16, 3.5 character gap).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MB_PDU_MAX 253
#define MB_RTU_MAX (1 + MB_PDU_MAX + 2)
#define MB_TCP_MAX (7 + MB_PDU_MAX)
#define MB_READ_REGS_MAX 125
#define MB_READ_BITS_MAX 2000

typedef enum {
    MB_FC_READ_COILS = 0x01,
    MB_FC_READ_DISCRETE = 0x02,
    MB_FC_READ_HOLDING = 0x03,
    MB_FC_READ_INPUT = 0x04,
    MB_FC_WRITE_COIL = 0x05,
    MB_FC_WRITE_REG = 0x06,
    MB_FC_WRITE_COILS = 0x0f,
    MB_FC_WRITE_REGS = 0x10,
} mb_fc_t;

/* Register spaces of a template entity */
typedef enum { MB_SPACE_COIL, MB_SPACE_DISCRETE, MB_SPACE_INPUT, MB_SPACE_HOLDING } mb_space_t;

/* Results: 0 ok, > 0 a Modbus exception code (1 illegal function, 2 illegal address, 3 illegal value,
 * 4 device failure, 6 busy, 0x0b gateway target failed), < 0 our errors */
#define MB_OK 0
#define MB_ERR_TIMEOUT (-1)
#define MB_ERR_CRC (-2)
#define MB_ERR_FRAME (-3)  /* wrong length, address, function or transaction */
#define MB_ERR_IO (-4)     /* transport (socket, UART) */
#define MB_ERR_ARG (-5)

const char *mb_err_name(int err);

uint16_t mb_crc16(const uint8_t *data, size_t len);

/* A request PDU: function and its data. */
typedef struct {
    uint8_t fc;
    uint16_t addr, count;
    const uint16_t *regs; /* write registers: count values */
    const uint8_t *bits;  /* write coils: packed, LSB first */
    uint16_t value;       /* write single coil (0xff00/0x0000) or register */
} mb_req_t;

/* Encodes the PDU of req into pdu; returns its length, 0 for invalid arguments. */
size_t mb_pdu_encode(const mb_req_t *req, uint8_t *pdu, size_t len);
/* The PDU length a correct answer to req has (exception answers are 2). */
size_t mb_pdu_answer_len(const mb_req_t *req);
/* Checks an answer PDU against the request: MB_OK, an exception code (> 0) or MB_ERR_FRAME. Reads give
 * their data at *data (registers big endian, coils packed) with *data_len bytes. */
int mb_pdu_check(const mb_req_t *req, const uint8_t *pdu, size_t len, const uint8_t **data, size_t *data_len);

/* Server side: decodes a request PDU of the standard functions (1-6, 15, 16) into req; written registers go
 * to regs (MB_PDU_MAX / 2 values), written coils point into pdu. MB_OK, or the exception to answer:
 * 1 for another function, 3 for an invalid count or length. */
int mb_pdu_parse(const uint8_t *pdu, size_t len, mb_req_t *req, uint16_t *regs);
/* The answer PDU to a successful req: reads carry data (data_len bytes as a device sends them), writes
 * echo the address and value or count. Returns its length, 0 when it does not fit. */
size_t mb_pdu_answer(const mb_req_t *req, const uint8_t *data, size_t data_len, uint8_t *pdu, size_t len);
/* Copies the part [addr, addr + count) of a read block starting at block_addr into out from register or
 * bit pos on: registers (2 bytes each) or packed bits (coils, discrete inputs; the bits of out not
 * written stay). */
void mb_extract(bool bits, const uint8_t *block, uint16_t block_addr, uint16_t addr, uint16_t count, uint8_t *out, int pos);

/* RTU: address + PDU + CRC (low byte first). */
size_t mb_rtu_frame(uint8_t slave, const uint8_t *pdu, size_t pdu_len, uint8_t *out, size_t out_len);
/* Checks a received RTU frame: MB_OK with the PDU at *pdu, MB_ERR_CRC or MB_ERR_FRAME. */
int mb_rtu_unframe(uint8_t slave, const uint8_t *frame, size_t len, const uint8_t **pdu, size_t *pdu_len);
/* How many bytes a complete RTU answer has, given the first bytes received (0: not known yet). */
size_t mb_rtu_expected(const uint8_t *frame, size_t len);
/* Silent interval of 3.5 characters at baud (11 bits per character), at least 1750 µs above 19200 Bd. */
uint32_t mb_rtu_gap_us(uint32_t baud);

/* GoodWe Wi-Fi modules (UDP 8899): the request is an RTU frame, the answer 0xAA 0x55 + an RTU frame. */
#define MB_AA55_HEAD 2
int mb_aa55_unframe(uint8_t slave, const uint8_t *frame, size_t len, const uint8_t **pdu, size_t *pdu_len);

/* TCP: MBAP header (transaction, protocol 0, length, unit) + PDU. */
size_t mb_tcp_frame(uint16_t transaction, uint8_t unit, const uint8_t *pdu, size_t pdu_len, uint8_t *out, size_t out_len);
/* Length of the whole ADU announced by a 7-byte MBAP header, 0 when the header is invalid. */
size_t mb_tcp_expected(const uint8_t *mbap);
int mb_tcp_unframe(uint16_t transaction, uint8_t unit, const uint8_t *adu, size_t len, const uint8_t **pdu, size_t *pdu_len);

/* ---- values ---- */

typedef enum {
    MB_T_BOOL, /* a coil or discrete input, or one bit of a register (bit >= 0) */
    MB_T_U16,
    MB_T_S16,
    MB_T_U32,
    MB_T_S32,
    MB_T_U64,
    MB_T_S64,
    MB_T_F32,
    MB_T_F64,
    MB_T_STR, /* ASCII, two characters per register, high byte first */
} mb_type_t;

/* Order of the bytes of a multi-register value, as the letters of a 32-bit value A (most significant)
 * to D appear on the wire: ABCD big endian (Modbus default), CDAB word swapped, BADC byte swapped,
 * DCBA little endian. 64-bit values follow the same pattern per 16-bit word and byte. */
typedef enum { MB_ORDER_ABCD, MB_ORDER_CDAB, MB_ORDER_BADC, MB_ORDER_DCBA } mb_order_t;

/* Registers a value of the type occupies (str: its own count). */
int mb_type_regs(mb_type_t type);
bool mb_type_from_name(const char *name, mb_type_t *type);
const char *mb_type_name(mb_type_t type);
bool mb_order_from_name(const char *name, mb_order_t *order);
const char *mb_order_name(mb_order_t order);

/* Decodes a number from registers as read (big endian bytes per register, regs[0] first on the wire).
 * bit >= 0 takes that bit of the first register (bool). Returns false for NaN/Inf. */
bool mb_decode(const uint8_t *regs, mb_type_t type, mb_order_t order, int bit, double *out);
/* Encodes value into nregs registers (host order uint16 values ready for mb_req_t.regs); false when the
 * value does not fit the type. */
bool mb_encode(double value, mb_type_t type, mb_order_t order, uint16_t *regs, int *nregs);

#ifdef __cplusplus
}
#endif
