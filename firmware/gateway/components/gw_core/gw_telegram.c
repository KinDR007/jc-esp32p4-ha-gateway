#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/param.h>
#include <time.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "gw.h"
#include "gw_auth_priv.h"

static const char *TAG = "gw_telegram";

/* Telegram bot: notifications to the chats paired with user accounts, and commands from them.
 *
 * One task polls getUpdates with a 5 s long-poll over one kept TLS connection and sends what is
 * queued between the polls, so a notification leaves within about 5 s and only one TLS session is
 * open. A chat is paired with a user by a one-time code from My account (/start <code>). Commands
 * run with that user's rights (page and level like the web UI); those that change something ask for
 * a confirmation button and, when the user has two-factor sign-in, a current authenticator code. */

#define KEY_TOKEN "tg_token"
#define KEY_ON "tg_on"
#define KEY_TZ "tz"
#define DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#define POLL_S 5
#define RESP_MAX (24 * 1024)
#define MAX_COMMANDS 24
#define PENDING 4
#define PENDING_US (120 * 1000000LL)
#define PAIR_US (600 * 1000000LL)
#define NOTE_QUEUE 16
#define OUT_QUEUE 16

typedef struct {
    const char *name, *help_en, *help_cs, *page;
    int level;
    bool confirm;
    gw_bot_fn fn;
} command_t;

typedef struct {
    bool used;
    int64_t chat;
    int user;
    int cmd;
    char args[48];
    int64_t expires_us;
    bool want_code;
} pending_t;

typedef struct {
    uint8_t topic, level;
    char text[200];
} note_t;

typedef struct {
    int64_t chat;
    char *text;
} out_t;

static char s_token[64];
static bool s_enabled;
static char s_bot[48];
static char s_state[16] = "off";
static char s_error[120];
static char s_tz[64];
static int64_t s_offset;
static command_t s_cmds[MAX_COMMANDS];
static int s_ncmds;
static pending_t s_pending[PENDING];
static struct {
    uint32_t code;
    int64_t until_us;
} s_pair[16];
static int64_t s_mute_until_us[16]; /* sound notifications muted per user */
static QueueHandle_t s_notes, s_out;
static TaskHandle_t s_task;
static esp_http_client_handle_t s_http;
static char *s_resp;
static int s_resp_len;

static const char *const s_topic_keys[GW_TG_TOPICS] = {"security", "system", "network", "sound"};

static bool cs(int user)
{
    return user < 0 || strcmp(gw_auth_user_tg(user)->lang, "en") != 0;
}
#define T(user, en, czech) (cs(user) ? (czech) : (en))

