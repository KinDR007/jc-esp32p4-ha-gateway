#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "esp_timer.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/sha256.h"
#include "qrcode.h"
#include "gw.h"
#include "gw_auth_priv.h"

static const char *TAG = "gw_auth";

/* Users, roles and permissions of the web UI and the REST API.
 *
 * Users (max AUTH_USERS) have a role, admin, user or viewer. Every page of the web UI has a level
 * per role (hidden, read, edit), and a user can have own levels for some pages. Admins always have
 * every right; the user management itself is for admins only. The server enforces the levels: each
 * API URI maps to a page, GET needs read, other methods need edit (AUTH_MAP).
 *
 * Clients prove who they are by a session cookie (web UI login with optional TOTP), by an API token
 * (Authorization: Bearer, for Home Assistant and scripts) or by HTTP Basic (only users without 2FA,
 * for clients that used the single admin password before). Without any user the gateway is open,
 * as it was without a password. */

#define AUTH_USERS 16
#define AUTH_TOKENS 16
#define AUTH_SESSIONS 24
#define AUTH_REQUESTS 16
#define PBKDF2_ITER 10000
#define RECOVERY_CODES 10
#define TOTP_STEP 30
#define BASIC_CACHE 4
#define BASIC_CACHE_US (10 * 60 * 1000000LL)
#define COOKIE "p4s"

#define KEY_USER_FMT "usr%d"   /* str JSON per user */
#define KEY_ROLES "auth_roles" /* str JSON {"user": {page: level}, "viewer": {...}} */
#define KEY_POLICY "auth_pol"  /* str JSON policy */
#define KEY_TOKENS "auth_tok"  /* str JSON array */
#define KEY_SESSIONS "auth_ses" /* blob session_t[] */
#define KEY_REQUESTS "auth_req" /* str JSON array of password reset requests */

enum { ROLE_ADMIN, ROLE_USER, ROLE_VIEWER, ROLE_COUNT };
static const char *const s_role_names[ROLE_COUNT] = {"admin", "user", "viewer"};

/* Pages of the web UI (routes), in menu order. */
static const char *const s_pages[] = {
    "overview",       "role",           "thread/info",   "thread/topology", "thread/diag",
    "thread/settings", "zigbee/info",   "zigbee/diag",   "zigbee/backups",  "zigbee/network",
    "sound/live",     "sound/events",   "sound/log",     "sound/classes",   "sound/learn",
    "ha/guide",       "ha/mqtt",        "ha/esphome",    "system/info",     "system/lan",
    "system/firmware", "system/diag",   "system/netscan", "system/settings", "system/maint",
};
#define PAGES (sizeof(s_pages) / sizeof(s_pages[0]))

/* defaults of the user and viewer roles, in the order of s_pages (0 hidden, 1 read, 2 edit) */
static const uint8_t s_default_levels[2][PAGES] = {
    /* user: operates the gateway, no firmware, maintenance, MQTT or system settings */
    {2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 2, 2, 0, 1},
    /* viewer: reads, no keys (Thread settings, Zigbee backups, MQTT, system settings, maintenance) */
    {1, 1, 1, 1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 0, 0},
};

typedef struct {
    bool used;
    char name[32];
    uint8_t role;
    bool disabled;
    bool must_change;
    uint8_t salt[16];
    uint8_t hash[32];
    bool totp;
    uint8_t secret[20];
    uint8_t pending[20]; /* secret being set up, not yet confirmed by a code */
    bool has_pending;
    uint8_t recovery[RECOVERY_CODES][16]; /* sha256 of each code, first 16 bytes; zero = used */
    uint8_t levels[PAGES];                /* own level per page, 0xff = the role's */
    int64_t created, last_login;          /* unix time, 0 unknown */
    gw_tg_prefs_t tg;                     /* Telegram chat and notification settings */
    /* runtime only */
    uint8_t fails;
    int64_t locked_until_us;
    int64_t last_counter; /* last TOTP counter accepted (no replay) */
} user_t;

typedef struct {
    bool used;
    char name[32];
    uint8_t role;
    uint8_t hash[32];
    int64_t created;
    int64_t last_used_us; /* runtime */
} token_t;

typedef struct {
    uint8_t hash[32]; /* sha256 of the cookie value */
    int8_t user;
    bool remember;
    int64_t expires; /* unix, remembered sessions; 0 = until idle */
} session_t;

typedef struct {
    int idle_h, remember_d, lock_n, lock_min, pw_min;
    bool totp_role[ROLE_COUNT];
} policy_t;

typedef struct {
    char name[32];
    int64_t time;
    char ip[48];
} request_t;

static user_t s_users[AUTH_USERS];
static token_t s_tokens[AUTH_TOKENS];
static session_t s_sessions[AUTH_SESSIONS];
static int64_t s_session_seen_us[AUTH_SESSIONS];
static uint8_t s_role_levels[2][PAGES];
static policy_t s_policy;
static request_t s_requests[AUTH_REQUESTS];
static SemaphoreHandle_t s_lock;

static struct {
    uint8_t key[32]; /* sha256 of "user:password" */
    int8_t user;
    int64_t until_us;
} s_basic_cache[BASIC_CACHE];

/* identity of the request being handled (the HTTP server runs one handler at a time) */
static struct {
    int user;  /* index or -1 */
    int token; /* index or -1 */
    int session;
    uint8_t role;
    bool open; /* no users: everyone is admin */
} s_cur;

/* ---- helpers ---- */

static int64_t now_unix(void)
{
    time_t t = time(NULL);
    return t > 1700000000 ? (int64_t)t : 0;
}

static bool equal_ct(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) {
        diff |= a[i] ^ b[i];
    }
    return diff == 0;
}

static void hex(const uint8_t *in, size_t len, char *out)
{
    for (size_t i = 0; i < len; i++) {
        sprintf(out + 2 * i, "%02x", in[i]);
    }
}

static bool unhex(const char *in, uint8_t *out, size_t len)
{
    if (!in || strlen(in) != 2 * len) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        unsigned v;
        if (sscanf(in + 2 * i, "%2x", &v) != 1) {
            return false;
        }
        out[i] = v;
    }
    return true;
}

static void sha256(const void *in, size_t len, uint8_t out[32])
{
    mbedtls_sha256(in, len, out, 0);
}

static void pw_hash(const char *password, const uint8_t salt[16], uint8_t out[32])
{
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)password, strlen(password), salt, 16,
                                  PBKDF2_ITER, 32, out);
}

static void base32(const uint8_t *in, size_t len, char *out)
{
    static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    uint32_t buffer = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        buffer = buffer << 8 | in[i];
        bits += 8;
        while (bits >= 5) {
            *out++ = alphabet[(buffer >> (bits - 5)) & 31];
            bits -= 5;
        }
    }
    if (bits) {
        *out++ = alphabet[(buffer << (5 - bits)) & 31];
    }
    *out = '\0';
}

/* RFC 6238: HMAC-SHA1, 30 s, 6 digits */
static uint32_t totp(const uint8_t secret[20], int64_t counter)
{
    uint8_t msg[8], mac[20];
    for (int i = 7; i >= 0; i--) {
        msg[i] = counter & 0xff;
        counter >>= 8;
    }
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA1), secret, 20, msg, 8, mac);
    int off = mac[19] & 0x0f;
    uint32_t bin = (mac[off] & 0x7f) << 24 | mac[off + 1] << 16 | mac[off + 2] << 8 | mac[off + 3];
    return bin % 1000000;
}

/* A TOTP code (one step of clock skew either way) or an unused recovery code. */
static bool check_code(user_t *u, const uint8_t secret[20], const char *code, bool allow_recovery)
{
    if (!code) {
        return false;
    }
    char clean[16];
    size_t n = 0;
    for (const char *p = code; *p && n < sizeof(clean) - 1; p++) {
        if (*p != ' ' && *p != '-') {
            clean[n++] = *p >= 'a' && *p <= 'z' ? *p - 32 : *p;
        }
    }
    clean[n] = '\0';
    if (n == 6 && strspn(clean, "0123456789") == 6) {
        int64_t t = now_unix();
        if (!t) {
            return false; /* clock not set: only recovery codes work */
        }
        uint32_t want = (uint32_t)atoi(clean);
        for (int d = -1; d <= 1; d++) {
            int64_t c = t / TOTP_STEP + d;
            if (c > u->last_counter && totp(secret, c) == want) {
                u->last_counter = c;
                return true;
            }
        }
        return false;
    }
    if (allow_recovery && n == 8) {
        uint8_t h[32];
        sha256(clean, n, h);
        for (int i = 0; i < RECOVERY_CODES; i++) {
            static const uint8_t zero[16];
            if (!equal_ct(u->recovery[i], zero, 16) && equal_ct(u->recovery[i], h, 16)) {
                memset(u->recovery[i], 0, 16);
                return true;
            }
        }
    }
    return false;
}

