#include "mb_proto.h"

#include <math.h>
#include <string.h>

const char *mb_err_name(int err)
{
    switch (err) {
    case MB_OK: return "ok";
    case MB_ERR_TIMEOUT: return "no answer";
    case MB_ERR_CRC: return "CRC error";
    case MB_ERR_FRAME: return "invalid answer";
    case MB_ERR_IO: return "connection error";
    case MB_ERR_ARG: return "invalid request";
    case 1: return "exception 1: illegal function";
    case 2: return "exception 2: illegal data address";
    case 3: return "exception 3: illegal data value";
    case 4: return "exception 4: device failure";
    case 5: return "exception 5: acknowledge";
    case 6: return "exception 6: device busy";
    case 0x0a: return "exception 10: gateway path unavailable";
    case 0x0b: return "exception 11: gateway target did not respond";
    default: return err > 0 ? "exception" : "error";
    }
}

uint16_t mb_crc16(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xffff;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = crc & 1 ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = v >> 8;
    p[1] = v & 0xff;
}

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static bool standard(uint8_t fc)
{
    return (fc >= MB_FC_READ_COILS && fc <= MB_FC_WRITE_REG) || fc == MB_FC_WRITE_COILS || fc == MB_FC_WRITE_REGS;
}

size_t mb_pdu_encode(const mb_req_t *req, uint8_t *pdu, size_t len)
{
    if (len < 6) {
        return 0;
    }
    pdu[0] = req->fc;
    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
        if (req->count < 1 || req->count > MB_READ_BITS_MAX) {
            return 0;
        }
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->count);
        return 5;
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        if (req->count < 1 || req->count > MB_READ_REGS_MAX) {
            return 0;
        }
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->count);
        return 5;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->value);
        return 5;
    case MB_FC_WRITE_REGS: {
        if (!req->regs || req->count < 1 || req->count > 123 || len < 6 + 2u * req->count) {
            return 0;
        }
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->count);
        pdu[5] = (uint8_t)(2 * req->count);
        for (int i = 0; i < req->count; i++) {
            put16(pdu + 6 + 2 * i, req->regs[i]);
        }
        return 6 + 2u * req->count;
    }
    case MB_FC_WRITE_COILS: {
        size_t bytes = (req->count + 7u) / 8;
        if (!req->bits || req->count < 1 || req->count > 1968 || len < 6 + bytes) {
            return 0;
        }
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->count);
        pdu[5] = (uint8_t)bytes;
        memcpy(pdu + 6, req->bits, bytes);
        return 6 + bytes;
    }
    default:
        /* a device specific function without data (PZEM: 0x42 resets the energy counter) */
        return standard(req->fc) || req->fc >= 0x80 ? 0 : 1;
    }
}

int mb_pdu_parse(const uint8_t *pdu, size_t len, mb_req_t *req, uint16_t *regs)
{
    memset(req, 0, sizeof(*req));
    if (len < 1) {
        return 3;
    }
    req->fc = pdu[0];
    if (!standard(req->fc)) {
        return 1;
    }
    if (len < 5) {
        return 3;
    }
    req->addr = get16(pdu + 1);
    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT: {
        req->count = get16(pdu + 3);
        bool bits = req->fc <= MB_FC_READ_DISCRETE;
        return len == 5 && req->count >= 1 && req->count <= (bits ? MB_READ_BITS_MAX : MB_READ_REGS_MAX) ? MB_OK : 3;
    }
    case MB_FC_WRITE_COIL:
        req->value = get16(pdu + 3);
        return len == 5 && (req->value == 0xff00 || req->value == 0) ? MB_OK : 3;
    case MB_FC_WRITE_REG:
        req->value = get16(pdu + 3);
        return len == 5 ? MB_OK : 3;
    case MB_FC_WRITE_COILS:
        req->count = get16(pdu + 3);
        if (len < 6 || req->count < 1 || req->count > 1968 || pdu[5] != (req->count + 7) / 8 || len != 6u + pdu[5]) {
            return 3;
        }
        req->bits = pdu + 6;
        return MB_OK;
    default: /* MB_FC_WRITE_REGS */
        req->count = get16(pdu + 3);
        if (len < 6 || req->count < 1 || req->count > 123 || pdu[5] != 2 * req->count || len != 6u + pdu[5]) {
            return 3;
        }
        for (int i = 0; i < req->count; i++) {
            regs[i] = get16(pdu + 6 + 2 * i);
        }
        req->regs = regs;
        return MB_OK;
    }
}

