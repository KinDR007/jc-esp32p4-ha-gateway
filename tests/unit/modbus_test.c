/* Modbus protocol layer (CRC, RTU and TCP framing, answer checks, value coding in every word order) and
 * device templates (validation, read planning, emulation); every built-in template given as an argument must parse.
 *   cc -std=c11 -Wall -Wextra -Werror -Wno-unused-parameter -I firmware/gateway/components/modbus -I <cJSON dir> \
 *      -o /tmp/modbus_test tests/unit/modbus_test.c firmware/gateway/components/modbus/mb_proto.c \
 *      firmware/gateway/components/modbus/mb_template.c <cJSON dir>/cJSON.c -lm \
 *      && /tmp/modbus_test firmware/gateway/components/modbus/templates/<every>.json
 * scripts/ci_checks.sh takes cJSON from ESP-IDF or downloads it. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mb_proto.h"
#include "mb_template.h"

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

static void test_crc_rtu(void)
{
    /* Modbus over serial line V1.02, and the common "01 03 00 00 00 0A C5 CD" example */
    uint8_t pdu[8], frame[16];
    mb_req_t r = {.fc = MB_FC_READ_HOLDING, .addr = 0, .count = 10};
    size_t n = mb_pdu_encode(&r, pdu, sizeof(pdu));
    size_t f = mb_rtu_frame(1, pdu, n, frame, sizeof(frame));
    CHECK(f == 8 && frame[6] == 0xc5 && frame[7] == 0xcd, "read holding frame CRC %02x %02x", frame[6], frame[7]);
    /* Eastron SDM: read input 0x0000, 2 registers -> 01 04 00 00 00 02 71 CB */
    r = (mb_req_t){.fc = MB_FC_READ_INPUT, .addr = 0, .count = 2};
    n = mb_pdu_encode(&r, pdu, sizeof(pdu));
    f = mb_rtu_frame(1, pdu, n, frame, sizeof(frame));
    CHECK(f == 8 && frame[6] == 0x71 && frame[7] == 0xcb, "read input frame CRC %02x %02x", frame[6], frame[7]);

    /* its answer (Eastron SDM120 protocol 2.1: 230.2 V = 43 66 33 34) */
    uint8_t ans[9] = {0x01, 0x04, 0x04, 0x43, 0x66, 0x33, 0x34};
    uint16_t crc = mb_crc16(ans, 7);
    ans[7] = crc & 0xff, ans[8] = crc >> 8;
    CHECK(mb_rtu_expected(ans, 2) == 0 && mb_rtu_expected(ans, 3) == 9, "expected length of a read answer");
    const uint8_t *p, *data;
    size_t plen, dlen;
    CHECK(mb_rtu_unframe(1, ans, 9, &p, &plen) == MB_OK, "unframe");
    CHECK(mb_pdu_check(&r, p, plen, &data, &dlen) == MB_OK && dlen == 4, "answer checked");
    double v;
    CHECK(mb_decode(data, MB_T_F32, MB_ORDER_ABCD, -1, &v) && fabs(v - 230.2) < 1e-4, "230.2 V decoded: %f", v);
    ans[8] ^= 1;
    CHECK(mb_rtu_unframe(1, ans, 9, &p, &plen) == MB_ERR_CRC, "bad CRC detected");
    ans[8] ^= 1;
    CHECK(mb_rtu_unframe(2, ans, 9, &p, &plen) == MB_ERR_FRAME, "answer of another slave refused");

    /* exception answer: 01 84 02 + CRC */
    uint8_t ex[5] = {0x01, 0x84, 0x02};
    crc = mb_crc16(ex, 3);
    ex[3] = crc & 0xff, ex[4] = crc >> 8;
    CHECK(mb_rtu_expected(ex, 2) == 5, "exception length");
    CHECK(mb_rtu_unframe(1, ex, 5, &p, &plen) == MB_OK && mb_pdu_check(&r, p, plen, NULL, NULL) == 2, "exception 2 reported");
    CHECK(strstr(mb_err_name(2), "illegal data address") != NULL, "exception name");

    /* wrong byte count for the request */
    uint8_t short_pdu[4] = {0x04, 0x02, 0x00, 0x01};
    CHECK(mb_pdu_check(&r, short_pdu, 4, NULL, NULL) == MB_ERR_FRAME, "short answer refused");
    CHECK(mb_rtu_gap_us(9600) > 4000 && mb_rtu_gap_us(9600) < 4020 && mb_rtu_gap_us(115200) == 1750, "3.5 character gap");
}