/* text for HTML parse mode */
static void html(const char *in, char *out, size_t len)
{
    size_t n = 0;
    for (; *in && n + 6 < len; in++) {
        const char *rep = *in == '<' ? "&lt;" : *in == '>' ? "&gt;" : *in == '&' ? "&amp;" : NULL;
        if (rep) {
            n += snprintf(out + n, len - n, "%s", rep);
        } else {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

void gw_bot_command(const char *name, const char *help_en, const char *help_cs, const char *page, int level,
                    bool confirm, gw_bot_fn fn)
{
    if (s_ncmds < MAX_COMMANDS) {
        s_cmds[s_ncmds++] = (command_t){name, help_en, help_cs, page, level, confirm, fn};
    }
}

/* ---- Bot API ---- */

static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && s_resp) {
        int n = MIN(e->data_len, RESP_MAX - 1 - s_resp_len);
        if (n > 0) {
            memcpy(s_resp + s_resp_len, e->data, n);
            s_resp_len += n;
        }
    }
    return ESP_OK;
}

static void http_close(void)
{
    if (s_http) {
        esp_http_client_cleanup(s_http);
        s_http = NULL;
    }
}

/* POST https://api.telegram.org/bot<token>/<method>; body is consumed. Returns "result" (owned by
 * the caller) or NULL with s_error set. */
static cJSON *tg_call(const char *method, cJSON *body, int timeout_s)
{
    char url[160];
    snprintf(url, sizeof(url), "https://api.telegram.org/bot%s/%s", s_token, method);
    if (!s_http) {
        esp_http_client_config_t cfg = {
            .url = url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = (POLL_S + 15) * 1000,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .event_handler = http_event,
            .buffer_size = 2048,
            .buffer_size_tx = 1024,
            .keep_alive_enable = true,
        };
        s_http = esp_http_client_init(&cfg);
        if (!s_http) {
            strlcpy(s_error, "no memory for the HTTP client", sizeof(s_error));
            cJSON_Delete(body);
            return NULL;
        }
    }
    char *payload = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    esp_http_client_set_url(s_http, url);
    esp_http_client_set_method(s_http, HTTP_METHOD_POST);
    esp_http_client_set_timeout_ms(s_http, (timeout_s + 15) * 1000);
    esp_http_client_set_header(s_http, "Content-Type", "application/json");
    esp_http_client_set_post_field(s_http, payload, payload ? strlen(payload) : 0);
    s_resp_len = 0;
    esp_err_t err = esp_http_client_perform(s_http);
    cJSON_free(payload);
    int status = esp_http_client_get_status_code(s_http);
    if (err != ESP_OK) {
        /* on 401 the client looks for an HTTP auth challenge and fails: Telegram refused the token */
        if (status == 401) {
            snprintf(s_error, sizeof(s_error), "%s: invalid bot token (HTTP 401)", method);
        } else {
            snprintf(s_error, sizeof(s_error), "%s: %s", method, esp_err_to_name(err));
        }
        http_close();
        return NULL;
    }
    s_resp[s_resp_len] = '\0';
    cJSON *json = cJSON_Parse(s_resp);
    cJSON *result = cJSON_IsTrue(cJSON_GetObjectItem(json, "ok")) ? cJSON_DetachItemFromObject(json, "result") : NULL;
    if (!result) {
        const char *d = GW_JSON_STR(json, "description");
        snprintf(s_error, sizeof(s_error), "%s: HTTP %d %s", method, status, d ? d : "");
    }
    cJSON_Delete(json);
    return result;
}

static void send_text(int64_t chat, const char *text, cJSON *markup)
{
    cJSON *b = cJSON_CreateObject();
    cJSON_AddNumberToObject(b, "chat_id", (double)chat);
    cJSON_AddStringToObject(b, "text", text);
    cJSON_AddStringToObject(b, "parse_mode", "HTML");
    cJSON_AddBoolToObject(b, "disable_web_page_preview", true);
    if (markup) {
        cJSON_AddItemToObject(b, "reply_markup", markup);
    }
    cJSON_Delete(tg_call("sendMessage", b, 10));
}

static cJSON *buttons(int user, int slot)
{
    char ok[16], no[16];
    snprintf(ok, sizeof(ok), "ok:%d", slot);
    snprintf(no, sizeof(no), "no:%d", slot);
    cJSON *m = cJSON_CreateObject(), *rows = cJSON_AddArrayToObject(m, "inline_keyboard"), *row = cJSON_CreateArray();
    cJSON *b1 = cJSON_CreateObject(), *b2 = cJSON_CreateObject();
    cJSON_AddStringToObject(b1, "text", T(user, "✅ Confirm", "✅ Potvrdit"));
    cJSON_AddStringToObject(b1, "callback_data", ok);
    cJSON_AddStringToObject(b2, "text", T(user, "✖ Cancel", "✖ Zrušit"));
    cJSON_AddStringToObject(b2, "callback_data", no);
    cJSON_AddItemToArray(row, b1);
    cJSON_AddItemToArray(row, b2);
    cJSON_AddItemToArray(rows, row);
    return m;
}

/* ---- commands ---- */

static int find_command(const char *name)
{
    for (int i = 0; i < s_ncmds; i++) {
        if (strcmp(s_cmds[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void help(int user, char *out, size_t len)
{
    size_t n = snprintf(out, len, "%s\n", T(user, "<b>Commands</b>", "<b>Příkazy</b>"));
    for (int i = 0; i < s_ncmds && n < len; i++) {
        if (gw_auth_user_level(user, s_cmds[i].page) >= s_cmds[i].level) {
            n += snprintf(out + n, len - n, "/%s – %s\n", s_cmds[i].name, cs(user) ? s_cmds[i].help_cs : s_cmds[i].help_en);
        }
    }
    snprintf(out + n, len - n, "/mute – %s", T(user, "mute sound notifications (/mute 1h, /mute off)",
                                                  "ztlumit notifikace zvuků (/mute 1h, /mute off)"));
}

static void run(int user, int cmd, const char *args, char *out, size_t len)
{
    out[0] = '\0';
    s_cmds[cmd].fn(args, cs(user), out, len);
    if (!out[0]) {
        strlcpy(out, T(user, "Done.", "Hotovo."), len);
    }
}

static void mute(int user, const char *args, char *out, size_t len)
{
    int64_t now = esp_timer_get_time();
    if (strcmp(args, "off") == 0) {
        s_mute_until_us[user] = 0;
        strlcpy(out, T(user, "Sound notifications on again.", "Notifikace zvuků opět zapnuté."), len);
        return;
    }
    int value = args[0] ? atoi(args) : 1;
    char unit = args[0] ? args[strlen(args) - 1] : 'h';
    int64_t s = value * (unit == 'm' ? 60LL : unit == 'd' ? 86400LL : 3600LL);
    if (value <= 0 || s > 7 * 86400LL) {
        strlcpy(out, T(user, "Use /mute 30m, /mute 2h, /mute 1d or /mute off.", "Použijte /mute 30m, /mute 2h, /mute 1d nebo /mute off."), len);
        return;
    }
    s_mute_until_us[user] = now + s * 1000000LL;
    snprintf(out, len, T(user, "Sound notifications muted for %d min.", "Notifikace zvuků ztlumené na %d min."), (int)(s / 60));
}

static void pair(int64_t chat, const char *code, const char *from)
{
    uint32_t c = strtoul(code, NULL, 10);
    int64_t now = esp_timer_get_time();
    gw_auth_lock();
    int user = -1;
    for (int i = 0; i < gw_auth_users_max() && i < 16; i++) {
        if (c && s_pair[i].code == c && s_pair[i].until_us > now && gw_auth_user_valid(i)) {
            user = i;
        }
    }
    if (user >= 0) {
        int old = gw_auth_user_by_chat(chat);
        if (old >= 0 && old != user) {
            gw_auth_user_tg(old)->chat = 0;
            gw_auth_user_save(old);
        }
        gw_auth_user_tg(user)->chat = chat;
        gw_auth_user_save(user);
        s_pair[user].code = 0;
    }
    gw_auth_unlock();
    char text[200];
    if (user < 0) {
        send_text(chat, "Neplatný nebo prošlý kód. Vytvořte nový ve webovém rozhraní brány: Můj účet › Telegram.\n"
                        "Invalid or expired code. Create a new one in the gateway web UI: My account › Telegram.", NULL);
        return;
    }
    snprintf(text, sizeof(text), T(user, "Paired with the gateway account <b>%s</b>. /help lists the commands.",
                                   "Spárováno s účtem brány <b>%s</b>. /help vypíše příkazy."), gw_auth_user_name(user));
    send_text(chat, text, NULL);
    gw_notify("security", GW_NOTIFY_INFO, "Telegram chat @%s paired with the account %s", from, gw_auth_user_name(user));
}

static void on_message(const cJSON *msg)
{
    const cJSON *chat_j = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "chat"), "id");
    const char *text = GW_JSON_STR(msg, "text");
    const char *from = GW_JSON_STR(cJSON_GetObjectItem(msg, "from"), "username");
    if (!cJSON_IsNumber(chat_j) || !text) {
        return;
    }
    int64_t chat = (int64_t)chat_j->valuedouble;
    char name[24] = "", args[48] = "", out[1500];
    if (text[0] == '/') {
        sscanf(text + 1, "%23[^ @\n] %47[^\n]", name, args);
        char *at = strchr(name, '@');
        if (at) {
            *at = '\0';
        }
    }
    if ((strcmp(name, "start") == 0 || strcmp(name, "pair") == 0) && args[0]) {
        pair(chat, args, from ? from : "?");
        return;
    }
    gw_auth_lock();
    int user = gw_auth_user_by_chat(chat);
    gw_auth_unlock();
    if (user < 0) {
        send_text(chat, "Tento chat není spárovaný s účtem brány. Ve webovém rozhraní otevřete Můj účet › Telegram › Spárovat.\n"
                        "This chat is not paired with a gateway account. In the web UI open My account › Telegram › Pair.", NULL);
        return;
    }
    /* a code for a confirmed command */
    for (int p = 0; p < PENDING; p++) {
        pending_t *pd = &s_pending[p];
        if (pd->used && pd->chat == chat && pd->want_code && !name[0]) {
            pd->used = false;
            gw_auth_lock();
            bool ok = pd->expires_us > esp_timer_get_time() && gw_auth_user_code(user, text);
            gw_auth_unlock();
            if (!ok) {
                send_text(chat, T(user, "Wrong or late code, the command was cancelled.", "Chybný nebo pozdní kód, příkaz zrušen."), NULL);
                return;
            }
            run(user, pd->cmd, pd->args, out, sizeof(out));
            send_text(chat, out, NULL);
            gw_notify("security", GW_NOTIFY_INFO, "Telegram: %s ran /%s %s", gw_auth_user_name(user), s_cmds[pd->cmd].name, pd->args);
            return;
        }
    }
    if (!name[0] || strcmp(name, "help") == 0 || strcmp(name, "start") == 0) {
        help(user, out, sizeof(out));
        send_text(chat, out, NULL);
        return;
    }
    if (strcmp(name, "mute") == 0) {
        mute(user, args, out, sizeof(out));
        send_text(chat, out, NULL);
        return;
    }
    int cmd = find_command(name);
    if (cmd < 0) {
        send_text(chat, T(user, "Unknown command, see /help.", "Neznámý příkaz, viz /help."), NULL);
        return;
    }
    gw_auth_lock();
    bool allowed = gw_auth_user_level(user, s_cmds[cmd].page) >= s_cmds[cmd].level;
    gw_auth_unlock();
    if (!allowed) {
        send_text(chat, T(user, "Your account has no right to this command.", "Váš účet k tomuto příkazu nemá oprávnění."), NULL);
        return;
    }
    if (!s_cmds[cmd].confirm) {
        run(user, cmd, args, out, sizeof(out));
        send_text(chat, out, NULL);
        return;
    }
    int slot = 0;
    for (int p = 0; p < PENDING; p++) {
        if (!s_pending[p].used || s_pending[p].expires_us < s_pending[slot].expires_us) {
            slot = p;
            if (!s_pending[p].used) {
                break;
            }
        }
    }
    s_pending[slot] = (pending_t){.used = true, .chat = chat, .user = user, .cmd = cmd, .expires_us = esp_timer_get_time() + PENDING_US};
    strlcpy(s_pending[slot].args, args, sizeof(s_pending[slot].args));
    char a[64];
    html(args, a, sizeof(a));
    snprintf(out, sizeof(out), T(user, "Run <b>/%s %s</b> (%s)?", "Provést <b>/%s %s</b> (%s)?"), s_cmds[cmd].name, a,
             cs(user) ? s_cmds[cmd].help_cs : s_cmds[cmd].help_en);
    send_text(chat, out, buttons(user, slot));
}

static void on_callback(const cJSON *cb)
{
    const char *data = GW_JSON_STR(cb, "data");
    const cJSON *msg = cJSON_GetObjectItem(cb, "message");
    const cJSON *chat_j = cJSON_GetObjectItem(cJSON_GetObjectItem(msg, "chat"), "id");
    cJSON *ack = cJSON_CreateObject();
    cJSON_AddStringToObject(ack, "callback_query_id", GW_JSON_STR(cb, "id") ? GW_JSON_STR(cb, "id") : "");
    cJSON_Delete(tg_call("answerCallbackQuery", ack, 10));
    if (!data || !cJSON_IsNumber(chat_j) || strlen(data) < 4) {
        return;
    }
    int64_t chat = (int64_t)chat_j->valuedouble;
    int slot = atoi(data + 3);
    if (slot < 0 || slot >= PENDING) {
        return;
    }
    pending_t *pd = &s_pending[slot];
    gw_auth_lock();
    int user = gw_auth_user_by_chat(chat);
    bool totp = user >= 0 && gw_auth_user_totp(user);
    gw_auth_unlock();
    if (!pd->used || pd->chat != chat || pd->user != user || pd->want_code) {
        return;
    }
    if (strncmp(data, "no:", 3) == 0 || pd->expires_us < esp_timer_get_time()) {
        pd->used = false;
        send_text(chat, T(user, "Cancelled.", "Zrušeno."), NULL);
        return;
    }
    if (totp) {
        pd->want_code = true;
        pd->expires_us = esp_timer_get_time() + PENDING_US;
        send_text(chat, T(user, "Send the 6-digit code from your authenticator app.", "Pošlete 6místný kód z aplikace Authenticator."), NULL);
        return;
    }
    pd->used = false;
    char out[1500];
    run(user, pd->cmd, pd->args, out, sizeof(out));
    send_text(chat, out, NULL);
    gw_notify("security", GW_NOTIFY_INFO, "Telegram: %s ran /%s %s", gw_auth_user_name(user), s_cmds[pd->cmd].name, pd->args);
}

/* ---- notifications ---- */

static void on_note(const char *topic, gw_notify_level_t level, const char *text)
{
    if (!s_notes || !s_enabled) {
        return;
    }
    note_t n = {.level = level};
    for (int t = 0; t < GW_TG_TOPICS; t++) {
        if (strcmp(topic, s_topic_keys[t]) == 0) {
            n.topic = t;
        }
    }
    strlcpy(n.text, text, sizeof(n.text));
    xQueueSend(s_notes, &n, 0);
}

static bool quiet_now(const gw_tg_prefs_t *p)
{
    time_t t = time(NULL);
    if (p->quiet_from < 0 || p->quiet_to < 0 || t < 1700000000) {
        return false;
    }
    struct tm lt;
    localtime_r(&t, &lt);
    return p->quiet_from <= p->quiet_to ? lt.tm_hour >= p->quiet_from && lt.tm_hour < p->quiet_to
                                        : lt.tm_hour >= p->quiet_from || lt.tm_hour < p->quiet_to;
}

/* topic -> page a user needs to read to get its notifications */
static const char *const s_topic_page[GW_TG_TOPICS] = {"admin", "system/info", "system/lan", "sound/log"};

static void deliver(const note_t *n)
{
    static const char *const icons[] = {"ℹ️", "⚠️", "🚨"};
    static const char *const names_cs[] = {"Zabezpečení", "Systém", "Síť", "Zvuk"};
    static const char *const names_en[] = {"Security", "System", "Network", "Sound"};
    int64_t chats[16];
    int users[16], count = 0;
    gw_auth_lock();
    for (int i = 0; i < gw_auth_users_max() && i < 16; i++) {
        const gw_tg_prefs_t *p = gw_auth_user_valid(i) ? gw_auth_user_tg(i) : NULL;
        if (!p || !p->chat || gw_auth_user_level(i, s_topic_page[n->topic]) < 1) {
            continue;
        }
        /* 1 critical, 2 warnings and critical, 3 all */
        int need = p->levels[n->topic] == 3 ? 0 : p->levels[n->topic] == 2 ? 1 : p->levels[n->topic] == 1 ? 2 : 99;
        if (quiet_now(p) && need < 2) {
            need = 2;
        }
        if (n->topic == GW_TG_SOUND && s_mute_until_us[i] > esp_timer_get_time()) {
            continue;
        }
        if (n->level >= need) {
            chats[count] = p->chat;
            users[count++] = i;
        }
    }
    gw_auth_unlock();
    char text[400], esc[300];
    html(n->text, esc, sizeof(esc));
    for (int k = 0; k < count; k++) {
        snprintf(text, sizeof(text), "%s <b>%s</b> · %s\n%s", icons[n->level],
                 cs(users[k]) ? names_cs[n->topic] : names_en[n->topic], gw_state()->hostname, esc);
        send_text(chats[k], text, NULL);
    }
}

/* ---- task ---- */

static void load(void)
{
    if (!gw_config_get_str(KEY_TOKEN, s_token, sizeof(s_token))) {
        s_token[0] = '\0';
    }
    s_enabled = gw_config_get_u8(KEY_ON, 0) && s_token[0];
    if (!gw_config_get_str(KEY_TZ, s_tz, sizeof(s_tz)) || !s_tz[0]) {
        strlcpy(s_tz, DEFAULT_TZ, sizeof(s_tz));
    }
    setenv("TZ", s_tz, 1);
    tzset();
}

static void tg_task(void *arg)
{
    bool restart = true;
    while (true) {
        if (ulTaskNotifyTake(pdTRUE, 0)) {
            restart = true;
        }
        if (restart) {
            restart = false;
            http_close();
            load();
            s_bot[0] = '\0';
            s_error[0] = '\0';
        }
        if (!s_enabled) {
            strlcpy(s_state, "off", sizeof(s_state));
            xQueueReset(s_notes);
            restart = ulTaskNotifyTake(pdTRUE, portMAX_DELAY) > 0;
            continue;
        }
        if (!gw_eth_wait_ip(pdMS_TO_TICKS(5000))) {
            continue;
        }
        if (!s_bot[0]) {
            strlcpy(s_state, "connecting", sizeof(s_state));
            cJSON *me = tg_call("getMe", cJSON_CreateObject(), 10);
            if (!me) {
                strlcpy(s_state, "error", sizeof(s_state));
                ESP_LOGW(TAG, "%s", s_error);
                restart = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(60000)) > 0;
                continue;
            }
            strlcpy(s_bot, GW_JSON_STR(me, "username") ? GW_JSON_STR(me, "username") : "?", sizeof(s_bot));
            cJSON_Delete(me);
            ESP_LOGI(TAG, "bot @%s", s_bot);
        }
        note_t n;
        while (xQueueReceive(s_notes, &n, 0) == pdTRUE) {
            deliver(&n);
        }
        out_t o;
        while (xQueueReceive(s_out, &o, 0) == pdTRUE) {
            send_text(o.chat, o.text, NULL);
            free(o.text);
        }
        cJSON *b = cJSON_CreateObject();
        cJSON_AddNumberToObject(b, "offset", (double)s_offset);
        cJSON_AddNumberToObject(b, "timeout", POLL_S);
        cJSON_AddNumberToObject(b, "limit", 10);
        cJSON *allowed = cJSON_AddArrayToObject(b, "allowed_updates");
        cJSON_AddItemToArray(allowed, cJSON_CreateString("message"));
        cJSON_AddItemToArray(allowed, cJSON_CreateString("callback_query"));
        cJSON *updates = tg_call("getUpdates", b, POLL_S);
        if (!updates) {
            strlcpy(s_state, "error", sizeof(s_state));
            ESP_LOGW(TAG, "%s", s_error);
            restart = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(10000)) > 0;
            continue;
        }
        strlcpy(s_state, "running", sizeof(s_state));
        s_error[0] = '\0';
        const cJSON *u;
        cJSON_ArrayForEach(u, updates)
        {
            const cJSON *id = cJSON_GetObjectItem(u, "update_id");
            if (cJSON_IsNumber(id)) {
                s_offset = (int64_t)id->valuedouble + 1;
            }
            if (cJSON_GetObjectItem(u, "message")) {
                on_message(cJSON_GetObjectItem(u, "message"));
            } else if (cJSON_GetObjectItem(u, "callback_query")) {
                on_callback(cJSON_GetObjectItem(u, "callback_query"));
            }
        }
        cJSON_Delete(updates);
    }
}

/* ---- built-in commands ---- */

static void cmd_status(const char *args, bool cz, char *out, size_t len)
{
    gw_state_t *st = gw_state();
    cJSON *eth = gw_eth_status();
    float temp = 0;
    bool has_temp = gw_api_temperature(&temp);
    int64_t up = esp_timer_get_time() / 1000000;
    int inet = gw_inet_state();
    snprintf(out, len,
             "<b>%s</b> %s\n%s: %s%s\nLAN: %s, %s\n%s: %s · MQTT: %s\n%s: %lldd %lldh %lldm%s",
             st->hostname, esp_app_get_description()->version, cz ? "Role" : "Role", gw_role_name(st->active),
             st->maintenance[0] ? (cz ? " (údržba)" : " (maintenance)") : "",
             cJSON_IsTrue(cJSON_GetObjectItem(eth, "link")) ? (cz ? "linka" : "link up") : (cz ? "bez linky" : "link down"),
             GW_JSON_STR(eth, "ipv4") ? GW_JSON_STR(eth, "ipv4") : "?", "Internet",
             inet > 0 ? (cz ? "ano" : "yes") : inet == 0 ? (cz ? "ne" : "no") : "?", gw_mqtt_link(),
             cz ? "Běží" : "Up", up / 86400, up % 86400 / 3600, up % 3600 / 60, "");
    cJSON_Delete(eth);
    if (has_temp) {
        size_t n = strlen(out);
        snprintf(out + n, len - n, "\n%s: %.1f °C", cz ? "Teplota" : "Temperature", (double)temp);
    }
}

static void cmd_lan(const char *args, bool cz, char *out, size_t len)
{
    cJSON *eth = gw_eth_status(), *tr = gw_eth_traffic();
    const cJSON *dns = cJSON_GetArrayItem(cJSON_GetObjectItem(eth, "dns"), 0);
    snprintf(out, len, "<b>LAN</b>\n%s: %s, %d Mb/s %s\n%s: %s\nIPv4: %s / %s\n%s: %s\nDNS: %s\nRX %.1f MB · TX %.1f MB",
             cz ? "Linka" : "Link", cJSON_IsTrue(cJSON_GetObjectItem(eth, "link")) ? "up" : "down",
             cJSON_GetObjectItem(eth, "speed") ? cJSON_GetObjectItem(eth, "speed")->valueint : 0,
             cJSON_IsTrue(cJSON_GetObjectItem(eth, "full_duplex")) ? "full" : "half", cz ? "Adresace" : "Addressing",
             GW_JSON_STR(eth, "mode") ? GW_JSON_STR(eth, "mode") : "?", GW_JSON_STR(eth, "ipv4") ? GW_JSON_STR(eth, "ipv4") : "?",
             GW_JSON_STR(eth, "netmask") ? GW_JSON_STR(eth, "netmask") : "?", cz ? "Brána" : "Gateway",
             GW_JSON_STR(eth, "gateway") ? GW_JSON_STR(eth, "gateway") : "?", cJSON_IsString(dns) ? dns->valuestring : "?",
             cJSON_GetObjectItem(tr, "rx")->valuedouble / 1e6, cJSON_GetObjectItem(tr, "tx")->valuedouble / 1e6);
    cJSON_Delete(eth);
    cJSON_Delete(tr);
}

/* state of the active role, from its status provider (flat values only) */
static void role_status(const char *prefix, bool cz, char *out, size_t len)
{
    if (strncmp(gw_role_name(gw_state()->active), prefix, strlen(prefix)) != 0) {
        snprintf(out, len, cz ? "Role %s neběží (aktivní: %s)." : "The %s role is not active (active: %s).", prefix,
                 gw_role_name(gw_state()->active));
        return;
    }
    cJSON *j = gw_api_role_status();
    size_t n = snprintf(out, len, "<b>%s</b>\n", gw_role_name(gw_state()->active));
    const cJSON *v;
    cJSON_ArrayForEach(v, j)
    {
        char val[48] = "";
        if (cJSON_IsString(v)) {
            html(v->valuestring, val, sizeof(val));
        } else if (cJSON_IsNumber(v)) {
            snprintf(val, sizeof(val), "%g", v->valuedouble);
        } else if (cJSON_IsBool(v)) {
            strlcpy(val, cJSON_IsTrue(v) ? "true" : "false", sizeof(val));
        } else {
            continue;
        }
        if (n + 64 < len) {
            n += snprintf(out + n, len - n, "%s: %s\n", v->string, val);
        }
    }
    cJSON_Delete(j);
}

static void cmd_thread(const char *args, bool cz, char *out, size_t len)
{
    role_status("thread", cz, out, len);
}

static void cmd_zigbee(const char *args, bool cz, char *out, size_t len)
{
    role_status("zigbee", cz, out, len);
}

static void cmd_restart(const char *args, bool cz, char *out, size_t len)
{
    gw_restart_later_full(3000, true);
    strlcpy(out, cz ? "Restartuji C6 a P4, zpět asi za 15 s." : "Restarting the C6 and the P4, back in about 15 s.", len);
}

static void cmd_identify(const char *args, bool cz, char *out, size_t len)
{
    gw_identify(10);
    strlcpy(out, cz ? "Ikona identifikace bliká 10 s ve webovém rozhraní." : "The identify icon blinks for 10 s in the web UI.", len);
}

/* gateway and C6 versions, release notes and build of the offered release */
static void cmd_update(const char *args, bool cz, char *out, size_t len)
{
    gw_update_check_soon();
    cJSON *u = gw_update_status();
    const cJSON *c6 = cJSON_GetObjectItem(u, "c6");
    bool available = cJSON_IsTrue(cJSON_GetObjectItem(u, "available"));
    bool c6_available = cJSON_IsTrue(cJSON_GetObjectItem(c6, "available"));
    char notes[700] = "", build[160] = "";
    html(GW_JSON_STR(u, "release_summary") ? GW_JSON_STR(u, "release_summary") : "", notes, sizeof(notes));
    html(GW_JSON_STR(u, "build") ? GW_JSON_STR(u, "build") : "", build, sizeof(build));
    size_t n = snprintf(out, len, cz ? "<b>Brána</b>: %s → %s%s\n<b>Rádio C6</b>: %s"
                                     : "<b>Gateway</b>: %s → %s%s\n<b>C6 radio</b>: %s",
                        GW_JSON_STR(u, "installed_version") ? GW_JSON_STR(u, "installed_version") : "?",
                        GW_JSON_STR(u, "latest_version") ? GW_JSON_STR(u, "latest_version") : "?",
                        available ? (cz ? " (/install)" : " (/install)") : (cz ? " (aktuální)" : " (up to date)"),
                        c6_available ? (cz ? "nový balík (/install c6)" : "new package (/install c6)")
                                     : cJSON_IsTrue(cJSON_GetObjectItem(c6, "offered")) ? (cz ? "aktuální" : "up to date")
                                                                                        : (cz ? "nenabízeno" : "not offered"));
    if (build[0] && n < len) {
        n += snprintf(out + n, len - n, cz ? "\n<b>Sestavení</b>: %s" : "\n<b>Build</b>: %s", build);
    }
    if (notes[0] && n < len) {
        n += snprintf(out + n, len - n, cz ? "\n\n<b>Poznámky k vydání</b>\n%s" : "\n\n<b>Release notes</b>\n%s", notes);
    }
    if (!GW_JSON_STR(u, "url") || !GW_JSON_STR(u, "url")[0]) {
        snprintf(out + n, len - n, "%s", cz ? "\n\nAdresa manifestu není nastavená (Systém › Firmware)." : "\n\nNo manifest URL set (System › Firmware).");
    }
    cJSON_Delete(u);
}

static void cmd_install(const char *args, bool cz, char *out, size_t len)
{
    if (strcmp(args, "c6") == 0) {
        esp_err_t err = gw_update_install_c6();
        snprintf(out, len, err == ESP_OK ? (cz ? "Stahuji balík C6, brána se pak restartuje a aktualizuje rádio (asi 2 min)." : "Downloading the C6 package; the gateway then restarts and updates the radio (about 2 min).")
                                         : (cz ? "Balík C6 nejde nainstalovat: %s" : "Cannot install the C6 package: %s"),
                 err == ESP_ERR_NOT_FOUND ? (cz ? "není nabízen" : "none offered") : esp_err_to_name(err));
        return;
    }
    esp_err_t err = gw_update_install();
    snprintf(out, len, err == ESP_OK ? (cz ? "Instaluji aktualizaci, brána se pak restartuje." : "Installing the update, the gateway restarts after it.")
                                     : (cz ? "Aktualizaci nejde spustit: %s" : "Cannot start the update: %s"),
             esp_err_to_name(err));
}

static void cmd_role(const char *args, bool cz, char *out, size_t len)
{
    char err[80];
    if (gw_api_role_switch(args, false, err, sizeof(err)) == ESP_OK) {
        snprintf(out, len, cz ? "Přepínám na roli %s, brána se restartuje." : "Switching to %s, the gateway restarts.", args);
    } else {
        snprintf(out, len, cz ? "Roli nejde přepnout: %s\nRole: thread_br, thread_router, zigbee_coordinator, zigbee_router, esphome"
                              : "Cannot switch: %s\nRoles: thread_br, thread_router, zigbee_coordinator, zigbee_router, esphome",
                 err);
    }
}

/* ---- HTTP API ---- */

static cJSON *admin_json(void)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "enabled", s_enabled);
    cJSON_AddBoolToObject(j, "token_set", s_token[0] != 0);
    cJSON_AddStringToObject(j, "bot", s_bot);
    cJSON_AddStringToObject(j, "state", s_state);
    cJSON_AddStringToObject(j, "error", s_error);
    cJSON_AddStringToObject(j, "tz", s_tz);
    int paired = 0;
    gw_auth_lock();
    for (int i = 0; i < gw_auth_users_max(); i++) {
        paired += gw_auth_user_valid(i) && gw_auth_user_tg(i)->chat;
    }
    gw_auth_unlock();
    cJSON_AddNumberToObject(j, "paired", paired);
    return j;
}