size_t mb_pdu_answer(const mb_req_t *req, const uint8_t *data, size_t data_len, uint8_t *pdu, size_t len)
{
    size_t n = mb_pdu_answer_len(req);
    if (n > len || n < 2) {
        return 0;
    }
    pdu[0] = req->fc;
    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        if (data_len != n - 2) {
            return 0;
        }
        pdu[1] = (uint8_t)data_len;
        memcpy(pdu + 2, data, data_len);
        return n;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->value);
        return n;
    default: /* write coils, write registers */
        put16(pdu + 1, req->addr);
        put16(pdu + 3, req->count);
        return n;
    }
}

void mb_extract(bool bits, const uint8_t *block, uint16_t block_addr, uint16_t addr, uint16_t count, uint8_t *out, int pos)
{
    int off = addr - block_addr;
    if (!bits) {
        memcpy(out + 2 * pos, block + 2 * off, 2u * count);
        return;
    }
    for (int i = 0; i < count; i++) {
        int b = off + i, o = pos + i;
        if ((block[b / 8] >> (b % 8)) & 1) {
            out[o / 8] |= (uint8_t)(1 << (o % 8));
        } else {
            out[o / 8] &= (uint8_t)~(1 << (o % 8));
        }
    }
}

size_t mb_pdu_answer_len(const mb_req_t *req)
{
    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
        return 2 + (req->count + 7u) / 8;
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        return 2 + 2u * req->count;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        return 5;
    default:
        return 1;
    }
}

int mb_pdu_check(const mb_req_t *req, const uint8_t *pdu, size_t len, const uint8_t **data, size_t *data_len)
{
    if (len == 2 && pdu[0] == (req->fc | 0x80)) {
        return pdu[1] ? pdu[1] : MB_ERR_FRAME;
    }
    if (len < 1 || pdu[0] != req->fc || len != mb_pdu_answer_len(req)) {
        return MB_ERR_FRAME;
    }
    switch (req->fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        if (pdu[1] != len - 2) {
            return MB_ERR_FRAME;
        }
        if (data) {
            *data = pdu + 2;
            *data_len = pdu[1];
        }
        return MB_OK;
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
        return get16(pdu + 1) == req->addr && get16(pdu + 3) == req->value ? MB_OK : MB_ERR_FRAME;
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        return get16(pdu + 1) == req->addr && get16(pdu + 3) == req->count ? MB_OK : MB_ERR_FRAME;
    default:
        return MB_OK;
    }
}

size_t mb_rtu_frame(uint8_t slave, const uint8_t *pdu, size_t pdu_len, uint8_t *out, size_t out_len)
{
    if (pdu_len < 1 || out_len < pdu_len + 3) {
        return 0;
    }
    out[0] = slave;
    memcpy(out + 1, pdu, pdu_len);
    uint16_t crc = mb_crc16(out, pdu_len + 1);
    out[pdu_len + 1] = crc & 0xff;
    out[pdu_len + 2] = crc >> 8;
    return pdu_len + 3;
}

int mb_rtu_unframe(uint8_t slave, const uint8_t *frame, size_t len, const uint8_t **pdu, size_t *pdu_len)
{
    if (len < 4) {
        return MB_ERR_FRAME;
    }
    uint16_t crc = mb_crc16(frame, len - 2);
    if ((crc & 0xff) != frame[len - 2] || (crc >> 8) != frame[len - 1]) {
        return MB_ERR_CRC;
    }
    if (frame[0] != slave) {
        return MB_ERR_FRAME;
    }
    *pdu = frame + 1;
    *pdu_len = len - 3;
    return MB_OK;
}