static void test_writes(void)
{
    uint8_t pdu[32];
    uint16_t regs[2];
    int n;
    /* SDM: address register written as float32 60.0 = 42 70 00 00 (Eastron SDM120 protocol 2.3) */
    CHECK(mb_encode(60.0, MB_T_F32, MB_ORDER_ABCD, regs, &n) && n == 2 && regs[0] == 0x4270 && regs[1] == 0, "60.0 as float32");
    mb_req_t w = {.fc = MB_FC_WRITE_REGS, .addr = 0x0014, .count = 2, .regs = regs};
    size_t len = mb_pdu_encode(&w, pdu, sizeof(pdu));
    uint8_t want[] = {0x10, 0x00, 0x14, 0x00, 0x02, 0x04, 0x42, 0x70, 0x00, 0x00};
    CHECK(len == sizeof(want) && memcmp(pdu, want, len) == 0, "write multiple registers PDU");
    uint8_t echo[5] = {0x10, 0x00, 0x14, 0x00, 0x02};
    CHECK(mb_pdu_check(&w, echo, 5, NULL, NULL) == MB_OK, "write multiple echo");
    echo[4] = 1;
    CHECK(mb_pdu_check(&w, echo, 5, NULL, NULL) == MB_ERR_FRAME, "wrong echo refused");

    mb_req_t c = {.fc = MB_FC_WRITE_COIL, .addr = 7, .value = 0xff00};
    len = mb_pdu_encode(&c, pdu, sizeof(pdu));
    CHECK(len == 5 && pdu[3] == 0xff && pdu[4] == 0x00, "write coil on");
    CHECK(mb_pdu_check(&c, pdu, 5, NULL, NULL) == MB_OK, "write coil echo");

    /* PZEM energy reset: function 0x42 without data, answer the function alone */
    mb_req_t z = {.fc = 0x42};
    len = mb_pdu_encode(&z, pdu, sizeof(pdu));
    CHECK(len == 1 && pdu[0] == 0x42 && mb_pdu_answer_len(&z) == 1, "device specific function");
    mb_req_t bad = {.fc = MB_FC_READ_INPUT, .count = 126};
    CHECK(mb_pdu_encode(&bad, pdu, sizeof(pdu)) == 0, "126 registers refused");
}

static void test_tcp(void)
{
    uint8_t pdu[8], adu[16];
    mb_req_t r = {.fc = MB_FC_READ_HOLDING, .addr = 0x2006, .count = 2};
    size_t n = mb_pdu_encode(&r, pdu, sizeof(pdu));
    size_t len = mb_tcp_frame(0x1234, 3, pdu, n, adu, sizeof(adu));
    uint8_t want[] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x06, 0x03, 0x03, 0x20, 0x06, 0x00, 0x02};
    CHECK(len == sizeof(want) && memcmp(adu, want, len) == 0, "MBAP + PDU");
    uint8_t ans[] = {0x12, 0x34, 0x00, 0x00, 0x00, 0x07, 0x03, 0x03, 0x04, 0x43, 0x66, 0x33, 0x34};
    const uint8_t *p, *data;
    size_t plen, dlen;
    CHECK(mb_tcp_expected(ans) == sizeof(ans), "length from MBAP");
    CHECK(mb_tcp_unframe(0x1234, 3, ans, sizeof(ans), &p, &plen) == MB_OK && mb_pdu_check(&r, p, plen, &data, &dlen) == MB_OK,
          "TCP answer");
    CHECK(mb_tcp_unframe(0x1235, 3, ans, sizeof(ans), &p, &plen) == MB_ERR_FRAME, "other transaction refused");
    ans[2] = 1;
    CHECK(mb_tcp_expected(ans) == 0, "protocol id not 0 refused");
}

static void test_aa55(void)
{
    /* GoodWe DT/XS device info over UDP 8899: request 7F 03 75 31 00 28 04 09 (goodwe library), answer
     * AA 55 7F 03 50 + 80 bytes + CRC, as a GW3000-XS sent it */
    uint8_t pdu[8], frame[16];
    mb_req_t r = {.fc = MB_FC_READ_HOLDING, .addr = 30001, .count = 40};
    size_t f = mb_rtu_frame(0x7f, pdu, mb_pdu_encode(&r, pdu, sizeof(pdu)), frame, sizeof(frame));
    CHECK(f == 8 && frame[6] == 0x04 && frame[7] == 0x09, "GoodWe request CRC %02x %02x", frame[6], frame[7]);
    uint8_t ans[2 + 3 + 80 + 2] = {0xaa, 0x55, 0x7f, 0x03, 80, 0x00, 0x02, 0xff, 0xff, 0xff, 0xff};
    memcpy(ans + 11, "53000SSX211W0000GW3000-XS", 25);
    uint16_t crc = mb_crc16(ans + 2, sizeof(ans) - 4);
    ans[sizeof(ans) - 2] = crc & 0xff, ans[sizeof(ans) - 1] = crc >> 8;
    CHECK(mb_rtu_expected(ans + MB_AA55_HEAD, 3) == sizeof(ans) - MB_AA55_HEAD, "expected length after the AA 55 head");
    const uint8_t *p, *data = ans;
    size_t plen, dlen;
    CHECK(mb_aa55_unframe(0x7f, ans, sizeof(ans), &p, &plen) == MB_OK && mb_pdu_check(&r, p, plen, &data, &dlen) == MB_OK && dlen == 80,
          "AA 55 answer unframed");
    CHECK(memcmp(data + 6, "53000SSX211W0000", 16) == 0, "serial number at 30004");
    CHECK(mb_aa55_unframe(0xf7, ans, sizeof(ans), &p, &plen) == MB_ERR_FRAME, "answer of another unit refused");
    CHECK(mb_aa55_unframe(0x7f, ans + MB_AA55_HEAD, sizeof(ans) - MB_AA55_HEAD, &p, &plen) == MB_ERR_FRAME, "plain RTU without the head refused");
    ans[20] ^= 1;
    CHECK(mb_aa55_unframe(0x7f, ans, sizeof(ans), &p, &plen) == MB_ERR_CRC, "bad CRC detected");
}