static int recovery_left(const user_t *u)
{
    static const uint8_t zero[16];
    int n = 0;
    for (int i = 0; i < RECOVERY_CODES; i++) {
        n += !equal_ct(u->recovery[i], zero, 16);
    }
    return n;
}

static int find_user(const char *name)
{
    for (int i = 0; i < AUTH_USERS; i++) {
        if (s_users[i].used && name && strcmp(s_users[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static int user_count(void)
{
    int n = 0;
    for (int i = 0; i < AUTH_USERS; i++) {
        n += s_users[i].used;
    }
    return n;
}

static int admin_count(void)
{
    int n = 0;
    for (int i = 0; i < AUTH_USERS; i++) {
        n += s_users[i].used && !s_users[i].disabled && s_users[i].role == ROLE_ADMIN;
    }
    return n;
}

static int page_index(const char *page)
{
    for (size_t i = 0; i < PAGES; i++) {
        if (strcmp(s_pages[i], page) == 0) {
            return i;
        }
    }
    return -1;
}

static int role_from_name(const char *name)
{
    for (int i = 0; i < ROLE_COUNT; i++) {
        if (name && strcmp(name, s_role_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

static uint8_t level_of(int role, const user_t *u, int page)
{
    if (role == ROLE_ADMIN) {
        return 2;
    }
    if (u && u->levels[page] != 0xff) {
        return u->levels[page];
    }
    return s_role_levels[role - 1][page];
}

static bool name_valid(const char *name)
{
    size_t len = name ? strlen(name) : 0;
    if (len < 2 || len > 31) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')) {
            return false;
        }
    }
    return true;
}

/* ---- storage ---- */

static void save_user(int i)
{
    char key[8];
    snprintf(key, sizeof(key), KEY_USER_FMT, i);
    user_t *u = &s_users[i];
    if (!u->used) {
        gw_config_erase(key);
        return;
    }
    char buf[80];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", u->name);
    cJSON_AddStringToObject(j, "role", s_role_names[u->role]);
    cJSON_AddBoolToObject(j, "disabled", u->disabled);
    cJSON_AddBoolToObject(j, "must_change", u->must_change);
    hex(u->salt, 16, buf);
    cJSON_AddStringToObject(j, "salt", buf);
    hex(u->hash, 32, buf);
    cJSON_AddStringToObject(j, "hash", buf);
    if (u->totp) {
        hex(u->secret, 20, buf);
        cJSON_AddStringToObject(j, "totp", buf);
        cJSON *rc = cJSON_AddArrayToObject(j, "recovery");
        for (int k = 0; k < RECOVERY_CODES; k++) {
            hex(u->recovery[k], 16, buf);
            cJSON_AddItemToArray(rc, cJSON_CreateString(buf));
        }
    }
    cJSON *lv = cJSON_AddObjectToObject(j, "levels");
    for (size_t p = 0; p < PAGES; p++) {
        if (u->levels[p] != 0xff) {
            cJSON_AddNumberToObject(lv, s_pages[p], u->levels[p]);
        }
    }
    cJSON_AddNumberToObject(j, "created", (double)u->created);
    cJSON_AddNumberToObject(j, "last_login", (double)u->last_login);
    cJSON *tg = cJSON_AddObjectToObject(j, "tg");
    cJSON_AddNumberToObject(tg, "chat", (double)u->tg.chat);
    cJSON *tl = cJSON_AddArrayToObject(tg, "levels");
    for (int t = 0; t < GW_TG_TOPICS; t++) {
        cJSON_AddItemToArray(tl, cJSON_CreateNumber(u->tg.levels[t]));
    }
    cJSON_AddNumberToObject(tg, "quiet_from", u->tg.quiet_from);
    cJSON_AddNumberToObject(tg, "quiet_to", u->tg.quiet_to);
    cJSON_AddStringToObject(tg, "lang", u->tg.lang);
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(key, text);
        cJSON_free(text);
    }
}

static void load_user(int i)
{
    char key[8], *text = malloc(2048);
    snprintf(key, sizeof(key), KEY_USER_FMT, i);
    user_t *u = &s_users[i];
    memset(u, 0, sizeof(*u));
    memset(u->levels, 0xff, sizeof(u->levels));
    cJSON *j = text && gw_config_get_str(key, text, 2048) ? cJSON_Parse(text) : NULL;
    free(text);
    int role = role_from_name(GW_JSON_STR(j, "role"));
    if (!j || !name_valid(GW_JSON_STR(j, "name")) || role < 0 || !unhex(GW_JSON_STR(j, "salt"), u->salt, 16) ||
        !unhex(GW_JSON_STR(j, "hash"), u->hash, 32)) {
        cJSON_Delete(j);
        return;
    }
    u->used = true;
    strlcpy(u->name, GW_JSON_STR(j, "name"), sizeof(u->name));
    u->role = role;
    u->disabled = cJSON_IsTrue(cJSON_GetObjectItem(j, "disabled"));
    u->must_change = cJSON_IsTrue(cJSON_GetObjectItem(j, "must_change"));
    u->totp = unhex(GW_JSON_STR(j, "totp"), u->secret, 20);
    const cJSON *rc = cJSON_GetObjectItem(j, "recovery");
    for (int k = 0; k < RECOVERY_CODES && u->totp; k++) {
        const cJSON *c = cJSON_GetArrayItem(rc, k);
        unhex(cJSON_IsString(c) ? c->valuestring : NULL, u->recovery[k], 16);
    }
    const cJSON *lv;
    cJSON_ArrayForEach(lv, cJSON_GetObjectItem(j, "levels"))
    {
        int p = page_index(lv->string);
        if (p >= 0 && cJSON_IsNumber(lv) && lv->valueint >= 0 && lv->valueint <= 2) {
            u->levels[p] = lv->valueint;
        }
    }
    const cJSON *n;
    u->created = cJSON_IsNumber(n = cJSON_GetObjectItem(j, "created")) ? (int64_t)n->valuedouble : 0;
    u->last_login = cJSON_IsNumber(n = cJSON_GetObjectItem(j, "last_login")) ? (int64_t)n->valuedouble : 0;
    gw_tg_prefs_default(&u->tg);
    const cJSON *tg = cJSON_GetObjectItem(j, "tg");
    if (tg) {
        u->tg.chat = cJSON_IsNumber(n = cJSON_GetObjectItem(tg, "chat")) ? (int64_t)n->valuedouble : 0;
        for (int t = 0; t < GW_TG_TOPICS; t++) {
            const cJSON *l = cJSON_GetArrayItem(cJSON_GetObjectItem(tg, "levels"), t);
            if (cJSON_IsNumber(l) && l->valueint >= 0 && l->valueint <= 3) {
                u->tg.levels[t] = l->valueint;
            }
        }
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(tg, "quiet_from"))) {
            u->tg.quiet_from = n->valueint;
        }
        if (cJSON_IsNumber(n = cJSON_GetObjectItem(tg, "quiet_to"))) {
            u->tg.quiet_to = n->valueint;
        }
        if (GW_JSON_STR(tg, "lang")) {
            strlcpy(u->tg.lang, GW_JSON_STR(tg, "lang"), sizeof(u->tg.lang));
        }
    }
    cJSON_Delete(j);
}

static cJSON *levels_json(const uint8_t *levels)
{
    cJSON *j = cJSON_CreateObject();
    for (size_t p = 0; p < PAGES; p++) {
        cJSON_AddNumberToObject(j, s_pages[p], levels[p]);
    }
    return j;
}

static void save_roles(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "user", levels_json(s_role_levels[0]));
    cJSON_AddItemToObject(j, "viewer", levels_json(s_role_levels[1]));
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(KEY_ROLES, text);
        cJSON_free(text);
    }
}

static void parse_levels(const cJSON *j, uint8_t *levels)
{
    const cJSON *lv;
    cJSON_ArrayForEach(lv, j)
    {
        int p = page_index(lv->string);
        if (p >= 0 && cJSON_IsNumber(lv) && lv->valueint >= 0 && lv->valueint <= 2) {
            levels[p] = lv->valueint;
        }
    }
}

static void load_roles(void)
{
    memcpy(s_role_levels, s_default_levels, sizeof(s_role_levels));
    char *text = malloc(2048);
    cJSON *j = text && gw_config_get_str(KEY_ROLES, text, 2048) ? cJSON_Parse(text) : NULL;
    free(text);
    parse_levels(cJSON_GetObjectItem(j, "user"), s_role_levels[0]);
    parse_levels(cJSON_GetObjectItem(j, "viewer"), s_role_levels[1]);
    cJSON_Delete(j);
}

static cJSON *policy_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "idle_h", s_policy.idle_h);
    cJSON_AddNumberToObject(j, "remember_d", s_policy.remember_d);
    cJSON_AddNumberToObject(j, "lock_attempts", s_policy.lock_n);
    cJSON_AddNumberToObject(j, "lock_min", s_policy.lock_min);
    cJSON_AddNumberToObject(j, "password_min", s_policy.pw_min);
    cJSON *t = cJSON_AddArrayToObject(j, "totp_required");
    for (int r = 0; r < ROLE_COUNT; r++) {
        if (s_policy.totp_role[r]) {
            cJSON_AddItemToArray(t, cJSON_CreateString(s_role_names[r]));
        }
    }
    return j;
}