static esp_err_t telegram_get(httpd_req_t *req)
{
    return gw_http_send_json(req, admin_json());
}

static void restart_task(void)
{
    if (s_task) {
        xTaskNotifyGive(s_task);
    }
}

/* {"token", "enabled", "tz"} */
static esp_err_t telegram_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *token = GW_JSON_STR(body, "token"), *tz = GW_JSON_STR(body, "tz");
    const cJSON *en = cJSON_GetObjectItem(body, "enabled");
    if (token && (strlen(token) >= sizeof(s_token) || (token[0] && !strchr(token, ':')))) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "token: the one BotFather gave, 123456:ABC...");
    }
    if (tz && (strlen(tz) >= sizeof(s_tz) || !tz[0])) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "tz: POSIX time zone, e.g. CET-1CEST,M3.5.0,M10.5.0/3");
    }
    if (token) {
        gw_config_set_str(KEY_TOKEN, token);
    }
    if (tz) {
        gw_config_set_str(KEY_TZ, tz);
    }
    if (cJSON_IsBool(en)) {
        gw_config_set_u8(KEY_ON, cJSON_IsTrue(en));
    }
    cJSON_Delete(body);
    load();
    if (!s_enabled) {
        strlcpy(s_state, "off", sizeof(s_state));
        s_error[0] = '\0';
    }
    restart_task();
    gw_notify("security", GW_NOTIFY_INFO, "Telegram bot settings changed by %s", gw_auth_who());
    return gw_http_send_json(req, admin_json());
}