static void test_server(void)
{
    /* requests a client sends, decoded and answered as the gateway's Modbus TCP server does */
    uint16_t regs[MB_PDU_MAX / 2];
    mb_req_t r;
    const uint8_t rd[] = {0x03, 0x00, 0x10, 0x00, 0x02};
    CHECK(mb_pdu_parse(rd, sizeof(rd), &r, regs) == MB_OK && r.fc == 3 && r.addr == 0x10 && r.count == 2, "read holding parsed");
    const uint8_t data[] = {0x12, 0x34, 0x56, 0x78};
    uint8_t ans[MB_PDU_MAX];
    size_t n = mb_pdu_answer(&r, data, sizeof(data), ans, sizeof(ans));
    CHECK(n == 6 && ans[0] == 3 && ans[1] == 4 && !memcmp(ans + 2, data, 4), "read answer");
    CHECK(mb_pdu_answer(&r, data, 2, ans, sizeof(ans)) == 0, "read answer with the wrong data length refused");
    const uint8_t big[] = {0x04, 0x00, 0x00, 0x00, 126};
    CHECK(mb_pdu_parse(big, sizeof(big), &r, regs) == 3, "126 registers: exception 3");
    const uint8_t bits[] = {0x01, 0x00, 0x00, 0x07, 0xd1};
    CHECK(mb_pdu_parse(bits, sizeof(bits), &r, regs) == 3, "2001 coils: exception 3");
    const uint8_t fn[] = {0x2b, 0x0e, 0x01, 0x00};
    CHECK(mb_pdu_parse(fn, sizeof(fn), &r, regs) == 1, "function 0x2b: exception 1");
    const uint8_t extra[] = {0x03, 0x00, 0x10, 0x00, 0x02, 0x00};
    CHECK(mb_pdu_parse(extra, sizeof(extra), &r, regs) == 3, "a byte too many: exception 3");

    const uint8_t w16[] = {0x10, 0x00, 0x20, 0x00, 0x02, 0x04, 0x00, 0x0a, 0x01, 0x02};
    CHECK(mb_pdu_parse(w16, sizeof(w16), &r, regs) == MB_OK && r.count == 2 && r.regs[0] == 10 && r.regs[1] == 0x0102, "write registers parsed");
    uint8_t enc[MB_PDU_MAX];
    CHECK(mb_pdu_encode(&r, enc, sizeof(enc)) == sizeof(w16) && !memcmp(enc, w16, sizeof(w16)), "write registers encode back to the same PDU");
    n = mb_pdu_answer(&r, NULL, 0, ans, sizeof(ans));
    CHECK(n == 5 && ans[0] == 0x10 && ans[2] == 0x20 && ans[4] == 2, "write registers answer: address and count");
    const uint8_t w16bad[] = {0x10, 0x00, 0x20, 0x00, 0x02, 0x03, 0x00, 0x0a, 0x01};
    CHECK(mb_pdu_parse(w16bad, sizeof(w16bad), &r, regs) == 3, "byte count not 2 * count: exception 3");
    const uint8_t w5[] = {0x05, 0x00, 0x03, 0xff, 0x00};
    CHECK(mb_pdu_parse(w5, sizeof(w5), &r, regs) == MB_OK && r.value == 0xff00, "write coil parsed");
    n = mb_pdu_answer(&r, NULL, 0, ans, sizeof(ans));
    CHECK(n == 5 && !memcmp(ans, w5, 5), "write coil answer echoes the request");
    const uint8_t w5bad[] = {0x05, 0x00, 0x03, 0x12, 0x34};
    CHECK(mb_pdu_parse(w5bad, sizeof(w5bad), &r, regs) == 3, "write coil value 0x1234: exception 3");
    const uint8_t w15[] = {0x0f, 0x00, 0x00, 0x00, 0x0a, 0x02, 0xcd, 0x01};
    CHECK(mb_pdu_parse(w15, sizeof(w15), &r, regs) == MB_OK && r.count == 10 && r.bits[0] == 0xcd && r.bits[1] == 0x01, "write coils parsed");

    /* the cache answers a part of a block read earlier */
    const uint8_t block[] = {0x00, 0x01, 0x00, 0x02, 0x00, 0x03, 0x00, 0x04};
    uint8_t out[16] = {0};
    mb_extract(false, block, 100, 101, 2, out, 0);
    CHECK(out[1] == 2 && out[3] == 3, "registers 101-102 of a block at 100");
    mb_extract(false, block, 100, 100, 1, out, 2);
    CHECK(out[1] == 2 && out[3] == 3 && out[5] == 1, "register 100 appended as the third");
    const uint8_t coils[] = {0xb5, 0x03}; /* bits 0..9: 1 0 1 0 1 1 0 1 1 1 */
    memset(out, 0xff, sizeof(out));
    mb_extract(true, coils, 10, 13, 6, out, 0);
    CHECK((out[0] & 0x3f) == 0x36, "coils 13-18 (0 1 1 0 1 1) of a block at 10: %02x", out[0]);
    mb_extract(true, coils, 10, 10, 10, out, 0);
    CHECK(out[0] == 0xb5 && (out[1] & 3) == 0x03, "the whole bit block");
    memset(out, 0, sizeof(out));
    mb_extract(true, coils, 10, 18, 2, out, 7);
    CHECK(out[0] == 0x80 && out[1] == 0x01, "coils 18-19 as bits 7-8 of the answer: %02x %02x", out[0], out[1]);
}