/* Applies the known fields of j within their limits; returns an error text or NULL. */
static const char *policy_apply(const cJSON *j)
{
    static const struct {
        const char *key;
        int min, max;
        size_t off;
    } fields[] = {
        {"idle_h", 1, 720, offsetof(policy_t, idle_h)},           {"remember_d", 0, 365, offsetof(policy_t, remember_d)},
        {"lock_attempts", 3, 20, offsetof(policy_t, lock_n)},     {"lock_min", 1, 1440, offsetof(policy_t, lock_min)},
        {"password_min", 8, 64, offsetof(policy_t, pw_min)},
    };
    for (size_t i = 0; i < sizeof(fields) / sizeof(fields[0]); i++) {
        const cJSON *v = cJSON_GetObjectItem(j, fields[i].key);
        if (!v) {
            continue;
        }
        if (!cJSON_IsNumber(v) || v->valueint < fields[i].min || v->valueint > fields[i].max) {
            return fields[i].key;
        }
        *(int *)((char *)&s_policy + fields[i].off) = v->valueint;
    }
    const cJSON *t = cJSON_GetObjectItem(j, "totp_required");
    if (cJSON_IsArray(t)) {
        memset(s_policy.totp_role, 0, sizeof(s_policy.totp_role));
        const cJSON *r;
        cJSON_ArrayForEach(r, t)
        {
            int role = role_from_name(cJSON_IsString(r) ? r->valuestring : NULL);
            if (role < 0) {
                return "totp_required";
            }
            s_policy.totp_role[role] = true;
        }
    }
    return NULL;
}

static void load_policy(void)
{
    s_policy = (policy_t){.idle_h = 12, .remember_d = 30, .lock_n = 5, .lock_min = 15, .pw_min = 8};
    char text[256];
    cJSON *j = gw_config_get_str(KEY_POLICY, text, sizeof(text)) ? cJSON_Parse(text) : NULL;
    if (j) {
        policy_apply(j);
    }
    cJSON_Delete(j);
}

static void save_policy(void)
{
    cJSON *j = policy_json();
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(KEY_POLICY, text);
        cJSON_free(text);
    }
}

static void save_tokens(void)
{
    char buf[80];
    cJSON *j = cJSON_CreateArray();
    for (int i = 0; i < AUTH_TOKENS; i++) {
        token_t *t = &s_tokens[i];
        if (!t->used) {
            continue;
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", t->name);
        cJSON_AddStringToObject(o, "role", s_role_names[t->role]);
        hex(t->hash, 32, buf);
        cJSON_AddStringToObject(o, "hash", buf);
        cJSON_AddNumberToObject(o, "created", (double)t->created);
        cJSON_AddItemToArray(j, o);
    }
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(KEY_TOKENS, text);
        cJSON_free(text);
    }
}

static void load_tokens(void)
{
    char *text = malloc(3072);
    cJSON *j = text && gw_config_get_str(KEY_TOKENS, text, 3072) ? cJSON_Parse(text) : NULL;
    free(text);
    int i = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, j)
    {
        token_t *t = &s_tokens[i];
        int role = role_from_name(GW_JSON_STR(o, "role"));
        if (i >= AUTH_TOKENS || role < 0 || !GW_JSON_STR(o, "name") || !unhex(GW_JSON_STR(o, "hash"), t->hash, 32)) {
            continue;
        }
        t->used = true;
        t->role = role;
        strlcpy(t->name, GW_JSON_STR(o, "name"), sizeof(t->name));
        const cJSON *c = cJSON_GetObjectItem(o, "created");
        t->created = cJSON_IsNumber(c) ? (int64_t)c->valuedouble : 0;
        i++;
    }
    cJSON_Delete(j);
}

static void save_sessions(void)
{
    gw_config_set_blob(KEY_SESSIONS, s_sessions, sizeof(s_sessions));
}

static void save_requests(void)
{
    cJSON *j = cJSON_CreateArray();
    for (int i = 0; i < AUTH_REQUESTS; i++) {
        if (s_requests[i].name[0]) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", s_requests[i].name);
            cJSON_AddNumberToObject(o, "time", (double)s_requests[i].time);
            cJSON_AddStringToObject(o, "ip", s_requests[i].ip);
            cJSON_AddItemToArray(j, o);
        }
    }
    char *text = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    if (text) {
        gw_config_set_str(KEY_REQUESTS, text);
        cJSON_free(text);
    }
}

static void load_requests(void)
{
    char *text = malloc(2048);
    cJSON *j = text && gw_config_get_str(KEY_REQUESTS, text, 2048) ? cJSON_Parse(text) : NULL;
    free(text);
    int i = 0;
    const cJSON *o;
    cJSON_ArrayForEach(o, j)
    {
        if (i < AUTH_REQUESTS && GW_JSON_STR(o, "name")) {
            strlcpy(s_requests[i].name, GW_JSON_STR(o, "name"), sizeof(s_requests[i].name));
            const cJSON *t = cJSON_GetObjectItem(o, "time");
            s_requests[i].time = cJSON_IsNumber(t) ? (int64_t)t->valuedouble : 0;
            strlcpy(s_requests[i].ip, GW_JSON_STR(o, "ip") ? GW_JSON_STR(o, "ip") : "", sizeof(s_requests[i].ip));
            i++;
        }
    }
    cJSON_Delete(j);
}

static void set_password(user_t *u, const char *password)
{
    esp_fill_random(u->salt, sizeof(u->salt));
    pw_hash(password, u->salt, u->hash);
}

static bool password_ok(const user_t *u, const char *password)
{
    uint8_t h[32];
    pw_hash(password ? password : "", u->salt, h);
    return equal_ct(h, u->hash, 32);
}

/* drops sessions and cached Basic logins of user i (password change, disable, delete) */
static void drop_user_sessions(int i, int keep)
{
    bool changed = false;
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        if (s_sessions[s].user == i && s != keep && s_sessions[s].user >= 0) {
            memset(&s_sessions[s], 0, sizeof(s_sessions[s]));
            s_sessions[s].user = -1;
            changed = true;
        }
    }
    for (int b = 0; b < BASIC_CACHE; b++) {
        if (s_basic_cache[b].user == i) {
            s_basic_cache[b].until_us = 0;
        }
    }
    if (changed) {
        save_sessions();
    }
}

void gw_auth_init(void)
{
    s_lock = xSemaphoreCreateRecursiveMutex();
    for (int i = 0; i < AUTH_USERS; i++) {
        load_user(i);
    }
    load_roles();
    load_policy();
    load_tokens();
    load_requests();
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        s_sessions[s].user = -1;
    }
    if (gw_config_get_blob(KEY_SESSIONS, s_sessions, sizeof(s_sessions))) {
        int64_t now = esp_timer_get_time();
        for (int s = 0; s < AUTH_SESSIONS; s++) {
            if (s_sessions[s].user < 0 || s_sessions[s].user >= AUTH_USERS || !s_users[s_sessions[s].user].used) {
                memset(&s_sessions[s], 0, sizeof(s_sessions[s]));
                s_sessions[s].user = -1;
            }
            s_session_seen_us[s] = now;
        }
    }
    /* the single admin password of older versions becomes the account "admin" */
    char old[65];
    if (user_count() == 0 && gw_config_get_str(GW_KEY_PASSWORD, old, sizeof(old)) && old[0]) {
        user_t *u = &s_users[0];
        memset(u, 0, sizeof(*u));
        memset(u->levels, 0xff, sizeof(u->levels));
        u->used = true;
        strlcpy(u->name, "admin", sizeof(u->name));
        u->role = ROLE_ADMIN;
        gw_tg_prefs_default(&u->tg);
        u->created = now_unix();
        set_password(u, old);
        save_user(0);
        ESP_LOGW(TAG, "password of older versions moved to the account admin");
    }
    gw_config_erase(GW_KEY_PASSWORD);
    ESP_LOGI(TAG, "%d users, %s", user_count(), user_count() ? "login required" : "open (no users)");
}

bool gw_auth_enabled(void)
{
    return user_count() > 0;
}

/* ---- request identity ---- */

/* URI -> page and the level each method needs. "*" any signed-in client, "admin" admins only,
 * "self" every signed-in user also before a forced password change or 2FA setup. */