int mb_aa55_unframe(uint8_t slave, const uint8_t *frame, size_t len, const uint8_t **pdu, size_t *pdu_len)
{
    if (len < MB_AA55_HEAD || frame[0] != 0xaa || frame[1] != 0x55) {
        return MB_ERR_FRAME;
    }
    return mb_rtu_unframe(slave, frame + MB_AA55_HEAD, len - MB_AA55_HEAD, pdu, pdu_len);
}

size_t mb_rtu_expected(const uint8_t *frame, size_t len)
{
    if (len < 2) {
        return 0;
    }
    uint8_t fc = frame[1];
    if (fc & 0x80) {
        return 5;
    }
    switch (fc) {
    case MB_FC_READ_COILS:
    case MB_FC_READ_DISCRETE:
    case MB_FC_READ_HOLDING:
    case MB_FC_READ_INPUT:
        return len < 3 ? 0 : 5u + frame[2];
    case MB_FC_WRITE_COIL:
    case MB_FC_WRITE_REG:
    case MB_FC_WRITE_COILS:
    case MB_FC_WRITE_REGS:
        return 8;
    default:
        return 4;
    }
}

uint32_t mb_rtu_gap_us(uint32_t baud)
{
    if (!baud || baud > 19200) {
        return 1750;
    }
    return (uint32_t)(3.5 * 11 * 1000000.0 / baud + 0.5);
}

size_t mb_tcp_frame(uint16_t transaction, uint8_t unit, const uint8_t *pdu, size_t pdu_len, uint8_t *out, size_t out_len)
{
    if (pdu_len < 1 || out_len < pdu_len + 7) {
        return 0;
    }
    put16(out, transaction);
    put16(out + 2, 0);
    put16(out + 4, (uint16_t)(pdu_len + 1));
    out[6] = unit;
    memcpy(out + 7, pdu, pdu_len);
    return pdu_len + 7;
}

size_t mb_tcp_expected(const uint8_t *mbap)
{
    uint16_t n = get16(mbap + 4);
    return get16(mbap + 2) == 0 && n >= 2 && n <= MB_PDU_MAX + 1 ? 6u + n : 0;
}

int mb_tcp_unframe(uint16_t transaction, uint8_t unit, const uint8_t *adu, size_t len, const uint8_t **pdu, size_t *pdu_len)
{
    if (len < 8 || mb_tcp_expected(adu) != len || get16(adu) != transaction || adu[6] != unit) {
        return MB_ERR_FRAME;
    }
    *pdu = adu + 7;
    *pdu_len = len - 7;
    return MB_OK;
}

/* ---------------------------------------------------------------- values */

static const char *const s_types[] = {"bool", "u16", "s16", "u32", "s32", "u64", "s64", "f32", "f64", "str"};
static const char *const s_orders[] = {"ABCD", "CDAB", "BADC", "DCBA"};

int mb_type_regs(mb_type_t type)
{
    static const int regs[] = {1, 1, 1, 2, 2, 4, 4, 2, 4, 0};
    return regs[type];
}

bool mb_type_from_name(const char *name, mb_type_t *type)
{
    for (size_t i = 0; i < sizeof(s_types) / sizeof(s_types[0]); i++) {
        if (strcmp(name, s_types[i]) == 0) {
            *type = (mb_type_t)i;
            return true;
        }
    }
    return false;
}

const char *mb_type_name(mb_type_t type)
{
    return s_types[type];
}

bool mb_order_from_name(const char *name, mb_order_t *order)
{
    for (size_t i = 0; i < sizeof(s_orders) / sizeof(s_orders[0]); i++) {
        if (strcmp(name, s_orders[i]) == 0) {
            *order = (mb_order_t)i;
            return true;
        }
    }
    return false;
}

const char *mb_order_name(mb_order_t order)
{
    return s_orders[order];
}