static void test_values(void)
{
    /* 0x12345678 in the four orders as they come over the wire */
    const uint8_t abcd[] = {0x12, 0x34, 0x56, 0x78}, cdab[] = {0x56, 0x78, 0x12, 0x34}, badc[] = {0x34, 0x12, 0x78, 0x56},
                  dcba[] = {0x78, 0x56, 0x34, 0x12};
    const uint8_t *wire[] = {abcd, cdab, badc, dcba};
    for (int o = 0; o < 4; o++) {
        double v;
        CHECK(mb_decode(wire[o], MB_T_U32, (mb_order_t)o, -1, &v) && v == 0x12345678, "u32 %s: %f", mb_order_name(o), v);
        uint16_t regs[4];
        int n;
        CHECK(mb_encode(0x12345678, MB_T_U32, (mb_order_t)o, regs, &n) && n == 2 && regs[0] == (wire[o][0] << 8 | wire[o][1]) &&
                  regs[1] == (wire[o][2] << 8 | wire[o][3]),
              "u32 %s encoded", mb_order_name(o));
    }
    double v;
    /* PZEM: current u32 low word first (CDAB), 0x0001 0x0000 -> wait: low word 0x86A0, high 0x0001 = 100000 mA */
    const uint8_t pz[] = {0x86, 0xa0, 0x00, 0x01};
    CHECK(mb_decode(pz, MB_T_U32, MB_ORDER_CDAB, -1, &v) && v == 100000, "low word first: %f", v);
    const uint8_t s16[] = {0xff, 0xfe};
    CHECK(mb_decode(s16, MB_T_S16, MB_ORDER_ABCD, -1, &v) && v == -2, "s16 -2");
    CHECK(mb_decode(s16, MB_T_S16, MB_ORDER_CDAB, -1, &v) && v == -2, "word order ignored for one register");
    const uint8_t sw[] = {0x34, 0x12};
    CHECK(mb_decode(sw, MB_T_U16, MB_ORDER_BADC, -1, &v) && v == 0x1234, "byte swapped u16");
    const uint8_t bits[] = {0x00, 0x24};
    CHECK(mb_decode(bits, MB_T_U16, MB_ORDER_ABCD, 2, &v) && v == 1 && mb_decode(bits, MB_T_U16, MB_ORDER_ABCD, 3, &v) && v == 0,
          "bit of a register");
    /* ABB u64 import energy 0x0000000000BC614E = 12345678 (x0.01 kWh) */
    const uint8_t u64[] = {0, 0, 0, 0, 0x00, 0xbc, 0x61, 0x4e};
    CHECK(mb_decode(u64, MB_T_U64, MB_ORDER_ABCD, -1, &v) && v == 12345678, "u64");
    const uint8_t s32n[] = {0xff, 0xff, 0xff, 0x9c};
    CHECK(mb_decode(s32n, MB_T_S32, MB_ORDER_ABCD, -1, &v) && v == -100, "s32 -100");
    const uint8_t nan[] = {0x7f, 0xc0, 0x00, 0x00};
    CHECK(!mb_decode(nan, MB_T_F32, MB_ORDER_ABCD, -1, &v), "NaN refused");
    uint16_t regs[4];
    int n;
    CHECK(mb_encode(-1.5, MB_T_F64, MB_ORDER_ABCD, regs, &n) && n == 4 && regs[0] == 0xbff8, "f64 -1.5");
    CHECK(!mb_encode(70000, MB_T_U16, MB_ORDER_ABCD, regs, &n), "70000 does not fit u16");
    CHECK(!mb_encode(-1, MB_T_U32, MB_ORDER_ABCD, regs, &n), "-1 does not fit u32");
    mb_type_t t;
    mb_order_t o;
    CHECK(mb_type_from_name("f32", &t) && t == MB_T_F32 && !mb_type_from_name("float", &t), "type names");
    CHECK(mb_order_from_name("CDAB", &o) && o == MB_ORDER_CDAB && !mb_order_from_name("cdab", &o), "order names");
}