typedef struct {
    const char *uri;
    int method; /* -1 any */
    const char *page;
    uint8_t level;
} auth_rule_t;

static const auth_rule_t AUTH_MAP[] = {
    {"/api/status", -1, "*", 1},
    {"/api/ha/discover", -1, "*", 1},
    {"/api/sound/classes", -1, "*", 1},
    {"/api/identify", -1, "overview", 2},
    {"/api/role", -1, "role", 2},
    {"/api/thread/status", -1, "thread/info", 1},
    {"/api/thread/topology", -1, "thread/topology", 1},
    {"/api/thread/cli", -1, "thread/diag", 2},
    {"/api/thread/dataset", -1, "thread/settings", 2}, /* GET too: the dataset holds the network key */
    {"/api/thread/txpower", -1, "thread/settings", 2},
    {"/api/zigbee/status", -1, "zigbee/info", 1},
    {"/api/zigbee/reset", -1, "zigbee/diag", 2},
    {"/api/zigbee/command", -1, "zigbee/diag", 2},
    {"/api/zigbee/net", HTTP_GET, "zigbee/network", 1},
    {"/api/zigbee/net/base", -1, "zigbee/network", 2},
    {"/api/zigbee/net/map", -1, "zigbee/network", 2},
    {"/api/zigbee/net/request", -1, "zigbee/network", 2},
    {"/api/c6/zigbee-snapshot", -1, "zigbee/backups", 2}, /* holds the network key */
    {"/api/sound/status", -1, "sound/live", 1},
    {"/api/sound/scores", -1, "sound/live", 1},
    {"/api/sound/clip", -1, "sound/live", 2}, /* a recording of the room */
    {"/api/sound/config", -1, "sound/live", 2},
    {"/api/sound/events", HTTP_GET, "sound/events", 1},
    {"/api/sound/events", HTTP_PUT, "sound/events", 2},
    {"/api/sound/events/preset", -1, "sound/events", 2},
    {"/api/sound/log", -1, "sound/log", 1},
    {"/api/sound/learn", HTTP_GET, "sound/learn", 1},
    {"/api/sound/learn", HTTP_DELETE, "sound/learn", 2},
    {"/api/sound/learn/record", -1, "sound/learn", 2},
    {"/api/sound/learn/sample", -1, "sound/learn", 2}, /* recordings */
    {"/api/sound/learn/upload", -1, "sound/learn", 2},
    {"/api/mqtt", HTTP_GET, "ha/mqtt", 1},
    {"/api/mqtt", HTTP_POST, "ha/mqtt", 2},
    {"/api/net/lan", HTTP_GET, "system/lan", 1},
    {"/api/net/lan", HTTP_POST, "system/lan", 2},
    {"/api/net/lan/confirm", -1, "system/lan", 2},
    {"/api/net/traffic", -1, "system/lan", 1},
    {"/api/net/scan", HTTP_GET, "system/netscan", 1},
    {"/api/net/scan", HTTP_POST, "system/netscan", 2},
    {"/api/ota/gateway", -1, "system/firmware", 2},
    {"/api/ota/esphome", -1, "system/firmware", 2},
    {"/api/ota/c6fw", -1, "system/firmware", 2},
    {"/api/ota/model", -1, "system/firmware", 2},
    {"/api/update/check", -1, "system/firmware", 2},
    {"/api/update/install", -1, "system/firmware", 2},
    {"/api/logs", -1, "system/diag", 1},
    {"/api/logs/previous", -1, "system/diag", 1},
    {"/api/coredump", HTTP_GET, "system/diag", 1},
    {"/api/coredump", HTTP_DELETE, "system/diag", 2},
    {"/api/tools/ping", -1, "system/diag", 2},
    {"/api/settings", -1, "system/settings", 2},
    {"/api/settings/export", -1, "system/settings", 2}, /* keys of the networks */
    {"/api/settings/import", -1, "system/settings", 2},
    {"/api/reboot", -1, "system/maint", 2},
    {"/api/c6/reflash", -1, "system/maint", 2},
    {"/api/radios/b/probe", -1, "system/maint", 2},
    {"/api/radios/b/flash", -1, "system/maint", 2},
    {"/api/radios/b/console", -1, "system/maint", 2},
    {"/api/radios/thread/forget", -1, "thread/settings", 2},
    {"/api/radios", -1, "system/maint", 1},
    {"/api/c6/backup", -1, "system/maint", 2},
    {"/api/ws", -1, "*", 1},
    {"/api/notify", -1, "*", 1},
    {"/api/auth/me", -1, "self", 1},
    {"/api/auth/logout", -1, "self", 1},
    {"/api/auth/password", -1, "self", 1},
    {"/api/auth/totp/setup", -1, "self", 1},
    {"/api/auth/totp/enable", -1, "self", 1},
    {"/api/auth/totp/disable", -1, "self", 1},
    {"/api/auth/sessions", -1, "self", 1},
    {"/api/auth/sessions/revoke", -1, "self", 1},
    {"/api/auth/telegram", -1, "self", 1},
    {"/api/auth/telegram/pair", -1, "self", 1},
    {"/api/auth/telegram/unpair", -1, "self", 1},
    {"/api/auth/telegram/test", -1, "self", 1},
};

static const auth_rule_t *rule_for(const char *uri, int method)
{
    for (size_t i = 0; i < sizeof(AUTH_MAP) / sizeof(AUTH_MAP[0]); i++) {
        if (strcmp(AUTH_MAP[i].uri, uri) == 0 && (AUTH_MAP[i].method < 0 || AUTH_MAP[i].method == method)) {
            return &AUTH_MAP[i];
        }
    }
    return NULL; /* not listed: admins only */
}

static int session_from_cookie(httpd_req_t *req)
{
    char value[80];
    size_t len = sizeof(value);
    if (httpd_req_get_cookie_val(req, COOKIE, value, &len) != ESP_OK || strlen(value) != 64) {
        return -1;
    }
    uint8_t h[32];
    sha256(value, 64, h);
    int64_t now = esp_timer_get_time(), unix = now_unix();
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        session_t *ss = &s_sessions[s];
        if (ss->user < 0 || !equal_ct(ss->hash, h, 32)) {
            continue;
        }
        bool expired = ss->remember ? (unix && ss->expires && unix > ss->expires)
                                    : now - s_session_seen_us[s] > (int64_t)s_policy.idle_h * 3600 * 1000000LL;
        user_t *u = &s_users[ss->user];
        if (expired || !u->used || u->disabled) {
            memset(ss, 0, sizeof(*ss));
            ss->user = -1;
            save_sessions();
            return -1;
        }
        s_session_seen_us[s] = now;
        return s;
    }
    return -1;
}

static int token_from_header(const char *header)
{
    if (strncmp(header, "Bearer ", 7) != 0) {
        return -1;
    }
    uint8_t h[32];
    sha256(header + 7, strlen(header + 7), h);
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (s_tokens[i].used && equal_ct(s_tokens[i].hash, h, 32)) {
            s_tokens[i].last_used_us = esp_timer_get_time();
            return i;
        }
    }
    return -1;
}

/* HTTP Basic: users without 2FA; a correct pair is cached so polling clients do not pay PBKDF2 */
static int user_from_basic(const char *header)
{
    unsigned char decoded[128];
    size_t n = 0;
    if (strncmp(header, "Basic ", 6) != 0 ||
        mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &n, (const unsigned char *)header + 6, strlen(header + 6))) {
        return -1;
    }
    decoded[n] = '\0';
    uint8_t key[32];
    sha256(decoded, n, key);
    int64_t now = esp_timer_get_time();
    for (int b = 0; b < BASIC_CACHE; b++) {
        if (s_basic_cache[b].until_us > now && equal_ct(s_basic_cache[b].key, key, 32)) {
            user_t *u = &s_users[s_basic_cache[b].user];
            return u->used && !u->disabled && !u->totp ? s_basic_cache[b].user : -1;
        }
    }
    char *sep = strchr((char *)decoded, ':');
    if (!sep) {
        return -1;
    }
    *sep = '\0';
    int i = find_user((char *)decoded);
    if (i < 0 || s_users[i].disabled || s_users[i].totp || s_users[i].locked_until_us > now ||
        !password_ok(&s_users[i], sep + 1)) {
        return -1;
    }
    int slot = 0;
    for (int b = 1; b < BASIC_CACHE; b++) {
        if (s_basic_cache[b].until_us < s_basic_cache[slot].until_us) {
            slot = b;
        }
    }
    memcpy(s_basic_cache[slot].key, key, 32);
    s_basic_cache[slot].user = i;
    s_basic_cache[slot].until_us = now + BASIC_CACHE_US;
    return i;
}

static bool setup_pending(const user_t *u)
{
    return u->must_change || (s_policy.totp_role[u->role] && !u->totp);
}

