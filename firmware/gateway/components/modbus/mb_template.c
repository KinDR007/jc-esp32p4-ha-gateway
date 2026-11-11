#include "mb_template.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
/* templates are large and long lived: PSRAM */
#define TPL_CALLOC(n, size) heap_caps_calloc((n), (size), MALLOC_CAP_SPIRAM)
#else
#define TPL_CALLOC(n, size) calloc((n), (size))
#endif

#define JSON_STR(obj, key) (cJSON_IsString(cJSON_GetObjectItem((obj), (key))) ? cJSON_GetObjectItem((obj), (key))->valuestring : NULL)

static const char *const s_spaces[] = {"coil", "discrete", "input", "holding"};
static const char *const s_writes[] = {"", "number", "switch", "select", "button"};

/* keys and template ids: a-z, 0-9, _ */
static bool valid_name(const char *k, size_t max)
{
    size_t n = strlen(k);
    if (n < 1 || n >= max) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_')) {
            return false;
        }
    }
    return true;
}

/* a number, or a string "0x..." / decimal */
static bool get_num(const cJSON *j, double *out)
{
    if (cJSON_IsNumber(j)) {
        *out = j->valuedouble;
        return true;
    }
    if (cJSON_IsString(j) && j->valuestring[0]) {
        char *end;
        double v = strncmp(j->valuestring, "0x", 2) == 0 || strncmp(j->valuestring, "0X", 2) == 0
                       ? (double)strtoull(j->valuestring + 2, &end, 16)
                       : strtod(j->valuestring, &end);
        if (*end == '\0') {
            *out = v;
            return true;
        }
    }
    return false;
}

static bool copy_str(const cJSON *obj, const char *key, char *out, size_t len, bool required)
{
    const cJSON *j = cJSON_GetObjectItem(obj, key);
    if (!j) {
        return !required;
    }
    if (!cJSON_IsString(j) || strlen(j->valuestring) >= len || (required && !j->valuestring[0])) {
        return false;
    }
    strcpy(out, j->valuestring);
    return true;
}

#define FAIL(...)                                                                                                      \
    do {                                                                                                               \
        snprintf(err, err_len, __VA_ARGS__);                                                                           \
        return false;                                                                                                  \
    } while (0)

/* optional number in [lo, hi], default def */
static bool opt_num(const cJSON *obj, const char *key, double lo, double hi, double def, double *out)
{
    const cJSON *j = cJSON_GetObjectItem(obj, key);
    if (!j) {
        *out = def;
        return true;
    }
    return get_num(j, out) && *out >= lo && *out <= hi;
}