/* Wire bytes of an n-register value into big endian order (most significant byte first). */
static void to_be(const uint8_t *in, int nregs, mb_order_t order, uint8_t *out)
{
    bool swap_words = order == MB_ORDER_CDAB || order == MB_ORDER_DCBA;
    bool swap_bytes = order == MB_ORDER_BADC || order == MB_ORDER_DCBA;
    for (int w = 0; w < nregs; w++) {
        const uint8_t *src = in + 2 * (swap_words ? nregs - 1 - w : w);
        out[2 * w] = swap_bytes ? src[1] : src[0];
        out[2 * w + 1] = swap_bytes ? src[0] : src[1];
    }
}

static void from_be(const uint8_t *be, int nregs, mb_order_t order, uint8_t *out)
{
    /* every order is its own inverse */
    to_be(be, nregs, order, out);
}

static uint64_t be_uint(const uint8_t *b, int bytes)
{
    uint64_t v = 0;
    for (int i = 0; i < bytes; i++) {
        v = v << 8 | b[i];
    }
    return v;
}

bool mb_decode(const uint8_t *regs, mb_type_t type, mb_order_t order, int bit, double *out)
{
    uint8_t b[8] = {0};
    if (bit >= 0) {
        *out = (get16(regs) >> (bit & 15)) & 1;
        return true;
    }
    int n = mb_type_regs(type);
    if (n == 0) {
        return false;
    }
    /* 16-bit values: only the byte order applies */
    to_be(regs, n, n == 1 && order == MB_ORDER_CDAB ? MB_ORDER_ABCD : n == 1 && order == MB_ORDER_DCBA ? MB_ORDER_BADC : order, b);
    uint64_t u = be_uint(b, 2 * n);
    double v;
    switch (type) {
    case MB_T_BOOL: v = u != 0; break;
    case MB_T_U16: v = (uint16_t)u; break;
    case MB_T_S16: v = (int16_t)u; break;
    case MB_T_U32: v = (uint32_t)u; break;
    case MB_T_S32: v = (int32_t)u; break;
    case MB_T_U64: v = (double)u; break;
    case MB_T_S64: v = (double)(int64_t)u; break;
    case MB_T_F32: {
        uint32_t w = (uint32_t)u;
        float f;
        memcpy(&f, &w, 4);
        v = f;
        break;
    }
    case MB_T_F64: memcpy(&v, &u, 8); break;
    default: return false;
    }
    if (isnan(v) || isinf(v)) {
        return false;
    }
    *out = v;
    return true;
}

bool mb_encode(double value, mb_type_t type, mb_order_t order, uint16_t *regs, int *nregs)
{
    int n = mb_type_regs(type);
    uint64_t u;
    double r = round(value);
    switch (type) {
    case MB_T_BOOL: u = value != 0; break;
    case MB_T_U16: if (r < 0 || r > 65535) return false; u = (uint64_t)r; break;
    case MB_T_S16: if (r < -32768 || r > 32767) return false; u = (uint16_t)(int16_t)r; break;
    case MB_T_U32: if (r < 0 || r > 4294967295.0) return false; u = (uint64_t)r; break;
    case MB_T_S32: if (r < -2147483648.0 || r > 2147483647.0) return false; u = (uint32_t)(int32_t)r; break;
    case MB_T_U64: if (r < 0 || r >= 18446744073709551616.0) return false; u = (uint64_t)r; break;
    case MB_T_S64: if (r < -9223372036854775808.0 || r >= 9223372036854775808.0) return false; u = (uint64_t)(int64_t)r; break;
    case MB_T_F32: {
        float f = (float)value;
        uint32_t w;
        memcpy(&w, &f, 4);
        u = w;
        break;
    }
    case MB_T_F64: memcpy(&u, &value, 8); break;
    default: return false;
    }
    uint8_t be[8] = {0}, wire[8];
    for (int i = 0; i < 2 * n; i++) {
        be[i] = (uint8_t)(u >> (8 * (2 * n - 1 - i)));
    }
    from_be(be, n, n == 1 && order == MB_ORDER_CDAB ? MB_ORDER_ABCD : n == 1 && order == MB_ORDER_DCBA ? MB_ORDER_BADC : order, wire);
    for (int i = 0; i < n; i++) {
        regs[i] = get16(wire + 2 * i);
    }
    *nregs = n;
    return true;
}