bool gw_auth_check(httpd_req_t *req, const char *uri)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    s_cur.user = s_cur.token = s_cur.session = -1;
    s_cur.open = user_count() == 0;
    s_cur.role = ROLE_ADMIN;
    const auth_rule_t *rule = rule_for(uri, req->method);
    if (s_cur.open) {
        xSemaphoreGiveRecursive(s_lock);
        return true;
    }
    char header[200];
    if (httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) == ESP_OK) {
        s_cur.token = token_from_header(header);
        if (s_cur.token < 0) {
            s_cur.user = user_from_basic(header);
        }
    }
    if (s_cur.token < 0 && s_cur.user < 0) {
        s_cur.session = session_from_cookie(req);
        s_cur.user = s_cur.session >= 0 ? s_sessions[s_cur.session].user : -1;
    }
    if (s_cur.token < 0 && s_cur.user < 0) {
        xSemaphoreGiveRecursive(s_lock);
        gw_http_send_error(req, 401, "unauthorized");
        return false;
    }
    const user_t *u = s_cur.user >= 0 ? &s_users[s_cur.user] : NULL;
    s_cur.role = u ? u->role : s_tokens[s_cur.token].role;
    const char *page = rule ? rule->page : "admin";
    bool ok;
    if (strcmp(page, "self") == 0) {
        ok = true; /* the handlers tell users from tokens */
    } else if (u && setup_pending(u)) {
        xSemaphoreGiveRecursive(s_lock);
        gw_http_send_error(req, 403, u->must_change ? "password_change_required" : "totp_setup_required");
        return false;
    } else if (strcmp(page, "*") == 0) {
        ok = true;
    } else if (strcmp(page, "admin") == 0) {
        ok = s_cur.role == ROLE_ADMIN;
    } else {
        int p = page_index(page);
        uint8_t need = rule->method < 0 && rule->level == 1 && req->method != HTTP_GET ? 2 : rule->level;
        ok = p >= 0 && level_of(s_cur.role, u, p) >= need;
    }
    xSemaphoreGiveRecursive(s_lock);
    if (!ok) {
        gw_http_send_error(req, 403, "forbidden");
    }
    return ok;
}

const char *gw_auth_who(void)
{
    if (s_cur.open) {
        return "";
    }
    return s_cur.user >= 0 ? s_users[s_cur.user].name : s_cur.token >= 0 ? s_tokens[s_cur.token].name : "";
}

/* ---- QR code of the authenticator URI, as SVG ---- */

static char *s_qr_svg;
static size_t s_qr_len;

static void qr_draw(esp_qrcode_handle_t qr)
{
    int size = esp_qrcode_get_size(qr);
    size_t cap = 256 + (size_t)size * size * 22;
    s_qr_svg = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM);
    if (!s_qr_svg) {
        return;
    }
    size_t n = snprintf(s_qr_svg, cap,
                        "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"-4 -4 %d %d\" shape-rendering=\"crispEdges\">"
                        "<rect x=\"-4\" y=\"-4\" width=\"%d\" height=\"%d\" fill=\"#fff\"/><path fill=\"#000\" d=\"",
                        size + 8, size + 8, size + 8, size + 8);
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            if (esp_qrcode_get_module(qr, x, y) && n + 24 < cap) {
                n += snprintf(s_qr_svg + n, cap - n, "M%d %dh1v1h-1z", x, y);
            }
        }
    }
    snprintf(s_qr_svg + n, cap - n, "\"/></svg>");
    s_qr_len = strlen(s_qr_svg);
}

static char *qr_svg(const char *text)
{
    s_qr_svg = NULL;
    esp_qrcode_config_t cfg = {.display_func = qr_draw, .max_qrcode_version = 10, .qrcode_ecc_level = ESP_QRCODE_ECC_MED};
    esp_qrcode_generate(&cfg, text);
    return s_qr_svg;
}

/* ---- API: sign-in, own account ---- */

static void client_ip(httpd_req_t *req, char *out, size_t len)
{
    struct sockaddr_in6 addr;
    socklen_t alen = sizeof(addr);
    out[0] = '\0';
    if (getpeername(httpd_req_to_sockfd(req), (struct sockaddr *)&addr, &alen) == 0) {
        if (IN6_IS_ADDR_V4MAPPED(&addr.sin6_addr)) {
            inet_ntop(AF_INET, &addr.sin6_addr.un.u32_addr[3], out, len);
        } else {
            inet_ntop(AF_INET6, &addr.sin6_addr, out, len);
        }
    }
}

static cJSON *me_json(int i)
{
    user_t *u = &s_users[i];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", u->name);
    cJSON_AddStringToObject(j, "role", s_role_names[u->role]);
    cJSON_AddBoolToObject(j, "must_change", u->must_change);
    cJSON_AddBoolToObject(j, "totp", u->totp);
    cJSON_AddBoolToObject(j, "totp_required", s_policy.totp_role[u->role]);
    cJSON_AddNumberToObject(j, "recovery_left", u->totp ? recovery_left(u) : 0);
    uint8_t levels[PAGES];
    for (size_t p = 0; p < PAGES; p++) {
        levels[p] = level_of(u->role, u, p);
    }
    cJSON_AddItemToObject(j, "levels", levels_json(levels));
    return j;
}

static esp_err_t send_locked(httpd_req_t *req, cJSON *json)
{
    xSemaphoreGiveRecursive(s_lock);
    return gw_http_send_json(req, json);
}

static esp_err_t error_locked(httpd_req_t *req, int status, const char *msg)
{
    xSemaphoreGiveRecursive(s_lock);
    return gw_http_send_error(req, status, msg);
}

static esp_err_t login_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char name[32], password[72], code[16];
    strlcpy(name, GW_JSON_STR(body, "user") ? GW_JSON_STR(body, "user") : "", sizeof(name));
    strlcpy(password, GW_JSON_STR(body, "password") ? GW_JSON_STR(body, "password") : "", sizeof(password));
    strlcpy(code, GW_JSON_STR(body, "code") ? GW_JSON_STR(body, "code") : "", sizeof(code));
    bool remember = cJSON_IsTrue(cJSON_GetObjectItem(body, "remember"));
    cJSON_Delete(body);

    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (user_count() == 0) {
        return error_locked(req, 409, "no users: the gateway is open");
    }
    int i = find_user(name);
    int64_t now = esp_timer_get_time();
    if (i < 0) {
        user_t dummy = {0};
        password_ok(&dummy, password); /* same time as a known name */
        return error_locked(req, 401, "invalid user or password");
    }
    user_t *u = &s_users[i];
    if (u->locked_until_us > now) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "error", "locked");
        cJSON_AddNumberToObject(j, "retry_s", (int)((u->locked_until_us - now) / 1000000) + 1);
        httpd_resp_set_status(req, "403 Forbidden");
        return send_locked(req, j);
    }
    bool ok = password_ok(u, password);
    if (ok && u->totp && !code[0]) {
        cJSON *j = cJSON_CreateObject();
        cJSON_AddBoolToObject(j, "code_required", true);
        cJSON_AddBoolToObject(j, "clock_set", now_unix() != 0);
        return send_locked(req, j);
    }
    if (ok && u->totp) {
        ok = check_code(u, u->secret, code, true);
        if (ok && recovery_left(u) < RECOVERY_CODES) {
            save_user(i); /* a recovery code was used up */
        }
    }
    if (!ok) {
        if (++u->fails >= s_policy.lock_n) {
            u->fails = 0;
            u->locked_until_us = now + (int64_t)s_policy.lock_min * 60 * 1000000LL;
            ESP_LOGW(TAG, "user %s locked for %d min after failed sign-ins", u->name, s_policy.lock_min);
            gw_notify("security", GW_NOTIFY_WARNING, "Account %s locked for %d min after %d failed sign-ins", u->name,
                      s_policy.lock_min, s_policy.lock_n);
        }
        return error_locked(req, 401, u->totp && password_ok(u, password) ? "invalid code" : "invalid user or password");
    }
    if (u->disabled) {
        return error_locked(req, 403, "account disabled");
    }
    u->fails = 0;
    u->last_login = now_unix();
    save_user(i);

    /* a new session; the oldest goes when all are taken */
    int slot = 0;
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        if (s_sessions[s].user < 0) {
            slot = s;
            break;
        }
        if (s_session_seen_us[s] < s_session_seen_us[slot]) {
            slot = s;
        }
    }
    uint8_t raw[32];
    char cookie_value[65];
    esp_fill_random(raw, sizeof(raw));
    hex(raw, 32, cookie_value);
    session_t *ss = &s_sessions[slot];
    sha256(cookie_value, 64, ss->hash);
    ss->user = i;
    ss->remember = remember && s_policy.remember_d > 0;
    ss->expires = ss->remember && now_unix() ? now_unix() + (int64_t)s_policy.remember_d * 86400 : 0;
    s_session_seen_us[slot] = now;
    save_sessions();

    static char set_cookie[160];
    snprintf(set_cookie, sizeof(set_cookie), COOKIE "=%s; Path=/; HttpOnly; SameSite=Strict%s", cookie_value,
             ss->remember ? "; Max-Age=31536000" : "");
    httpd_resp_set_hdr(req, "Set-Cookie", set_cookie);
    char ip[48];
    client_ip(req, ip, sizeof(ip));
    ESP_LOGI(TAG, "%s signed in from %s", u->name, ip);
    return send_locked(req, me_json(i));
}