static bool parse_entity(const cJSON *j, mb_entity_t *e, char *err, size_t err_len)
{
    double v;
    const char *s;
    if (!cJSON_IsObject(j) || !(s = JSON_STR(j, "key")) || !valid_name(s, MB_KEY_LEN)) {
        FAIL("key: a-z, 0-9 and _, up to %d characters", MB_KEY_LEN - 1);
    }
    strcpy(e->key, s);
    if (!copy_str(j, "q", e->q, sizeof(e->q), false) || (e->q[0] && !valid_name(e->q, sizeof(e->q)))) {
        FAIL("%s: q: a-z, 0-9 and _, up to %d characters", e->key, MB_KEY_LEN - 1);
    }
    if (!copy_str(j, "name", e->name, sizeof(e->name), true) || !copy_str(j, "name_cs", e->name_cs, sizeof(e->name_cs), false)) {
        FAIL("%s: name up to %d characters", e->key, (int)sizeof(e->name) - 1);
    }
    s = JSON_STR(j, "reg");
    e->space_none = s && strcmp(s, "none") == 0;
    bool found = e->space_none;
    for (int i = 0; s && !found && i < 4; i++) {
        if (strcmp(s, s_spaces[i]) == 0) {
            e->space = (mb_space_t)i;
            found = true;
        }
    }
    if (!found) {
        FAIL("%s: reg is coil, discrete, input, holding (or none for a button)", e->key);
    }
    if (!e->space_none) {
        if (!get_num(cJSON_GetObjectItem(j, "addr"), &v) || v < 0 || v > 65535 || v != floor(v)) {
            FAIL("%s: addr 0-65535 (number or \"0x..\")", e->key);
        }
        e->addr = (uint16_t)v;
    }
    bool bits = !e->space_none && (e->space == MB_SPACE_COIL || e->space == MB_SPACE_DISCRETE);
    s = JSON_STR(j, "type");
    if (!s) {
        e->type = bits ? MB_T_BOOL : MB_T_U16;
    } else if (!mb_type_from_name(s, &e->type)) {
        FAIL("%s: type bool, u16, s16, u32, s32, u64, s64, f32, f64 or str", e->key);
    }
    if (bits && e->type != MB_T_BOOL) {
        FAIL("%s: coils and discrete inputs are bool", e->key);
    }
    s = JSON_STR(j, "order");
    e->order = MB_ORDER_ABCD;
    if (s && !mb_order_from_name(s, &e->order)) {
        FAIL("%s: order ABCD, CDAB, BADC or DCBA", e->key);
    }
    if (!opt_num(j, "bit", -1, 15, -1, &v)) {
        FAIL("%s: bit 0-15", e->key);
    }
    e->bit = (int8_t)v;
    if (e->bit >= 0 && (bits || mb_type_regs(e->type) != 1)) {
        FAIL("%s: bit only of a 16-bit register", e->key);
    }
    if (e->bit >= 0) {
        e->type = MB_T_BOOL;
    }
    if (e->type == MB_T_BOOL && !bits && e->bit < 0) {
        FAIL("%s: bool of a register needs bit", e->key);
    }
    if (!opt_num(j, "count", 1, 64, 1, &v) || (e->type == MB_T_STR && !cJSON_GetObjectItem(j, "count"))) {
        FAIL("%s: count 1-64 registers (str)", e->key);
    }
    e->count = (uint8_t)v;
    if (!opt_num(j, "scale", -1e12, 1e12, 1, &e->scale) || e->scale == 0 || !opt_num(j, "offset", -1e12, 1e12, 0, &e->offset)) {
        FAIL("%s: scale (not 0) and offset are numbers", e->key);
    }
    const cJSON *nan = cJSON_GetObjectItem(j, "nan");
    e->has_nan = nan != NULL;
    if (nan && !get_num(nan, &e->nan_raw)) {
        FAIL("%s: nan is the raw value meaning no value", e->key);
    }
    if (!opt_num(j, "precision", 0, 6, e->type == MB_T_F32 || e->type == MB_T_F64 ? 2 : 0, &v)) {
        FAIL("%s: precision 0-6", e->key);
    }
    e->precision = (int8_t)v;
    if (!copy_str(j, "unit", e->unit, sizeof(e->unit), false) || !copy_str(j, "class", e->device_class, sizeof(e->device_class), false) ||
        !copy_str(j, "state", e->state_class, sizeof(e->state_class), false) ||
        !copy_str(j, "category", e->category, sizeof(e->category), false)) {
        FAIL("%s: unit, class, state or category too long", e->key);
    }
    if (e->category[0] && strcmp(e->category, "diagnostic") != 0 && strcmp(e->category, "config") != 0) {
        FAIL("%s: category diagnostic or config", e->key);
    }
    if (!opt_num(j, "every", 0, 255, 1, &v)) {
        FAIL("%s: every 0 (once) to 255 polls", e->key);
    }
    e->every = (uint8_t)v;

    const cJSON *map = cJSON_GetObjectItem(j, "map");
    if (map) {
        const cJSON *o;
        int n = cJSON_GetArraySize(map);
        if (!cJSON_IsObject(map) || n < 1 || n > MB_OPTS_MAX) {
            FAIL("%s: map: {\"raw value\": \"text\"}, 1-%d entries", e->key, MB_OPTS_MAX);
        }
        e->opts = TPL_CALLOC(n, sizeof(mb_opt_t));
        if (!e->opts) {
            FAIL("out of memory");
        }
        cJSON_ArrayForEach(o, map)
        {
            mb_opt_t *opt = &e->opts[e->nopts];
            cJSON k = {.type = cJSON_String, .valuestring = o->string};
            if (!get_num(&k, &opt->raw) || !cJSON_IsString(o) || strlen(o->valuestring) >= sizeof(opt->label)) {
                FAIL("%s: map entry %s: a number and a text up to %d characters", e->key, o->string, (int)sizeof(opt->label) - 1);
            }
            strcpy(opt->label, o->valuestring);
            e->nopts++;
        }
    }

    const cJSON *w = cJSON_GetObjectItem(j, "write");
    e->write = MB_W_NONE;
    if (w) {
        s = cJSON_IsObject(w) ? JSON_STR(w, "kind") : NULL;
        for (int i = 1; s && i < 5; i++) {
            if (strcmp(s, s_writes[i]) == 0) {
                e->write = (mb_write_t)i;
            }
        }
        if (e->write == MB_W_NONE) {
            FAIL("%s: write.kind number, switch, select or button", e->key);
        }
        if (!opt_num(w, "fc", 0, 127, 0, &v)) {
            FAIL("%s: write.fc 5, 6, 15, 16 or a device function", e->key);
        }
        e->write_fc = (uint8_t)v;
        bool std_fc = e->write_fc == 0 || e->write_fc == 5 || e->write_fc == 6 || e->write_fc == 15 || e->write_fc == 16;
        if (e->space_none ? e->write != MB_W_BUTTON || std_fc : !std_fc) {
            FAIL("%s: a device function (write.fc) only for a button without register (reg none)", e->key);
        }
        if (!e->space_none && (e->space == MB_SPACE_INPUT || e->space == MB_SPACE_DISCRETE)) {
            FAIL("%s: input registers and discrete inputs cannot be written", e->key);
        }
        if (!e->space_none && e->type == MB_T_STR) {
            FAIL("%s: strings cannot be written", e->key);
        }
        if (!opt_num(w, "min", -1e15, 1e15, -1e15, &e->min) || !opt_num(w, "max", -1e15, 1e15, 1e15, &e->max) || e->min > e->max ||
            !opt_num(w, "step", 0, 1e9, 0, &e->step)) {
            FAIL("%s: write.min <= write.max, step >= 0", e->key);
        }
        if (!opt_num(w, "on", -1e15, 1e15, 1, &e->on) || !opt_num(w, "off", -1e15, 1e15, 0, &e->off) ||
            !opt_num(w, "value", -1e15, 1e15, 0, &e->press)) {
            FAIL("%s: write.on, off, value are numbers", e->key);
        }
        if (e->write == MB_W_SELECT && !e->nopts) {
            FAIL("%s: a select needs map", e->key);
        }
        if (e->write == MB_W_NUMBER && (e->min <= -1e15 || e->max >= 1e15)) {
            FAIL("%s: a number needs write.min and write.max", e->key);
        }
    } else if (e->space_none) {
        FAIL("%s: reg none only for a button", e->key);
    }
    const cJSON *em = cJSON_GetObjectItem(j, "emulate");
    if (em) {
        bool text = e->type == MB_T_STR;
        if (text ? !cJSON_IsString(em) || strlen(em->valuestring) > 2u * e->count || strlen(em->valuestring) >= sizeof(e->emulate)
                 : !cJSON_IsNumber(em)) {
            FAIL("%s: emulate: a number, or a text that fits (str)", e->key);
        }
        if (text) {
            strcpy(e->emulate, em->valuestring);
        } else {
            snprintf(e->emulate, sizeof(e->emulate), "%.15g", em->valuedouble);
        }
    }
    return true;
}