static bool parse_text(const char *text, mb_template_t *t, char *err, size_t len)
{
    cJSON *j = cJSON_Parse(text);
    bool ok = mb_template_parse(j, t, err, len);
    cJSON_Delete(j);
    return ok;
}

static void expect_refused(const char *text, const char *why)
{
    mb_template_t t;
    char err[160] = "";
    bool ok = parse_text(text, &t, err, sizeof(err));
    CHECK(!ok && strstr(err, why), "refused (%s): got \"%s\"", why, ok ? "accepted" : err);
    if (ok) {
        mb_template_free(&t);
    }
}

#define T_HEAD "{\"id\":\"x\",\"manufacturer\":\"M\",\"model\":\"X\",\"entities\":["

static void test_templates(void)
{
    mb_template_t t;
    char err[160];
    const char *ok =
        T_HEAD "{\"key\":\"v\",\"name\":\"Voltage\",\"reg\":\"input\",\"addr\":\"0x0000\",\"type\":\"f32\",\"unit\":\"V\"},"
               "{\"key\":\"i\",\"name\":\"Current\",\"reg\":\"input\",\"addr\":6,\"type\":\"u32\",\"order\":\"CDAB\",\"scale\":0.001},"
               "{\"key\":\"far\",\"name\":\"Far\",\"reg\":\"input\",\"addr\":\"0x0100\",\"type\":\"u16\"},"
               "{\"key\":\"relay\",\"name\":\"Relay\",\"reg\":\"coil\",\"addr\":3,\"write\":{\"kind\":\"switch\"}},"
               "{\"key\":\"alarm\",\"name\":\"Alarm\",\"reg\":\"holding\",\"addr\":16,\"bit\":3},"
               "{\"key\":\"mode\",\"name\":\"Mode\",\"reg\":\"holding\",\"addr\":17,\"map\":{\"0\":\"off\",\"1\":\"auto\"},\"write\":{\"kind\":\"select\"}},"
               "{\"key\":\"set\",\"name\":\"Setpoint\",\"reg\":\"holding\",\"addr\":18,\"type\":\"s16\",\"scale\":0.1,\"write\":{\"kind\":\"number\",\"min\":5,\"max\":30,\"step\":0.5}},"
               "{\"key\":\"reset\",\"name\":\"Reset\",\"reg\":\"none\",\"write\":{\"kind\":\"button\",\"fc\":66}}]}";
    CHECK(parse_text(ok, &t, err, sizeof(err)), "valid template: %s", err);
    CHECK(t.nent == 8 && t.baud == 9600 && t.parity == 'N' && t.slave == 1 && t.max_regs == 40, "defaults");
    const mb_entity_t *e = mb_template_entity(&t, "i");
    CHECK(e && e->order == MB_ORDER_CDAB && e->scale == 0.001 && e->precision == 0, "entity fields");
    CHECK(mb_entity_value(e, 12345) == 12, "value rounded to precision 0");
    e = mb_template_entity(&t, "mode");
    CHECK(e && mb_entity_label(e, 1) && strcmp(mb_entity_label(e, 1), "auto") == 0 && !mb_entity_label(e, 5), "map labels");
    e = mb_template_entity(&t, "alarm");
    CHECK(e && e->type == MB_T_BOOL && e->bit == 3, "bit entity is bool");

    /* planning: v (0-1) and i (6-7) join with max_gap 4, far (0x100) alone; coil and holding apart; reset never read */
    t.max_gap = 4;
    uint16_t idx[8];
    bool single[8] = {false};
    for (int i = 0; i < 8; i++) {
        idx[i] = (uint16_t)(7 - i);
    }
    mb_block_t b[8];
    int nb = mb_plan(&t, idx, 8, single, b, 8);
    CHECK(nb == 4, "4 blocks, got %d", nb);
    CHECK(b[0].space == MB_SPACE_COIL && b[0].addr == 3 && b[0].count == 1, "coil block");
    CHECK(b[1].space == MB_SPACE_INPUT && b[1].addr == 0 && b[1].count == 8 && b[1].n == 2, "joined input block %u+%u", b[1].addr, b[1].count);
    CHECK(b[2].addr == 0x100 && b[2].n == 1, "far register alone");
    CHECK(b[3].space == MB_SPACE_HOLDING && b[3].addr == 16 && b[3].count == 3 && b[3].n == 3, "holding block");
    t.max_gap = 0;
    for (int i = 0; i < 8; i++) {
        idx[i] = (uint16_t)i;
    }
    nb = mb_plan(&t, idx, 8, single, b, 8);
    CHECK(nb == 5, "no gap allowed: 5 blocks, got %d", nb);
    single[(int)(mb_template_entity(&t, "set") - t.ent)] = true;
    for (int i = 0; i < 8; i++) {
        idx[i] = (uint16_t)i;
    }
    nb = mb_plan(&t, idx, 8, single, b, 8);
    CHECK(nb == 6, "an entity marked single splits its block: %d", nb);
    t.max_gap = 4;
    t.max_regs = 4;
    for (int i = 0; i < 8; i++) {
        idx[i] = (uint16_t)i;
    }
    single[(int)(mb_template_entity(&t, "set") - t.ent)] = false;
    nb = mb_plan(&t, idx, 8, single, b, 8);
    CHECK(nb == 5 && b[1].count == 2 && b[2].addr == 6, "max_regs 4 keeps v and i apart (%d blocks)", nb);

    /* back to JSON and again */
    cJSON *j = mb_template_json(&t);
    mb_template_t t2;
    CHECK(mb_template_parse(j, &t2, err, sizeof(err)) && t2.nent == t.nent, "round trip: %s", err);
    cJSON_Delete(j);
    mb_template_free(&t2);
    mb_template_free(&t);

    expect_refused("{\"id\":\"x\"}", "manufacturer");
    expect_refused("{\"id\":\"Bad Id\",\"manufacturer\":\"M\",\"model\":\"X\",\"entities\":[]}", "id:");
    expect_refused(T_HEAD "]}", "entities: 1-");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"input\",\"addr\":70000}]}", "addr 0-65535");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"input\",\"addr\":1,\"type\":\"float\"}]}", "type bool");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"input\",\"addr\":1,\"write\":{\"kind\":\"number\",\"min\":0,\"max\":1}}]}",
                   "cannot be written");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"holding\",\"addr\":1,\"write\":{\"kind\":\"select\"}}]}", "needs map");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"holding\",\"addr\":1,\"write\":{\"kind\":\"number\"}}]}", "needs write.min");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"holding\",\"addr\":1},{\"key\":\"v\",\"name\":\"W\",\"reg\":\"holding\",\"addr\":2}]}",
                   "used twice");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"input\",\"addr\":1,\"type\":\"f32\",\"bit\":2}]}", "bit only");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"coil\",\"addr\":1,\"type\":\"u16\"}]}", "coils and discrete");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"none\"}]}", "reg none only");
    expect_refused(T_HEAD "{\"key\":\"v\",\"name\":\"V\",\"reg\":\"holding\",\"addr\":1,\"write\":{\"kind\":\"button\",\"fc\":66}}]}",
                   "device function");
    expect_refused("{\"id\":\"x\",\"manufacturer\":\"M\",\"model\":\"X\",\"serial\":{\"parity\":\"X\"},\"entities\":[{\"key\":\"v\",\"name\":\"V\",\"reg\":\"input\",\"addr\":1}]}",
                   "parity");
}