static esp_err_t logout_post(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_cur.session >= 0) {
        memset(&s_sessions[s_cur.session], 0, sizeof(session_t));
        s_sessions[s_cur.session].user = -1;
        save_sessions();
    }
    xSemaphoreGiveRecursive(s_lock);
    httpd_resp_set_hdr(req, "Set-Cookie", COOKIE "=; Path=/; HttpOnly; SameSite=Strict; Max-Age=0");
    return gw_http_send_json(req, cJSON_CreateObject());
}

/* State for the login page, without signing in. */
static esp_err_t auth_state_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", gw_auth_enabled());
    cJSON_AddBoolToObject(j, "clock_set", now_unix() != 0);
    cJSON_AddStringToObject(j, "hostname", gw_state()->hostname);
    return gw_http_send_json(req, j);
}

static esp_err_t me_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j;
    if (s_cur.open) {
        j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", "");
        cJSON_AddStringToObject(j, "role", "admin");
        cJSON_AddBoolToObject(j, "open", true);
    } else if (s_cur.user >= 0) {
        j = me_json(s_cur.user);
    } else {
        j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", s_tokens[s_cur.token].name);
        cJSON_AddStringToObject(j, "role", s_role_names[s_tokens[s_cur.token].role]);
        cJSON_AddBoolToObject(j, "token", true);
    }
    if (s_cur.user < 0) {
        uint8_t levels[PAGES];
        for (size_t p = 0; p < PAGES; p++) {
            levels[p] = level_of(s_cur.role, NULL, p);
        }
        cJSON_AddItemToObject(j, "levels", levels_json(levels));
    }
    cJSON_AddNumberToObject(j, "requests", s_cur.role == ROLE_ADMIN ? gw_auth_request_count() : 0);
    return send_locked(req, j);
}

static const char *password_problem(const char *password)
{
    static char msg[64];
    size_t len = password ? strlen(password) : 0;
    if (len < (size_t)s_policy.pw_min || len > 64) {
        snprintf(msg, sizeof(msg), "password: %d to 64 characters", s_policy.pw_min);
        return msg;
    }
    return NULL;
}

static esp_err_t password_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_cur.user < 0) {
        cJSON_Delete(body);
        return error_locked(req, 409, "only for signed-in users");
    }
    user_t *u = &s_users[s_cur.user];
    const char *old = GW_JSON_STR(body, "old"), *new = GW_JSON_STR(body, "new");
    const char *problem = password_problem(new);
    esp_err_t res;
    if (!password_ok(u, old)) {
        res = error_locked(req, 403, "current password is wrong");
    } else if (problem) {
        res = error_locked(req, 400, problem);
    } else {
        set_password(u, new);
        u->must_change = false;
        save_user(s_cur.user);
        drop_user_sessions(s_cur.user, s_cur.session);
        gw_notify("security", GW_NOTIFY_INFO, "User %s changed the password", u->name);
        res = send_locked(req, me_json(s_cur.user));
    }
    cJSON_Delete(body);
    return res;
}

static esp_err_t totp_setup_post(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_cur.user < 0) {
        return error_locked(req, 409, "only for signed-in users");
    }
    user_t *u = &s_users[s_cur.user];
    esp_fill_random(u->pending, sizeof(u->pending));
    u->has_pending = true;
    char secret[40], uri[200];
    base32(u->pending, 20, secret);
    snprintf(uri, sizeof(uri), "otpauth://totp/P4%%20Gateway:%s@%s?secret=%s&issuer=P4%%20Gateway&digits=6&period=30",
             u->name, gw_state()->hostname, secret);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "secret", secret);
    cJSON_AddStringToObject(j, "uri", uri);
    char *svg = qr_svg(uri);
    if (svg) {
        cJSON_AddStringToObject(j, "qr_svg", svg);
        free(svg);
    }
    cJSON_AddBoolToObject(j, "clock_set", now_unix() != 0);
    return send_locked(req, j);
}

static esp_err_t totp_enable_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t res;
    user_t *u = s_cur.user >= 0 ? &s_users[s_cur.user] : NULL;
    if (!u || !u->has_pending) {
        res = error_locked(req, 409, "start the setup first");
    } else if (!now_unix()) {
        res = error_locked(req, 409, "the gateway clock is not set (no SNTP): codes cannot be checked");
    } else if (!check_code(u, u->pending, GW_JSON_STR(body, "code"), false)) {
        res = error_locked(req, 400, "invalid code");
    } else {
        memcpy(u->secret, u->pending, 20);
        u->totp = true;
        u->has_pending = false;
        cJSON *j = cJSON_CreateObject();
        cJSON *codes = cJSON_AddArrayToObject(j, "recovery_codes");
        static const char alphabet[] = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
        for (int k = 0; k < RECOVERY_CODES; k++) {
            char c[10];
            uint8_t raw[8], h[32];
            esp_fill_random(raw, sizeof(raw));
            for (int m = 0; m < 8; m++) {
                c[m] = alphabet[raw[m] & 31];
            }
            sha256(c, 8, h);
            memcpy(u->recovery[k], h, 16);
            char shown[10] = {c[0], c[1], c[2], c[3], '-', c[4], c[5], c[6], c[7], 0};
            cJSON_AddItemToArray(codes, cJSON_CreateString(shown));
        }
        save_user(s_cur.user);
        drop_user_sessions(s_cur.user, s_cur.session);
        gw_notify("security", GW_NOTIFY_INFO, "User %s turned on two-factor sign-in", u->name);
        res = send_locked(req, j);
    }
    cJSON_Delete(body);
    return res;
}

static esp_err_t totp_disable_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    esp_err_t res;
    user_t *u = s_cur.user >= 0 ? &s_users[s_cur.user] : NULL;
    if (!u || !u->totp) {
        res = error_locked(req, 409, "two-factor sign-in is not on");
    } else if (s_policy.totp_role[u->role]) {
        res = error_locked(req, 409, "required for your role");
    } else if (!password_ok(u, GW_JSON_STR(body, "password"))) {
        res = error_locked(req, 403, "current password is wrong");
    } else {
        u->totp = false;
        memset(u->secret, 0, sizeof(u->secret));
        memset(u->recovery, 0, sizeof(u->recovery));
        save_user(s_cur.user);
        gw_notify("security", GW_NOTIFY_WARNING, "User %s turned off two-factor sign-in", u->name);
        res = send_locked(req, me_json(s_cur.user));
    }
    cJSON_Delete(body);
    return res;
}

static esp_err_t sessions_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        if (s_sessions[s].user >= 0 && s_sessions[s].user == s_cur.user) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "id", s);
            cJSON_AddBoolToObject(o, "current", s == s_cur.session);
            cJSON_AddBoolToObject(o, "remember", s_sessions[s].remember);
            cJSON_AddNumberToObject(o, "idle_s", (int)((now - s_session_seen_us[s]) / 1000000));
            cJSON_AddItemToArray(j, o);
        }
    }
    return send_locked(req, j);
}

static esp_err_t sessions_revoke_post(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    if (s_cur.user >= 0) {
        drop_user_sessions(s_cur.user, s_cur.session);
    }
    return send_locked(req, cJSON_CreateObject());
}

/* Forgotten password: anyone can ask, the admins see it in the web UI and get notified. */
static esp_err_t reset_request_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    char name[32];
    strlcpy(name, GW_JSON_STR(body, "user") ? GW_JSON_STR(body, "user") : "", sizeof(name));
    cJSON_Delete(body);
    static int64_t last_us[4];
    static int next;
    int64_t now = esp_timer_get_time();
    /* at most 4 requests per 10 minutes, whoever asks */
    if (last_us[next] && now - last_us[next] < 600 * 1000000LL) {
        return gw_http_send_error(req, 409, "too many requests, try again later");
    }
    last_us[next] = now;
    next = (next + 1) % 4;
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    int i = find_user(name);
    if (i >= 0) {
        int slot = -1;
        for (int r = 0; r < AUTH_REQUESTS; r++) {
            if (strcmp(s_requests[r].name, name) == 0 || (slot < 0 && !s_requests[r].name[0])) {
                slot = r;
                if (strcmp(s_requests[r].name, name) == 0) {
                    break;
                }
            }
        }
        if (slot >= 0) {
            strlcpy(s_requests[slot].name, name, sizeof(s_requests[slot].name));
            s_requests[slot].time = now_unix();
            client_ip(req, s_requests[slot].ip, sizeof(s_requests[slot].ip));
            save_requests();
            gw_notify("security", GW_NOTIFY_WARNING, "Password reset requested for %s from %s", name,
                      s_requests[slot].ip);
        }
    }
    /* the same answer for unknown names */
    return send_locked(req, cJSON_CreateObject());
}