bool mb_template_parse(const cJSON *json, mb_template_t *out, char *err, size_t err_len)
{
    double v;
    mb_template_t *t = out;
    memset(t, 0, sizeof(*t));
    if (!cJSON_IsObject(json)) {
        FAIL("a JSON object");
    }
    const cJSON *schema = cJSON_GetObjectItem(json, "schema");
    if (schema && (!cJSON_IsNumber(schema) || schema->valueint != MB_TPL_SCHEMA)) {
        FAIL("schema %d", MB_TPL_SCHEMA);
    }
    const char *id = JSON_STR(json, "id");
    if (!id || !valid_name(id, sizeof(t->id))) {
        FAIL("id: a-z, 0-9 and _, up to %d characters", (int)sizeof(t->id) - 1);
    }
    strcpy(t->id, id);
    if (!copy_str(json, "manufacturer", t->manufacturer, sizeof(t->manufacturer), true) ||
        !copy_str(json, "model", t->model, sizeof(t->model), true)) {
        FAIL("manufacturer and model required (up to %d and %d characters)", (int)sizeof(t->manufacturer) - 1,
             (int)sizeof(t->model) - 1);
    }
    if (!copy_str(json, "category", t->category, sizeof(t->category), false) || !copy_str(json, "notes", t->notes, sizeof(t->notes), false) ||
        !copy_str(json, "notes_cs", t->notes_cs, sizeof(t->notes_cs), false) ||
        !copy_str(json, "source", t->source, sizeof(t->source), false)) {
        FAIL("category, notes, notes_cs or source too long");
    }
    const cJSON *serial = cJSON_GetObjectItem(json, "serial");
    t->baud = 9600, t->parity = 'N', t->stop = 1;
    if (serial) {
        const char *p = JSON_STR(serial, "parity");
        if (!opt_num(serial, "baud", 1200, 115200, 9600, &v)) {
            FAIL("serial.baud 1200-115200");
        }
        t->baud = (uint32_t)v;
        if (p && (strlen(p) != 1 || !strchr("NEO", p[0]))) {
            FAIL("serial.parity N, E or O");
        }
        t->parity = p ? p[0] : 'N';
        if (!opt_num(serial, "stop", 1, 2, 1, &v)) {
            FAIL("serial.stop 1 or 2");
        }
        t->stop = (uint8_t)v;
    }
    if (!opt_num(json, "slave", 1, 247, 1, &v)) {
        FAIL("slave 1-247");
    }
    t->slave = (uint8_t)v;
    if (!opt_num(json, "timeout_ms", 50, 5000, 500, &v)) {
        FAIL("timeout_ms 50-5000");
    }
    t->timeout_ms = (uint16_t)v;
    if (!opt_num(json, "gap_ms", 0, 2000, 20, &v)) {
        FAIL("gap_ms 0-2000");
    }
    t->gap_ms = (uint16_t)v;
    if (!opt_num(json, "poll_s", 1, 3600, 10, &v)) {
        FAIL("poll_s 1-3600");
    }
    t->poll_s = (uint16_t)v;
    if (!opt_num(json, "max_regs", 1, MB_READ_REGS_MAX, 40, &v)) {
        FAIL("max_regs 1-%d", MB_READ_REGS_MAX);
    }
    t->max_regs = (uint8_t)v;
    if (!opt_num(json, "max_gap", 0, 100, 0, &v)) {
        FAIL("max_gap 0-100");
    }
    t->max_gap = (uint8_t)v;
    if (!opt_num(json, "fill", 0, 65535, 0, &v) || v != floor(v)) {
        FAIL("fill 0-65535");
    }
    t->fill = (uint16_t)v;

    const cJSON *list = cJSON_GetObjectItem(json, "entities");
    int n = cJSON_GetArraySize(list);
    if (!cJSON_IsArray(list) || n < 1 || n > MB_ENTITIES_MAX) {
        FAIL("entities: 1-%d", MB_ENTITIES_MAX);
    }
    t->ent = TPL_CALLOC(n, sizeof(mb_entity_t));
    if (!t->ent) {
        FAIL("out of memory");
    }
    const cJSON *j;
    cJSON_ArrayForEach(j, list)
    {
        mb_entity_t *e = &t->ent[t->nent];
        char why[120];
        if (!parse_entity(j, e, why, sizeof(why))) {
            snprintf(err, err_len, "entities[%d] %s", t->nent, why);
            t->nent++; /* frees its options */
            mb_template_free(t);
            return false;
        }
        t->nent++;
        for (int k = 0; k < t->nent - 1; k++) {
            if (strcmp(t->ent[k].key, e->key) == 0) {
                snprintf(err, err_len, "entities[%d] %s: key used twice", t->nent - 1, e->key);
                mb_template_free(t);
                return false;
            }
        }
        if (mb_entity_regs(e) > t->max_regs * (e->space == MB_SPACE_COIL || e->space == MB_SPACE_DISCRETE ? 8 : 1)) {
            snprintf(err, err_len, "entities[%d] %s: longer than max_regs", t->nent - 1, e->key);
            mb_template_free(t);
            return false;
        }
    }
    return true;
}