static int self_user(httpd_req_t *req)
{
    int u = gw_auth_current_user();
    if (u < 0) {
        gw_http_send_error(req, 409, "only for signed-in users");
    }
    return u;
}

static cJSON *self_json(int u)
{
    gw_tg_prefs_t *p = gw_auth_user_tg(u);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "bot_enabled", s_enabled);
    cJSON_AddStringToObject(j, "bot", s_bot);
    cJSON_AddBoolToObject(j, "paired", p->chat != 0);
    cJSON *lv = cJSON_AddObjectToObject(j, "levels");
    cJSON *al = cJSON_AddObjectToObject(j, "allowed");
    for (int t = 0; t < GW_TG_TOPICS; t++) {
        cJSON_AddNumberToObject(lv, s_topic_keys[t], p->levels[t]);
        cJSON_AddBoolToObject(al, s_topic_keys[t], gw_auth_user_level(u, s_topic_page[t]) >= 1);
    }
    cJSON_AddNumberToObject(j, "quiet_from", p->quiet_from);
    cJSON_AddNumberToObject(j, "quiet_to", p->quiet_to);
    cJSON_AddStringToObject(j, "lang", p->lang);
    int64_t mute = u < 16 ? s_mute_until_us[u] - esp_timer_get_time() : 0;
    cJSON_AddNumberToObject(j, "sound_muted_s", mute > 0 ? (int)(mute / 1000000) : 0);
    return j;
}