int gw_auth_request_count(void)
{
    int n = 0;
    for (int r = 0; r < AUTH_REQUESTS; r++) {
        n += s_requests[r].name[0] != 0;
    }
    return n;
}

/* ---- API: administration ---- */

static cJSON *user_json(int i)
{
    user_t *u = &s_users[i];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "name", u->name);
    cJSON_AddStringToObject(j, "role", s_role_names[u->role]);
    cJSON_AddBoolToObject(j, "disabled", u->disabled);
    cJSON_AddBoolToObject(j, "must_change", u->must_change);
    cJSON_AddBoolToObject(j, "totp", u->totp);
    cJSON_AddNumberToObject(j, "recovery_left", u->totp ? recovery_left(u) : 0);
    cJSON_AddBoolToObject(j, "locked", u->locked_until_us > esp_timer_get_time());
    cJSON_AddNumberToObject(j, "created", (double)u->created);
    cJSON_AddNumberToObject(j, "last_login", (double)u->last_login);
    int sessions = 0;
    for (int s = 0; s < AUTH_SESSIONS; s++) {
        sessions += s_sessions[s].user == i;
    }
    cJSON_AddNumberToObject(j, "sessions", sessions);
    cJSON *lv = cJSON_AddObjectToObject(j, "levels");
    for (size_t p = 0; p < PAGES; p++) {
        if (u->levels[p] != 0xff) {
            cJSON_AddNumberToObject(lv, s_pages[p], u->levels[p]);
        }
    }
    return j;
}

static esp_err_t users_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(j, "users");
    for (int i = 0; i < AUTH_USERS; i++) {
        if (s_users[i].used) {
            cJSON_AddItemToArray(list, user_json(i));
        }
    }
    cJSON *pages = cJSON_AddArrayToObject(j, "pages");
    for (size_t p = 0; p < PAGES; p++) {
        cJSON_AddItemToArray(pages, cJSON_CreateString(s_pages[p]));
    }
    cJSON_AddNumberToObject(j, "max", AUTH_USERS);
    return send_locked(req, j);
}

/* Creates a user ({"name", "role", "password"}; the user changes it at the first sign-in unless
 * "must_change": false) or changes one ({"name", "role", "disabled", "password", "must_change",
 * "totp_reset", "unlock", "levels": {page: level or -1 for the role's}}). */
static esp_err_t users_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const char *name = GW_JSON_STR(body, "name"), *password = GW_JSON_STR(body, "password");
    const char *role_name = GW_JSON_STR(body, "role");
    int role = role_name ? role_from_name(role_name) : -1;
    int i = find_user(name);
    bool create = i < 0;
    esp_err_t res = ESP_OK;
    const char *problem = NULL;
    if (create) {
        if (!name_valid(name)) {
            problem = "name: 2 to 31 characters a-z, 0-9, . _ -";
        } else if (role < 0) {
            problem = "role: admin, user or viewer";
        } else if ((problem = password_problem(password))) {
        } else if (user_count() == 0 && role != ROLE_ADMIN) {
            problem = "the first account must be an admin";
        } else {
            for (i = 0; i < AUTH_USERS && s_users[i].used; i++) {
            }
            if (i == AUTH_USERS) {
                problem = "no room for another user";
            }
        }
        if (problem) {
            res = error_locked(req, 400, problem);
            cJSON_Delete(body);
            return res;
        }
        user_t *u = &s_users[i];
        memset(u, 0, sizeof(*u));
        memset(u->levels, 0xff, sizeof(u->levels));
        u->used = true;
        strlcpy(u->name, name, sizeof(u->name));
        u->role = role;
        gw_tg_prefs_default(&u->tg);
        /* the first account is the one creating it: no forced change */
        const cJSON *mc = cJSON_GetObjectItem(body, "must_change");
        u->must_change = user_count() > 1 && !cJSON_IsFalse(mc);
        u->created = now_unix();
        set_password(u, password);
        save_user(i);
        gw_notify("security", GW_NOTIFY_INFO, "User %s (%s) created by %s", u->name, s_role_names[role],
                  gw_auth_who()[0] ? gw_auth_who() : "the first sign-up");
        res = send_locked(req, user_json(i));
        cJSON_Delete(body);
        return res;
    }

    user_t *u = &s_users[i];
    const cJSON *v;
    bool demote = role >= 0 && role != ROLE_ADMIN && u->role == ROLE_ADMIN;
    bool disable = cJSON_IsTrue(cJSON_GetObjectItem(body, "disabled")) && !u->disabled;
    if (role_name && role < 0) {
        problem = "role: admin, user or viewer";
    } else if ((demote || disable) && u->role == ROLE_ADMIN && !u->disabled && admin_count() <= 1) {
        problem = "the last admin cannot lose the role or be disabled";
    } else if (password && (problem = password_problem(password))) {
    }
    if (problem) {
        res = error_locked(req, 400, problem);
        cJSON_Delete(body);
        return res;
    }
    if (role >= 0) {
        u->role = role;
    }
    if (cJSON_IsBool(v = cJSON_GetObjectItem(body, "disabled"))) {
        u->disabled = cJSON_IsTrue(v);
        if (u->disabled) {
            drop_user_sessions(i, -1);
        }
    }
    if (password) {
        set_password(u, password);
        u->must_change = !cJSON_IsFalse(cJSON_GetObjectItem(body, "must_change"));
        drop_user_sessions(i, -1);
        /* a reset request of this user is answered */
        for (int r = 0; r < AUTH_REQUESTS; r++) {
            if (strcmp(s_requests[r].name, u->name) == 0) {
                memset(&s_requests[r], 0, sizeof(s_requests[r]));
                save_requests();
            }
        }
    } else if (cJSON_IsBool(v = cJSON_GetObjectItem(body, "must_change"))) {
        u->must_change = cJSON_IsTrue(v);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "totp_reset"))) {
        u->totp = false;
        memset(u->secret, 0, sizeof(u->secret));
        memset(u->recovery, 0, sizeof(u->recovery));
        drop_user_sessions(i, -1);
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "unlock"))) {
        u->locked_until_us = 0;
        u->fails = 0;
    }
    if ((v = cJSON_GetObjectItem(body, "levels"))) {
        const cJSON *lv;
        cJSON_ArrayForEach(lv, v)
        {
            int p = page_index(lv->string);
            if (p >= 0 && cJSON_IsNumber(lv)) {
                u->levels[p] = lv->valueint >= 0 && lv->valueint <= 2 ? lv->valueint : 0xff;
            }
        }
    }
    save_user(i);
    gw_notify("security", GW_NOTIFY_INFO, "User %s changed by %s", u->name, gw_auth_who());
    res = send_locked(req, user_json(i));
    cJSON_Delete(body);
    return res;
}

static esp_err_t users_delete_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    int i = find_user(GW_JSON_STR(body, "name"));
    cJSON_Delete(body);
    if (i < 0) {
        return error_locked(req, 404, "no such user");
    }
    user_t *u = &s_users[i];
    if (u->role == ROLE_ADMIN && !u->disabled && admin_count() <= 1 && user_count() > 1) {
        return error_locked(req, 400, "the last admin cannot be deleted while other users exist");
    }
    char name[32];
    strlcpy(name, u->name, sizeof(name));
    drop_user_sessions(i, -1);
    memset(u, 0, sizeof(*u));
    save_user(i);
    gw_notify("security", GW_NOTIFY_WARNING, "User %s deleted by %s", name, gw_auth_who());
    return send_locked(req, cJSON_CreateObject());
}

static esp_err_t roles_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "user", levels_json(s_role_levels[0]));
    cJSON_AddItemToObject(j, "viewer", levels_json(s_role_levels[1]));
    cJSON *d = cJSON_AddObjectToObject(j, "defaults");
    cJSON_AddItemToObject(d, "user", levels_json(s_default_levels[0]));
    cJSON_AddItemToObject(d, "viewer", levels_json(s_default_levels[1]));
    return send_locked(req, j);
}

static esp_err_t roles_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    parse_levels(cJSON_GetObjectItem(body, "user"), s_role_levels[0]);
    parse_levels(cJSON_GetObjectItem(body, "viewer"), s_role_levels[1]);
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "reset"))) {
        memcpy(s_role_levels, s_default_levels, sizeof(s_role_levels));
    }
    cJSON_Delete(body);
    save_roles();
    gw_notify("security", GW_NOTIFY_INFO, "Role permissions changed by %s", gw_auth_who());
    xSemaphoreGiveRecursive(s_lock);
    return roles_get(req);
}