void mb_template_free(mb_template_t *t)
{
    for (int i = 0; t->ent && i < t->nent; i++) {
        free(t->ent[i].opts);
    }
    free(t->ent);
    t->ent = NULL;
    t->nent = 0;
}

static void add_num_if(cJSON *o, const char *key, double v, double def)
{
    if (v != def) {
        cJSON_AddNumberToObject(o, key, v);
    }
}

static void add_str_if(cJSON *o, const char *key, const char *v)
{
    if (v[0]) {
        cJSON_AddStringToObject(o, key, v);
    }
}

cJSON *mb_template_json(const mb_template_t *t)
{
    cJSON *j = cJSON_CreateObject();
    char hex[8];
    cJSON_AddNumberToObject(j, "schema", MB_TPL_SCHEMA);
    cJSON_AddStringToObject(j, "id", t->id);
    cJSON_AddStringToObject(j, "manufacturer", t->manufacturer);
    cJSON_AddStringToObject(j, "model", t->model);
    add_str_if(j, "category", t->category);
    add_str_if(j, "notes", t->notes);
    add_str_if(j, "notes_cs", t->notes_cs);
    add_str_if(j, "source", t->source);
    cJSON *serial = cJSON_AddObjectToObject(j, "serial");
    cJSON_AddNumberToObject(serial, "baud", t->baud);
    char parity[2] = {t->parity, 0};
    cJSON_AddStringToObject(serial, "parity", parity);
    cJSON_AddNumberToObject(serial, "stop", t->stop);
    cJSON_AddNumberToObject(j, "slave", t->slave);
    cJSON_AddNumberToObject(j, "timeout_ms", t->timeout_ms);
    cJSON_AddNumberToObject(j, "gap_ms", t->gap_ms);
    cJSON_AddNumberToObject(j, "poll_s", t->poll_s);
    cJSON_AddNumberToObject(j, "max_regs", t->max_regs);
    add_num_if(j, "max_gap", t->max_gap, 0);
    add_num_if(j, "fill", t->fill, 0);
    cJSON *list = cJSON_AddArrayToObject(j, "entities");
    for (int i = 0; i < t->nent; i++) {
        const mb_entity_t *e = &t->ent[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "key", e->key);
        add_str_if(o, "q", e->q);
        cJSON_AddStringToObject(o, "name", e->name);
        add_str_if(o, "name_cs", e->name_cs);
        cJSON_AddStringToObject(o, "reg", e->space_none ? "none" : s_spaces[e->space]);
        if (!e->space_none) {
            snprintf(hex, sizeof(hex), "0x%04x", e->addr);
            cJSON_AddStringToObject(o, "addr", hex);
            if (e->bit < 0) {
                cJSON_AddStringToObject(o, "type", mb_type_name(e->type));
            }
        }
        if (e->order != MB_ORDER_ABCD) {
            cJSON_AddStringToObject(o, "order", mb_order_name(e->order));
        }
        add_num_if(o, "bit", e->bit, -1);
        if (e->type == MB_T_STR) {
            cJSON_AddNumberToObject(o, "count", e->count);
        }
        add_num_if(o, "scale", e->scale, 1);
        add_num_if(o, "offset", e->offset, 0);
        if (e->has_nan) {
            cJSON_AddNumberToObject(o, "nan", e->nan_raw);
        }
        cJSON_AddNumberToObject(o, "precision", e->precision);
        add_str_if(o, "unit", e->unit);
        add_str_if(o, "class", e->device_class);
        add_str_if(o, "state", e->state_class);
        add_str_if(o, "category", e->category);
        add_num_if(o, "every", e->every, 1);
        if (e->nopts) {
            cJSON *map = cJSON_AddObjectToObject(o, "map");
            for (int k = 0; k < e->nopts; k++) {
                char raw[24];
                snprintf(raw, sizeof(raw), "%.15g", e->opts[k].raw);
                cJSON_AddStringToObject(map, raw, e->opts[k].label);
            }
        }
        if (e->write) {
            cJSON *w = cJSON_AddObjectToObject(o, "write");
            cJSON_AddStringToObject(w, "kind", s_writes[e->write]);
            add_num_if(w, "fc", e->write_fc, 0);
            add_num_if(w, "min", e->min, -1e15);
            add_num_if(w, "max", e->max, 1e15);
            add_num_if(w, "step", e->step, 0);
            if (e->write == MB_W_SWITCH) {
                cJSON_AddNumberToObject(w, "on", e->on);
                cJSON_AddNumberToObject(w, "off", e->off);
            }
            if (e->write == MB_W_BUTTON) {
                cJSON_AddNumberToObject(w, "value", e->press);
            }
        }
        if (e->emulate[0]) {
            if (e->type == MB_T_STR) {
                cJSON_AddStringToObject(o, "emulate", e->emulate);
            } else {
                cJSON_AddNumberToObject(o, "emulate", strtod(e->emulate, NULL));
            }
        }
        cJSON_AddItemToArray(list, o);
    }
    return j;
}