static esp_err_t self_get(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    gw_auth_lock();
    cJSON *j = self_json(u);
    gw_auth_unlock();
    return gw_http_send_json(req, j);
}

/* {"levels": {topic: 0..3}, "quiet_from", "quiet_to" (-1 none), "lang"} */
static esp_err_t self_post(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    gw_auth_lock();
    gw_tg_prefs_t *p = gw_auth_user_tg(u);
    const cJSON *v;
    for (int t = 0; t < GW_TG_TOPICS; t++) {
        v = cJSON_GetObjectItem(cJSON_GetObjectItem(body, "levels"), s_topic_keys[t]);
        if (cJSON_IsNumber(v) && v->valueint >= 0 && v->valueint <= 3) {
            p->levels[t] = v->valueint;
        }
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "quiet_from")) && v->valueint >= -1 && v->valueint <= 23) {
        p->quiet_from = v->valueint;
    }
    if (cJSON_IsNumber(v = cJSON_GetObjectItem(body, "quiet_to")) && v->valueint >= -1 && v->valueint <= 23) {
        p->quiet_to = v->valueint;
    }
    const char *lang = GW_JSON_STR(body, "lang");
    if (lang && (strcmp(lang, "cs") == 0 || strcmp(lang, "en") == 0)) {
        strlcpy(p->lang, lang, sizeof(p->lang));
    }
    gw_auth_user_save(u);
    cJSON *j = self_json(u);
    gw_auth_unlock();
    cJSON_Delete(body);
    return gw_http_send_json(req, j);
}