/* every built-in template parses, and its registers are what the source says about the family */
static uint16_t reg_at(const uint8_t *img, int r)
{
    return (uint16_t)(img[2 * r] << 8 | img[2 * r + 1]);
}

/* a three phase meter with Eastron's keys shown as an ABB-like one */
static void test_emulation(void)
{
    mb_template_t src, dst;
    char err[160] = "";
    CHECK(parse_text("{\"id\":\"src\",\"manufacturer\":\"M\",\"model\":\"S\",\"entities\":["
                     "{\"key\":\"voltage_l1\",\"name\":\"V\",\"reg\":\"input\",\"addr\":0,\"type\":\"f32\",\"unit\":\"V\"},"
                     "{\"key\":\"total_power\",\"q\":\"power\",\"name\":\"P\",\"reg\":\"input\",\"addr\":2,\"type\":\"f32\",\"unit\":\"kW\"},"
                     "{\"key\":\"reactive_power_l1\",\"name\":\"Q1\",\"reg\":\"input\",\"addr\":4,\"type\":\"f32\",\"unit\":\"var\"},"
                     "{\"key\":\"reactive_power_l2\",\"name\":\"Q2\",\"reg\":\"input\",\"addr\":6,\"type\":\"f32\",\"unit\":\"var\"},"
                     "{\"key\":\"reactive_power_l3\",\"name\":\"Q3\",\"reg\":\"input\",\"addr\":8,\"type\":\"f32\",\"unit\":\"var\"},"
                     "{\"key\":\"import_energy\",\"name\":\"E\",\"reg\":\"input\",\"addr\":10,\"type\":\"f32\",\"unit\":\"kWh\"},"
                     "{\"key\":\"serial_number\",\"q\":\"serial\",\"name\":\"S\",\"reg\":\"holding\",\"addr\":0,\"type\":\"u32\"},"
                     "{\"key\":\"firmware\",\"name\":\"F\",\"reg\":\"holding\",\"addr\":2,\"type\":\"u16\"}]}",
                     &src, err, sizeof(err)),
          "source template: %s", err);
    CHECK(parse_text("{\"id\":\"dst\",\"manufacturer\":\"M\",\"model\":\"D\",\"fill\":65535,\"entities\":["
                     "{\"key\":\"voltage_l1\",\"name\":\"V\",\"reg\":\"holding\",\"addr\":0,\"type\":\"u32\",\"scale\":0.1,\"unit\":\"V\",\"nan\":4294967295},"
                     "{\"key\":\"power\",\"name\":\"P\",\"reg\":\"holding\",\"addr\":4,\"type\":\"s32\",\"scale\":0.01,\"unit\":\"W\",\"nan\":2147483647},"
                     "{\"key\":\"reactive_power\",\"name\":\"Q\",\"reg\":\"holding\",\"addr\":6,\"type\":\"s32\",\"scale\":0.01,\"unit\":\"var\"},"
                     "{\"key\":\"frequency\",\"name\":\"f\",\"reg\":\"holding\",\"addr\":8,\"type\":\"u16\",\"scale\":0.01,\"nan\":65535},"
                     "{\"key\":\"import_energy\",\"name\":\"E\",\"reg\":\"holding\",\"addr\":10,\"type\":\"u64\",\"scale\":0.01,\"unit\":\"kWh\","
                     "\"nan\":18446744073709551615},"
                     "{\"key\":\"export_energy\",\"name\":\"X\",\"reg\":\"holding\",\"addr\":14,\"type\":\"u64\",\"scale\":0.01,\"unit\":\"kWh\","
                     "\"nan\":18446744073709551615},"
                     "{\"key\":\"type\",\"name\":\"T\",\"reg\":\"holding\",\"addr\":20,\"type\":\"str\",\"count\":3,\"emulate\":\"B23 \"},"
                     "{\"key\":\"serial\",\"name\":\"S\",\"reg\":\"holding\",\"addr\":24,\"type\":\"u32\"},"
                     "{\"key\":\"firmware\",\"name\":\"F\",\"reg\":\"holding\",\"addr\":26,\"type\":\"str\",\"count\":1}]}",
                     &dst, err, sizeof(err)),
          "target template: %s", err);
    mb_map_t map[9];
    char how[64];
    int n = mb_map_resolve(&dst, &src, map);
    CHECK(n == 6, "resolved %d of 9", n);
    mb_map_describe(&map[1], &src, how, sizeof(how));
    CHECK(map[1].kind == MB_MAP_ONE && strcmp(how, "total_power x1000") == 0, "power from total_power in kW: %s", how);
    mb_map_describe(&map[2], &src, how, sizeof(how));
    CHECK(map[2].kind == MB_MAP_SUM3 && strcmp(how, "reactive_power_l1+reactive_power_l2+reactive_power_l3") == 0, "reactive power summed: %s", how);
    CHECK(map[3].kind == MB_MAP_NONE && map[5].kind == MB_MAP_NONE, "frequency and export energy not in the source");
    CHECK(map[6].kind == MB_MAP_CONST, "type fixed");
    CHECK(map[7].kind == MB_MAP_ONE && map[7].src[0] == 6, "serial from serial_number by q");
    CHECK(map[8].kind == MB_MAP_NONE, "a number does not become a text");

    double sval[8] = {230.4, 1.5, 100, 110, 120, 1234.56, 12345678, 7};
    uint8_t sok[8] = {1, 1, 1, 1, 0, 1, 1, 1};
    double dval[9];
    uint8_t dok[9];
    for (int i = 0; i < 9; i++) {
        dok[i] = mb_map_value(&map[i], sval, sok, &dval[i]);
    }
    CHECK(dok[0] && dval[0] == 230.4 && dok[1] && dval[1] == 1500 && !dok[2], "values: %g %g, a phase missing leaves the sum out", dval[0], dval[1]);
    sok[4] = 1;
    CHECK(mb_map_value(&map[2], sval, sok, &dval[2]) && dval[2] == 330, "sum of the phases %g", dval[2]);
    dok[2] = 1;

    uint8_t img[2 * 30];
    mb_template_image(&dst, MB_SPACE_HOLDING, 0, 28, dval, dok, NULL, img);
    CHECK(reg_at(img, 0) == 0 && reg_at(img, 1) == 2304, "voltage 230.4 V as 2304: %04x %04x", reg_at(img, 0), reg_at(img, 1));
    CHECK(reg_at(img, 2) == 0xffff && reg_at(img, 3) == 0xffff, "a gap reads fill");
    CHECK(reg_at(img, 4) == 0x0002 && reg_at(img, 5) == 0x49f0, "power 1500 W as 150000: %04x %04x", reg_at(img, 4), reg_at(img, 5));
    CHECK(reg_at(img, 8) == 0xffff, "frequency without value reads its nan");
    CHECK(reg_at(img, 10) == 0 && reg_at(img, 11) == 0 && reg_at(img, 12) == 0x0001 && reg_at(img, 13) == 0xe240,
          "energy 1234.56 kWh as u64 123456: %04x %04x", reg_at(img, 12), reg_at(img, 13));
    CHECK(reg_at(img, 14) == 0xffff && reg_at(img, 17) == 0xffff, "u64 without value: all ones");
    CHECK(reg_at(img, 20) == 0x4232 && reg_at(img, 21) == 0x3320 && reg_at(img, 22) == 0, "type \"B23 \": %04x %04x %04x", reg_at(img, 20),
          reg_at(img, 21), reg_at(img, 22));
    CHECK(reg_at(img, 24) == 0x00bc && reg_at(img, 25) == 0x614e, "serial 12345678: %04x %04x", reg_at(img, 24), reg_at(img, 25));
    CHECK(reg_at(img, 26) == 0 && reg_at(img, 27) == 0xffff, "a str without value reads 0, past the template fill");
    mb_template_image(&dst, MB_SPACE_HOLDING, 5, 2, dval, dok, NULL, img);
    CHECK(reg_at(img, 0) == 0x49f0 && reg_at(img, 1) == 0, "a read starting inside a value: %04x %04x", reg_at(img, 0), reg_at(img, 1));
    mb_template_image(&dst, MB_SPACE_INPUT, 0, 2, dval, dok, NULL, img);
    CHECK(reg_at(img, 0) == 0xffff, "another space reads fill");

    cJSON *j = mb_template_json(&dst);
    mb_template_t again;
    CHECK(mb_template_parse(j, &again, err, sizeof(err)) && again.fill == 65535 && strcmp(again.ent[6].emulate, "B23 ") == 0,
          "fill and emulate round trip: %s", err);
    cJSON_Delete(j);
    mb_template_free(&again);
    j = mb_template_json(&src);
    CHECK(mb_template_parse(j, &again, err, sizeof(err)) && strcmp(again.ent[1].q, "power") == 0, "q round trip: %s", err);
    cJSON_Delete(j);
    mb_template_free(&again);
    expect_refused(T_HEAD "{\"key\":\"t\",\"name\":\"T\",\"reg\":\"holding\",\"addr\":0,\"type\":\"str\",\"count\":1,\"emulate\":\"ABC\"}]}", "emulate");
    expect_refused(T_HEAD "{\"key\":\"t\",\"name\":\"T\",\"reg\":\"holding\",\"addr\":0,\"emulate\":\"1\"}]}", "emulate");
    expect_refused(T_HEAD "{\"key\":\"t\",\"q\":\"Power\",\"name\":\"T\",\"reg\":\"holding\",\"addr\":0}]}", "q:");
    expect_refused("{\"id\":\"x\",\"manufacturer\":\"M\",\"model\":\"X\",\"fill\":70000,\"entities\":[{\"key\":\"a\",\"name\":\"A\",\"reg\":\"holding\",\"addr\":0}]}",
                   "fill");
    mb_template_free(&src);
    mb_template_free(&dst);
}