const mb_entity_t *mb_template_entity(const mb_template_t *t, const char *key)
{
    for (int i = 0; i < t->nent; i++) {
        if (strcmp(t->ent[i].key, key) == 0) {
            return &t->ent[i];
        }
    }
    return NULL;
}

int mb_entity_regs(const mb_entity_t *e)
{
    if (e->space_none) {
        return 0;
    }
    if (e->space == MB_SPACE_COIL || e->space == MB_SPACE_DISCRETE) {
        return 1; /* bits */
    }
    return e->type == MB_T_STR ? e->count : e->bit >= 0 ? 1 : mb_type_regs(e->type);
}

double mb_entity_value(const mb_entity_t *e, double raw)
{
    double v = raw * e->scale + e->offset;
    double f = pow(10, e->precision);
    return round(v * f) / f;
}

const char *mb_entity_label(const mb_entity_t *e, double raw)
{
    for (int i = 0; i < e->nopts; i++) {
        if (e->opts[i].raw == raw) {
            return e->opts[i].label;
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------- emulation */

const char *mb_entity_q(const mb_entity_t *e)
{
    return e->q[0] ? e->q : e->key;
}

/* factor from a source unit to a target unit (W -> kW: 0.001), 0 when they do not convert */
static double unit_factor(const char *from, const char *to)
{
    if (strcmp(from, to) == 0) {
        return 1;
    }
    if (from[0] == 'k' && strcmp(from + 1, to) == 0) {
        return 1000;
    }
    if (to[0] == 'k' && strcmp(to + 1, from) == 0) {
        return 0.001;
    }
    return 0;
}

/* the source entity of quantity q a target entity can take its value from */
static int find_q(const mb_template_t *s, const char *q, const mb_entity_t *te, double *factor)
{
    for (int i = 0; i < s->nent; i++) {
        const mb_entity_t *se = &s->ent[i];
        if (se->space_none || strcmp(mb_entity_q(se), q) != 0 || (te->type == MB_T_STR) != (se->type == MB_T_STR) ||
            (te->type == MB_T_BOOL) != (se->type == MB_T_BOOL) || te->nopts != se->nopts) {
            continue;
        }
        bool same = true;
        for (int k = 0; k < te->nopts; k++) {
            same &= te->opts[k].raw == se->opts[k].raw && strcmp(te->opts[k].label, se->opts[k].label) == 0;
        }
        double f = te->type == MB_T_STR || te->type == MB_T_BOOL || te->nopts ? 1 : unit_factor(se->unit, te->unit);
        if (same && f != 0) {
            *factor = f;
            return i;
        }
    }
    return -1;
}

int mb_map_resolve(const mb_template_t *target, const mb_template_t *source, mb_map_t *map)
{
    static const char *const sums[] = {"power", "reactive_power", "apparent_power", "import_energy", "export_energy",
                                       "import_reactive_energy", "export_reactive_energy"};
    int resolved = 0;
    for (int i = 0; i < target->nent; i++) {
        const mb_entity_t *te = &target->ent[i];
        const char *q = mb_entity_q(te);
        mb_map_t *m = &map[i];
        memset(m, 0, sizeof(*m));
        m->src[0] = m->src[1] = m->src[2] = -1;
        double f[3];
        char name[MB_KEY_LEN + 4];
        if (te->emulate[0]) {
            m->kind = MB_MAP_CONST;
        } else if (te->space_none) {
            m->kind = MB_MAP_NONE;
        } else if ((m->src[0] = (int16_t)find_q(source, q, te, &m->factor)) >= 0) {
            m->kind = MB_MAP_ONE;
        } else {
            for (size_t k = 0; k < sizeof(sums) / sizeof(sums[0]) && m->kind == MB_MAP_NONE; k++) {
                if (strcmp(q, sums[k]) != 0) {
                    continue;
                }
                int found = 0;
                for (int p = 0; p < 3; p++) {
                    snprintf(name, sizeof(name), "%s_l%d", q, p + 1);
                    found += (m->src[p] = (int16_t)find_q(source, name, te, &f[p])) >= 0;
                }
                if (found == 3 && f[0] == f[1] && f[1] == f[2]) {
                    m->kind = MB_MAP_SUM3;
                    m->factor = f[0];
                }
            }
            if (m->kind == MB_MAP_NONE && strcmp(q, "power") == 0 && (m->src[0] = (int16_t)find_q(source, "import_power", te, &f[0])) >= 0 &&
                (m->src[1] = (int16_t)find_q(source, "export_power", te, &f[1])) >= 0 && f[0] == f[1]) {
                m->kind = MB_MAP_DIFF;
                m->factor = f[0];
            }
            size_t n = strlen(q);
            if (m->kind == MB_MAP_NONE && n > 3 && strcmp(q + n - 3, "_l1") == 0) {
                /* a single phase source on L1 */
                snprintf(name, sizeof(name), "%.*s_l2", (int)(n - 3), q);
                char base[MB_KEY_LEN];
                snprintf(base, sizeof(base), "%.*s", (int)(n - 3), q);
                if (find_q(source, name, te, &f[1]) < 0 && (m->src[0] = (int16_t)find_q(source, base, te, &m->factor)) >= 0) {
                    m->kind = MB_MAP_ONE;
                }
            }
        }
        if (m->kind == MB_MAP_NONE) {
            m->src[0] = m->src[1] = m->src[2] = -1;
        }
        resolved += m->kind != MB_MAP_NONE;
    }
    return resolved;
}

bool mb_map_value(const mb_map_t *m, const double *val, const uint8_t *ok, double *out)
{
    switch (m->kind) {
    case MB_MAP_ONE:
        if (ok[m->src[0]] != 1) {
            return false;
        }
        *out = val[m->src[0]] * m->factor;
        return true;
    case MB_MAP_SUM3:
        if (ok[m->src[0]] != 1 || ok[m->src[1]] != 1 || ok[m->src[2]] != 1) {
            return false;
        }
        *out = (val[m->src[0]] + val[m->src[1]] + val[m->src[2]]) * m->factor;
        return true;
    case MB_MAP_DIFF:
        if (ok[m->src[0]] != 1 || ok[m->src[1]] != 1) {
            return false;
        }
        *out = (val[m->src[0]] - val[m->src[1]]) * m->factor;
        return true;
    default:
        return false;
    }
}

void mb_map_describe(const mb_map_t *m, const mb_template_t *source, char *out, size_t len)
{
    const char *k[3] = {"", "", ""};
    for (int p = 0; p < 3; p++) {
        if (m->src[p] >= 0 && m->src[p] < source->nent) {
            k[p] = source->ent[m->src[p]].key;
        }
    }
    int n = m->kind == MB_MAP_ONE    ? snprintf(out, len, "%s", k[0])
            : m->kind == MB_MAP_SUM3 ? snprintf(out, len, "%s+%s+%s", k[0], k[1], k[2])
            : m->kind == MB_MAP_DIFF ? snprintf(out, len, "%s-%s", k[0], k[1])
            : m->kind == MB_MAP_CONST ? snprintf(out, len, "fixed")
                                      : snprintf(out, len, "%s", "");
    if (n > 0 && (size_t)n < len && m->factor != 0 && m->factor != 1) {
        snprintf(out + n, len - n, " x%g", m->factor);
    }
}

void mb_template_image(const mb_template_t *t, mb_space_t space, uint16_t addr, uint16_t count, const double *val, const uint8_t *ok,
                       char *const *text, uint8_t *out)
{
    bool bits = space == MB_SPACE_COIL || space == MB_SPACE_DISCRETE;
    if (bits) {
        memset(out, 0, (count + 7u) / 8);
    } else {
        for (int r = 0; r < count; r++) {
            out[2 * r] = (uint8_t)(t->fill >> 8);
            out[2 * r + 1] = (uint8_t)t->fill;
        }
    }
    for (int i = 0; i < t->nent; i++) {
        const mb_entity_t *e = &t->ent[i];
        int n = mb_entity_regs(e);
        if (e->space_none || e->space != space || e->addr + n <= addr || e->addr >= addr + count) {
            continue;
        }
        double v = 0;
        bool have = e->emulate[0] ? (v = strtod(e->emulate, NULL), true) : ok[i] == 1 ? (v = val[i], true) : false;
        if (bits || e->bit >= 0) {
            bool on = have && v != 0;
            if (bits) {
                int pos = e->addr - addr;
                out[pos / 8] |= (uint8_t)(on << (pos % 8));
            } else {
                uint8_t *w = &out[2 * (e->addr - addr)];
                uint16_t reg = (uint16_t)(w[0] << 8 | w[1]);
                reg = on ? reg | (1u << e->bit) : reg & ~(1u << e->bit);
                w[0] = (uint8_t)(reg >> 8), w[1] = (uint8_t)reg;
            }
            continue;
        }
        uint16_t regs[64] = {0};
        int nr = n;
        if (e->type == MB_T_STR) {
            const char *s = e->emulate[0] ? e->emulate : have && text && text[i] ? text[i] : "";
            for (int c = 0; c < 2 * n && s[c]; c++) {
                regs[c / 2] |= (uint16_t)((uint8_t)s[c] << (c % 2 ? 0 : 8));
            }
        } else {
            double raw = e->nopts || e->emulate[0] ? v : (v - e->offset) / e->scale;
            if (!have || !mb_encode(raw, e->type, e->order, regs, &nr)) {
                memset(regs, 0, sizeof(regs));
                if (e->has_nan && !mb_encode(e->nan_raw, e->type, e->order, regs, &nr)) {
                    memset(regs, 0xff, sizeof(regs)); /* u64 all ones does not fit a double exactly */
                }
            }
        }
        for (int r = 0; r < n; r++) {
            int pos = e->addr + r - addr;
            if (pos >= 0 && pos < count) {
                out[2 * pos] = (uint8_t)(regs[r] >> 8);
                out[2 * pos + 1] = (uint8_t)regs[r];
            }
        }
    }
}

/* ---------------------------------------------------------------- planning */

static const mb_template_t *s_sort_tpl;

static int cmp_entities(const void *a, const void *b)
{
    const mb_entity_t *x = &s_sort_tpl->ent[*(const uint16_t *)a], *y = &s_sort_tpl->ent[*(const uint16_t *)b];
    if (x->space_none != y->space_none) {
        return x->space_none ? 1 : -1; /* buttons without a register last, never read */
    }
    if (x->space != y->space) {
        return (int)x->space - (int)y->space;
    }
    return (int)x->addr - (int)y->addr;
}

int mb_plan(const mb_template_t *t, uint16_t *idx, int n, const bool *single, mb_block_t *blocks, int max_blocks)
{
    int nb = 0;
    s_sort_tpl = t;
    qsort(idx, n, sizeof(idx[0]), cmp_entities);
    for (int i = 0; i < n; i++) {
        const mb_entity_t *e = &t->ent[idx[i]];
        if (e->space_none) {
            break;
        }
        bool bits = e->space == MB_SPACE_COIL || e->space == MB_SPACE_DISCRETE;
        int limit = t->max_regs * (bits ? 8 : 1);
        uint32_t end = e->addr + mb_entity_regs(e);
        mb_block_t *b = nb ? &blocks[nb - 1] : NULL;
        bool join = b && !single[idx[i]] && !single[idx[b->first]] && b->space == e->space && e->addr >= b->addr &&
                    e->addr <= b->addr + b->count + t->max_gap && (end > b->addr + b->count ? end : b->addr + b->count) - b->addr <= (uint32_t)limit;
        if (join) {
            if (end > (uint32_t)(b->addr + b->count)) {
                b->count = (uint16_t)(end - b->addr);
            }
            b->n++;
            continue;
        }
        if (nb == max_blocks) {
            break;
        }
        blocks[nb++] = (mb_block_t){.space = e->space, .addr = e->addr, .count = (uint16_t)(end - e->addr), .first = (uint16_t)i, .n = 1};
    }
    return nb;
}