static esp_err_t pair_post(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    if (!s_enabled || !s_bot[0]) {
        return gw_http_send_error(req, 409, "the Telegram bot is not running (System > Notifications)");
    }
    s_pair[u].code = 10000000 + esp_random() % 90000000;
    s_pair[u].until_us = esp_timer_get_time() + PAIR_US;
    char link[96];
    snprintf(link, sizeof(link), "https://t.me/%s?start=%lu", s_bot, (unsigned long)s_pair[u].code);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddNumberToObject(j, "code", s_pair[u].code);
    cJSON_AddStringToObject(j, "bot", s_bot);
    cJSON_AddStringToObject(j, "link", link);
    cJSON_AddNumberToObject(j, "expires_s", PAIR_US / 1000000);
    return gw_http_send_json(req, j);
}

static esp_err_t unpair_post(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    gw_auth_lock();
    gw_auth_user_tg(u)->chat = 0;
    gw_auth_user_save(u);
    cJSON *j = self_json(u);
    gw_auth_unlock();
    return gw_http_send_json(req, j);
}

static esp_err_t test_post(httpd_req_t *req)
{
    int u = self_user(req);
    if (u < 0) {
        return ESP_OK;
    }
    gw_auth_lock();
    out_t o = {.chat = gw_auth_user_tg(u)->chat};
    bool czech = cs(u);
    gw_auth_unlock();
    if (!o.chat || !s_enabled) {
        return gw_http_send_error(req, 409, "not paired or the bot is off");
    }
    o.text = strdup(czech ? "✅ Testovací zpráva z brány. Notifikace fungují." : "✅ Test message from the gateway. Notifications work.");
    if (!o.text || xQueueSend(s_out, &o, 0) != pdTRUE) {
        free(o.text);
        return gw_http_send_error(req, 503, "queue full");
    }
    return gw_http_send_json(req, cJSON_CreateObject());
}

