#pragma once

/*
 * Device templates (docs/modbus.md): what to read from a Modbus device and how to show it in Home
 * Assistant. Parsed from JSON (built-in templates in the firmware, user templates in NVS); plain C with
 * cJSON, built on the host by tests/unit/modbus_test.c too.
 */

#include "cJSON.h"
#include "mb_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MB_TPL_SCHEMA 1
#define MB_KEY_LEN 24
#define MB_ENTITIES_MAX 96
#define MB_OPTS_MAX 32

typedef enum { MB_W_NONE, MB_W_NUMBER, MB_W_SWITCH, MB_W_SELECT, MB_W_BUTTON } mb_write_t;

typedef struct {
    double raw;
    char label[32];
} mb_opt_t;

typedef struct {
    char key[MB_KEY_LEN];
    char name[48], name_cs[48];
    bool space_none;    /* a button sending a device specific function (no register) */
    mb_space_t space;
    uint16_t addr;
    mb_type_t type;
    mb_order_t order;
    uint8_t count;      /* registers of a str */
    int8_t bit;         /* -1, or the bit of the register (bool) */
    double scale, offset;
    bool has_nan;
    double nan_raw;     /* raw value meaning "no value" (ABB: 0xffffffff) */
    int8_t precision;
    char unit[16], device_class[40], state_class[20], category[12];
    uint8_t every;      /* read every n-th poll, 0 = once after the device answers */
    uint8_t nopts;
    mb_opt_t *opts;     /* raw value -> text (enum sensor, select options) */
    mb_write_t write;
    uint8_t write_fc;   /* 0 = automatic (5, 6 or 16), else 5, 6, 15, 16 or a device function */
    double min, max, step, on, off, press;
} mb_entity_t;

typedef struct {
    char id[32];
    char manufacturer[32], model[48], category[20];
    char notes[200], source[200];
    uint32_t baud;
    char parity;        /* 'N', 'E', 'O' */
    uint8_t stop;
    uint8_t slave;
    uint16_t timeout_ms, gap_ms, poll_s;
    uint8_t max_regs;   /* registers per read request */
    uint8_t max_gap;    /* unread registers a block read may span */
    bool builtin;
    int nent;
    mb_entity_t *ent;
} mb_template_t;

/* Parses and checks a template; false with err set (the first problem, naming the entity). The
 * template owns allocations: mb_template_free. */
bool mb_template_parse(const cJSON *json, mb_template_t *out, char *err, size_t err_len);
void mb_template_free(mb_template_t *t);
/* Back to JSON (the same format), e.g. to export a built-in template as a starting point. */
cJSON *mb_template_json(const mb_template_t *t);
const mb_entity_t *mb_template_entity(const mb_template_t *t, const char *key);

/* Registers (coils: bits) an entity reads. */
int mb_entity_regs(const mb_entity_t *e);
/* raw * scale + offset, rounded to precision */
double mb_entity_value(const mb_entity_t *e, double raw);
/* label of a raw value of an entity with options, NULL when none fits */
const char *mb_entity_label(const mb_entity_t *e, double raw);

/* ---- read planning ---- */

typedef struct {
    mb_space_t space;
    uint16_t addr, count;
    uint16_t first, n;  /* entities of the block: idx[first .. first + n) */
} mb_block_t;

/* Groups the entities in idx (indexes into t->ent, sorted here by space and address) into read requests
 * of at most max_regs registers (coils: 8 * max_regs) spanning at most max_gap unread registers; entities
 * marked in single are read alone. Returns the number of blocks. */
int mb_plan(const mb_template_t *t, uint16_t *idx, int n, const bool *single, mb_block_t *blocks, int max_blocks);

#ifdef __cplusplus
}
#endif