static esp_err_t policy_get(httpd_req_t *req)
{
    return gw_http_send_json(req, policy_json());
}

static esp_err_t policy_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    policy_t before = s_policy;
    const char *bad = policy_apply(body);
    cJSON_Delete(body);
    if (bad) {
        s_policy = before;
        return error_locked(req, 400, bad);
    }
    save_policy();
    xSemaphoreGiveRecursive(s_lock);
    return gw_http_send_json(req, policy_json());
}

static esp_err_t requests_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateArray();
    for (int r = 0; r < AUTH_REQUESTS; r++) {
        if (s_requests[r].name[0]) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", s_requests[r].name);
            cJSON_AddNumberToObject(o, "time", (double)s_requests[r].time);
            cJSON_AddStringToObject(o, "ip", s_requests[r].ip);
            cJSON_AddItemToArray(j, o);
        }
    }
    return send_locked(req, j);
}

static esp_err_t requests_dismiss_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const char *name = GW_JSON_STR(body, "name");
    for (int r = 0; r < AUTH_REQUESTS; r++) {
        if (name && strcmp(s_requests[r].name, name) == 0) {
            memset(&s_requests[r], 0, sizeof(s_requests[r]));
        }
    }
    cJSON_Delete(body);
    save_requests();
    xSemaphoreGiveRecursive(s_lock);
    return requests_get(req);
}

static esp_err_t tokens_get(httpd_req_t *req)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    cJSON *j = cJSON_CreateArray();
    int64_t now = esp_timer_get_time();
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (s_tokens[i].used) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "name", s_tokens[i].name);
            cJSON_AddStringToObject(o, "role", s_role_names[s_tokens[i].role]);
            cJSON_AddNumberToObject(o, "created", (double)s_tokens[i].created);
            cJSON_AddNumberToObject(o, "used_s_ago",
                                    s_tokens[i].last_used_us ? (int)((now - s_tokens[i].last_used_us) / 1000000) : -1);
            cJSON_AddItemToArray(j, o);
        }
    }
    return send_locked(req, j);
}

/* {"name", "role"} -> {"token"}: shown only now, only its hash is kept */
static esp_err_t tokens_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const char *name = GW_JSON_STR(body, "name");
    int role = role_from_name(GW_JSON_STR(body, "role"));
    int slot = -1;
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (s_tokens[i].used && name && strcmp(s_tokens[i].name, name) == 0) {
            slot = -2;
            break;
        }
        if (!s_tokens[i].used && slot == -1) {
            slot = i;
        }
    }
    esp_err_t res;
    if (!name || !name[0] || strlen(name) > 31 || role < 0) {
        res = error_locked(req, 400, "name (max 31 characters) and role admin, user or viewer");
    } else if (slot == -2) {
        res = error_locked(req, 409, "a token with this name exists");
    } else if (slot < 0) {
        res = error_locked(req, 409, "no room for another token");
    } else {
        token_t *t = &s_tokens[slot];
        uint8_t raw[20];
        char value[48] = "p4gw_";
        esp_fill_random(raw, sizeof(raw));
        hex(raw, 20, value + 5);
        memset(t, 0, sizeof(*t));
        t->used = true;
        t->role = role;
        strlcpy(t->name, name, sizeof(t->name));
        t->created = now_unix();
        sha256(value, strlen(value), t->hash);
        save_tokens();
        gw_notify("security", GW_NOTIFY_INFO, "API token %s (%s) created by %s", t->name, s_role_names[role],
                  gw_auth_who());
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", t->name);
        cJSON_AddStringToObject(j, "token", value);
        res = send_locked(req, j);
    }
    cJSON_Delete(body);
    return res;
}

static esp_err_t tokens_delete_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
    const char *name = GW_JSON_STR(body, "name");
    bool found = false;
    for (int i = 0; i < AUTH_TOKENS; i++) {
        if (s_tokens[i].used && name && strcmp(s_tokens[i].name, name) == 0) {
            memset(&s_tokens[i], 0, sizeof(s_tokens[i]));
            found = true;
        }
    }
    if (found) {
        save_tokens();
        gw_notify("security", GW_NOTIFY_INFO, "API token %s revoked by %s", name, gw_auth_who());
    }
    cJSON_Delete(body);
    xSemaphoreGiveRecursive(s_lock);
    return found ? tokens_get(req) : gw_http_send_error(req, 404, "no such token");
}

void gw_auth_register(void)
{
    gw_http_register_public("/api/auth/state", HTTP_GET, auth_state_get);
    gw_http_register_public("/api/auth/login", HTTP_POST, login_post);
    gw_http_register_public("/api/auth/reset-request", HTTP_POST, reset_request_post);
    gw_http_register("/api/auth/me", HTTP_GET, me_get);
    gw_http_register("/api/auth/logout", HTTP_POST, logout_post);
    gw_http_register("/api/auth/password", HTTP_POST, password_post);
    gw_http_register("/api/auth/totp/setup", HTTP_POST, totp_setup_post);
    gw_http_register("/api/auth/totp/enable", HTTP_POST, totp_enable_post);
    gw_http_register("/api/auth/totp/disable", HTTP_POST, totp_disable_post);
    gw_http_register("/api/auth/sessions", HTTP_GET, sessions_get);
    gw_http_register("/api/auth/sessions/revoke", HTTP_POST, sessions_revoke_post);
    gw_http_register("/api/users", HTTP_GET, users_get);
    gw_http_register("/api/users", HTTP_POST, users_post);
    gw_http_register("/api/users/delete", HTTP_POST, users_delete_post);
    gw_http_register("/api/auth/roles", HTTP_GET, roles_get);
    gw_http_register("/api/auth/roles", HTTP_POST, roles_post);
    gw_http_register("/api/auth/policy", HTTP_GET, policy_get);
    gw_http_register("/api/auth/policy", HTTP_POST, policy_post);
    gw_http_register("/api/auth/requests", HTTP_GET, requests_get);
    gw_http_register("/api/auth/requests/dismiss", HTTP_POST, requests_dismiss_post);
    gw_http_register("/api/tokens", HTTP_GET, tokens_get);
    gw_http_register("/api/tokens", HTTP_POST, tokens_post);
    gw_http_register("/api/tokens/delete", HTTP_POST, tokens_delete_post);
}

/* ---- for the Telegram bot (gw_auth_priv.h) ---- */

void gw_tg_prefs_default(gw_tg_prefs_t *p)
{
    memset(p, 0, sizeof(*p));
    p->levels[GW_TG_SECURITY] = 2; /* warnings and critical */
    p->levels[GW_TG_SYSTEM] = 2;
    p->levels[GW_TG_NETWORK] = 2;
    p->levels[GW_TG_SOUND] = 1; /* critical (alarms, glass) */
    p->quiet_from = p->quiet_to = -1;
    strlcpy(p->lang, "cs", sizeof(p->lang));
}

void gw_auth_lock(void)
{
    xSemaphoreTakeRecursive(s_lock, portMAX_DELAY);
}

void gw_auth_unlock(void)
{
    xSemaphoreGiveRecursive(s_lock);
}

int gw_auth_users_max(void)
{
    return AUTH_USERS;
}

bool gw_auth_user_valid(int i)
{
    return i >= 0 && i < AUTH_USERS && s_users[i].used && !s_users[i].disabled;
}

const char *gw_auth_user_name(int i)
{
    return s_users[i].name;
}

bool gw_auth_user_admin(int i)
{
    return s_users[i].role == ROLE_ADMIN;
}

bool gw_auth_user_totp(int i)
{
    return s_users[i].totp;
}

bool gw_auth_user_code(int i, const char *code)
{
    return s_users[i].totp && check_code(&s_users[i], s_users[i].secret, code, false);
}

/* level of user i on a page; "admin" pages only for admins */
int gw_auth_user_level(int i, const char *page)
{
    if (strcmp(page, "admin") == 0) {
        return s_users[i].role == ROLE_ADMIN ? 2 : 0;
    }
    if (strcmp(page, "*") == 0) {
        return 2;
    }
    int p = page_index(page);
    return p < 0 ? 0 : level_of(s_users[i].role, &s_users[i], p);
}

gw_tg_prefs_t *gw_auth_user_tg(int i)
{
    return &s_users[i].tg;
}

void gw_auth_user_save(int i)
{
    save_user(i);
}

int gw_auth_user_by_chat(int64_t chat)
{
    for (int i = 0; i < AUTH_USERS; i++) {
        if (gw_auth_user_valid(i) && chat && s_users[i].tg.chat == chat) {
            return i;
        }
    }
    return -1;
}

int gw_auth_current_user(void)
{
    return s_cur.user;
}