void gw_telegram_start(void)
{
    s_resp = heap_caps_malloc(RESP_MAX, MALLOC_CAP_SPIRAM);
    s_notes = xQueueCreate(NOTE_QUEUE, sizeof(note_t));
    s_out = xQueueCreate(OUT_QUEUE, sizeof(out_t));
    load();
    gw_bot_command("status", "gateway state", "stav brány", "overview", 1, false, cmd_status);
    gw_bot_command("lan", "LAN state and traffic", "stav a provoz LAN", "system/lan", 1, false, cmd_lan);
    gw_bot_command("thread", "Thread network", "síť Thread", "thread/info", 1, false, cmd_thread);
    gw_bot_command("zigbee", "Zigbee coordinator or router", "Zigbee koordinátor nebo router", "zigbee/info", 1, false, cmd_zigbee);
    gw_bot_command("restart", "restart the whole gateway (C6 and P4)", "restart celé brány (C6 a P4)", "system/maint", 2, true, cmd_restart);
    gw_bot_command("identify", "blink the identify icon", "rozblikat ikonu identifikace", "overview", 2, true, cmd_identify);
    gw_bot_command("update", "check for a firmware update", "kontrola aktualizace firmwaru", "system/firmware", 1, false, cmd_update);
    gw_bot_command("install", "install the offered gateway firmware (/install c6: the C6 radio package)",
                   "instalace nabízeného firmwaru brány (/install c6: balík rádia C6)", "system/firmware", 2, true, cmd_install);
    gw_bot_command("role", "switch the role, e.g. /role zigbee_coordinator", "přepnutí role, např. /role zigbee_coordinator", "role", 2, true, cmd_role);
    gw_notify_set_listener(on_note);
    gw_http_register("/api/telegram", HTTP_GET, telegram_get);
    gw_http_register("/api/telegram", HTTP_POST, telegram_post);
    gw_http_register("/api/auth/telegram", HTTP_GET, self_get);
    gw_http_register("/api/auth/telegram", HTTP_POST, self_post);
    gw_http_register("/api/auth/telegram/pair", HTTP_POST, pair_post);
    gw_http_register("/api/auth/telegram/unpair", HTTP_POST, unpair_post);
    gw_http_register("/api/auth/telegram/test", HTTP_POST, test_post);
    xTaskCreate(tg_task, "telegram", 8192, NULL, 3, &s_task);
}