static void test_builtin(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        static char text[65536];
        size_t n = f ? fread(text, 1, sizeof(text) - 1, f) : 0;
        if (f) {
            fclose(f);
        }
        text[n] = '\0';
        mb_template_t t;
        char err[160] = "";
        CHECK(n && parse_text(text, &t, err, sizeof(err)), "%s: %s", argv[i], err);
        if (!n || err[0]) {
            continue;
        }
        const char *base = strrchr(argv[i], '/') ? strrchr(argv[i], '/') + 1 : argv[i];
        char want[48];
        snprintf(want, sizeof(want), "%s.json", t.id);
        CHECK(strcmp(base, want) == 0, "%s: file name is not the id %s", argv[i], t.id);
        /* all entities read in one cycle stay within max_regs per request */
        uint16_t idx[MB_ENTITIES_MAX];
        bool single[MB_ENTITIES_MAX] = {false};
        mb_block_t b[MB_ENTITIES_MAX];
        for (int k = 0; k < t.nent; k++) {
            idx[k] = (uint16_t)k;
        }
        int nb = mb_plan(&t, idx, t.nent, single, b, MB_ENTITIES_MAX);
        for (int k = 0; k < nb; k++) {
            CHECK(b[k].count <= t.max_regs, "%s: block of %u registers", t.id, b[k].count);
        }
        if (strncmp(t.id, "eastron_", 8) == 0) {
            for (int k = 0; k < t.nent; k++) {
                const mb_entity_t *e = &t.ent[k];
                CHECK(e->space != MB_SPACE_INPUT || (e->type == MB_T_F32 && e->addr % 2 == 0), "%s %s: SDM input registers are f32 at even addresses",
                      t.id, e->key);
            }
            CHECK(t.ent[0].addr == 0 || strcmp(t.id, "eastron_sdm72d") == 0, "%s: starts at 0x0000", t.id);
        }
        printf("  %s: %d entities, %d requests per full poll\n", t.id, t.nent, nb);
        mb_template_free(&t);
    }
}

int main(int argc, char **argv)
{
    test_crc_rtu();
    test_writes();
    test_tcp();
    test_aa55();
    test_server();
    test_values();
    test_templates();
    test_emulation();
    test_builtin(argc, argv);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
