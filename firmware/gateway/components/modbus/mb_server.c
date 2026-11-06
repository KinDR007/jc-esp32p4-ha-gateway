#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "lwip/sockets.h"
#include "mb_server.h"
#include "sdkconfig.h"

static const char *TAG = "mb_server";

/*
 * Modbus TCP server for the gateway's Modbus devices (docs/modbus.md): the shared port (502) routes by unit
 * id, a device may also have its own port where any unit id reaches it. Reads the gateway polls anyway come
 * from its register cache; other requests go to the device through the modbus task (the only user of the
 * RS485 bus), limited per device. Writes only where switched on. One task serves every socket, one request
 * per client at a time.
 */

#define KEY_SRV "mb_srv" /* str JSON {"enabled", "port", "devices": [{device, unit, port, write, allow, rate}]} */
#define ENTRIES_MAX 16
#define LISTEN_MAX 8
#define CLIENTS_PER_PORT 2
#define CLIENTS_MAX 8
#define ALLOW_MAX 4
#define RATE_DEFAULT 5
#define RATE_MAX 50
#define IDLE_US (120 * 1000000LL)

typedef struct {
    char dev[8];
    uint8_t unit;  /* on the shared port, 0 = not there */
    uint16_t port; /* own port, 0 = none */
    bool write;
    uint8_t nallow;
    uint32_t allow[ALLOW_MAX]; /* addresses allowed to write (network order), none = anyone */
    uint8_t rate;              /* requests per second passed to the device */
    /* runtime */
    double tokens;
    int64_t refill_us;
    uint32_t requests, cached, passed, refused, failed;
} entry_t;

typedef struct {
    bool enabled; /* the shared port */
    uint16_t port;
    int n;
    entry_t e[ENTRIES_MAX];
} cfg_t;

typedef struct {
    uint16_t port;
    int entry; /* -1: the shared port */
    int sock;
    char error[40];
} listener_t;

typedef struct {
    int sock; /* -1: free (once no job runs) */
    int lst;
    uint32_t ip;
    int64_t since_us, last_us;
    uint32_t requests;
    size_t got;
    uint8_t buf[MB_TCP_MAX];
    bool pending; /* a job runs, also after the client is gone */
    int entry;
    uint16_t tid;
    uint8_t unit;
} client_t;

static SemaphoreHandle_t s_lock;
static TaskHandle_t s_task;
static cfg_t *s_cfg; /* PSRAM */
static uint32_t s_gen;
static listener_t s_lst[LISTEN_MAX];
static int s_nlst;
static client_t s_cli[CLIENTS_MAX];
static mb_srv_job_t *s_job; /* CLIENTS_MAX, PSRAM: the job of client slot i */
static uint32_t s_turned_away;

static uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] << 8 | p[1]);
}

static bool reserved_port(int port)
{
    return port == 80 || port == 8081 || port == CONFIG_GW_ZIGBEE_TCP_PORT;
}

/* ---------------------------------------------------------------- configuration */

/* a dotted quad only (inet_aton also takes "10.1" and the like); network order */
static bool parse_ip(const char *s, uint32_t *ip)
{
    unsigned a, b, c, d;
    char end;
    if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &end) != 4 || a > 255 || b > 255 || c > 255 || d > 255) {
        return false;
    }
    *ip = htonl(a << 24 | b << 16 | c << 8 | d);
    return true;
}

/* devices: check that each device is configured (not for the stored configuration: a device removed
 * meanwhile stays, its requests answer exception 0x0a) */
static const char *parse(const cJSON *root, cfg_t *c, bool devices)
{
    memset(c, 0, sizeof(*c));
    c->enabled = true;
    c->port = 502;
    const cJSON *j = cJSON_GetObjectItem(root, "enabled");
    if (j) {
        if (!cJSON_IsBool(j)) {
            return "enabled: true or false";
        }
        c->enabled = cJSON_IsTrue(j);
    }
    j = cJSON_GetObjectItem(root, "port");
    if (j) {
        if (!cJSON_IsNumber(j) || j->valueint < 1 || j->valueint > 65535 || reserved_port(j->valueint)) {
            return "port: 1-65535, not 80, 8081 or the Zigbee port";
        }
        c->port = (uint16_t)j->valueint;
    }
    const cJSON *list = cJSON_GetObjectItem(root, "devices");
    if (list && (!cJSON_IsArray(list) || cJSON_GetArraySize(list) > ENTRIES_MAX)) {
        return "devices: a list of up to 16";
    }
    static char why[96];
    int listeners = 0;
    bool shared = false;
    cJSON_ArrayForEach(j, list)
    {
        entry_t *e = &c->e[c->n];
        const cJSON *dev = cJSON_GetObjectItem(j, "device"), *unit = cJSON_GetObjectItem(j, "unit"),
                    *port = cJSON_GetObjectItem(j, "port"), *write = cJSON_GetObjectItem(j, "write"),
                    *allow = cJSON_GetObjectItem(j, "allow"), *rate = cJSON_GetObjectItem(j, "rate");
        if (!cJSON_IsString(dev) || strlen(dev->valuestring) >= sizeof(e->dev)
            || (devices && !modbus_srv_device(dev->valuestring, NULL, 0))) {
            snprintf(why, sizeof(why), "devices[%d]: device: the id of a configured Modbus device", c->n);
            return why;
        }
        strcpy(e->dev, dev->valuestring);
        if ((unit && (!cJSON_IsNumber(unit) || unit->valueint < 0 || unit->valueint > 247))
            || (port && (!cJSON_IsNumber(port) || port->valueint < 0 || port->valueint > 65535))) {
            snprintf(why, sizeof(why), "devices[%d]: unit 0-247, port 0-65535", c->n);
            return why;
        }
        e->unit = unit ? (uint8_t)unit->valueint : 0;
        e->port = port ? (uint16_t)port->valueint : 0;
        if (!e->unit && !e->port) {
            snprintf(why, sizeof(why), "devices[%d]: a unit on port %u or an own port", c->n, c->port);
            return why;
        }
        if (e->port && (reserved_port(e->port) || e->port == c->port)) {
            snprintf(why, sizeof(why), "devices[%d]: port %u taken", c->n, e->port);
            return why;
        }
        if (write && !cJSON_IsBool(write)) {
            snprintf(why, sizeof(why), "devices[%d]: write: true or false", c->n);
            return why;
        }
        e->write = cJSON_IsTrue(write);
        if (allow && (!cJSON_IsArray(allow) || cJSON_GetArraySize(allow) > ALLOW_MAX)) {
            snprintf(why, sizeof(why), "devices[%d]: allow: up to %d IPv4 addresses", c->n, ALLOW_MAX);
            return why;
        }
        const cJSON *a;
        cJSON_ArrayForEach(a, allow)
        {
            if (!cJSON_IsString(a) || !parse_ip(a->valuestring, &e->allow[e->nallow])) {
                snprintf(why, sizeof(why), "devices[%d]: allow: %s is not an IPv4 address", c->n, cJSON_IsString(a) ? a->valuestring : "?");
                return why;
            }
            e->nallow++;
        }
        if (rate && (!cJSON_IsNumber(rate) || rate->valueint < 1 || rate->valueint > RATE_MAX)) {
            snprintf(why, sizeof(why), "devices[%d]: rate: 1-%d requests per second", c->n, RATE_MAX);
            return why;
        }
        e->rate = rate ? (uint8_t)rate->valueint : RATE_DEFAULT;
        for (int k = 0; k < c->n; k++) {
            const entry_t *o = &c->e[k];
            if (!strcmp(o->dev, e->dev) || (e->unit && o->unit == e->unit) || (e->port && o->port == e->port)) {
                snprintf(why, sizeof(why), "devices[%d]: device, unit or port used twice", c->n);
                return why;
            }
        }
        shared |= e->unit != 0;
        listeners += e->port != 0;
        c->n++;
    }
    if (listeners + (shared && c->enabled) > LISTEN_MAX) {
        snprintf(why, sizeof(why), "at most %d ports (the shared one counts)", LISTEN_MAX);
        return why;
    }
    return NULL;
}

static cJSON *config_json(const cfg_t *c, bool stats)
{
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "enabled", c->enabled);
    cJSON_AddNumberToObject(root, "port", c->port);
    cJSON *list = cJSON_AddArrayToObject(root, "devices");
    for (int i = 0; i < c->n; i++) {
        const entry_t *e = &c->e[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "device", e->dev);
        cJSON_AddNumberToObject(j, "unit", e->unit);
        cJSON_AddNumberToObject(j, "port", e->port);
        cJSON_AddBoolToObject(j, "write", e->write);
        cJSON *allow = cJSON_AddArrayToObject(j, "allow");
        for (int k = 0; k < e->nallow; k++) {
            char ip[16];
            inet_ntoa_r(*(struct in_addr *)&e->allow[k], ip, sizeof(ip));
            cJSON_AddItemToArray(allow, cJSON_CreateString(ip));
        }
        cJSON_AddNumberToObject(j, "rate", e->rate);
        if (stats) {
            char name[100] = "";
            cJSON_AddBoolToObject(j, "missing", !modbus_srv_device(e->dev, name, sizeof(name)));
            cJSON_AddStringToObject(j, "name", name);
            cJSON_AddNumberToObject(j, "requests", e->requests);
            cJSON_AddNumberToObject(j, "cached", e->cached);
            cJSON_AddNumberToObject(j, "passed", e->passed);
            cJSON_AddNumberToObject(j, "refused", e->refused);
            cJSON_AddNumberToObject(j, "failed", e->failed);
        }
        cJSON_AddItemToArray(list, j);
    }
    return root;
}

/* ---------------------------------------------------------------- task */

static void close_client(client_t *c)
{
    if (c->sock >= 0) {
        close(c->sock);
    }
    c->sock = -1;
    c->got = 0;
}

static int open_listener(uint16_t port, char *err, size_t len)
{
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) {
        snprintf(err, len, "no socket left");
        return -1;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) != 0 || listen(s, CLIENTS_PER_PORT) != 0) {
        snprintf(err, len, "port %u in use", port);
        close(s);
        return -1;
    }
    return s;
}

/* new configuration: every listener and client anew; under s_lock */
static void rebuild(void)
{
    for (int i = 0; i < CLIENTS_MAX; i++) {
        close_client(&s_cli[i]);
    }
    for (int i = 0; i < s_nlst; i++) {
        if (s_lst[i].sock >= 0) {
            close(s_lst[i].sock);
        }
    }
    s_nlst = 0;
    bool shared = false;
    for (int i = 0; i < s_cfg->n; i++) {
        shared |= s_cfg->e[i].unit != 0;
    }
    if (shared && s_cfg->enabled) {
        s_lst[s_nlst++] = (listener_t){.port = s_cfg->port, .entry = -1};
    }
    for (int i = 0; i < s_cfg->n && s_nlst < LISTEN_MAX; i++) {
        if (s_cfg->e[i].port) {
            s_lst[s_nlst++] = (listener_t){.port = s_cfg->e[i].port, .entry = i};
        }
    }
    for (int i = 0; i < s_nlst; i++) {
        listener_t *l = &s_lst[i];
        l->sock = open_listener(l->port, l->error, sizeof(l->error));
        if (l->sock >= 0) {
            ESP_LOGI(TAG, "port %u: %s", l->port, l->entry < 0 ? "shared, by unit id" : s_cfg->e[l->entry].dev);
        } else {
            ESP_LOGW(TAG, "port %u: %s", l->port, l->error);
        }
    }
}

static void send_pdu(client_t *c, const uint8_t *pdu, size_t len)
{
    uint8_t adu[MB_TCP_MAX];
    size_t n = mb_tcp_frame(c->tid, c->unit, pdu, len, adu, sizeof(adu));
    if (!n || send(c->sock, adu, n, 0) != (int)n) {
        close_client(c);
    }
}

static void send_exception(client_t *c, uint8_t fc, uint8_t code)
{
    uint8_t pdu[2] = {(uint8_t)(fc | 0x80), code};
    send_pdu(c, pdu, sizeof(pdu));
}

static bool may_write(const entry_t *e, uint32_t ip)
{
    if (!e->write) {
        return false;
    }
    for (int k = 0; k < e->nallow; k++) {
        if (e->allow[k] == ip) {
            return true;
        }
    }
    return e->nallow == 0;
}

/* one request of client slot i (complete in its buffer, len bytes); under s_lock */
static void handle(int i, size_t len)
{
    client_t *c = &s_cli[i];
    mb_srv_job_t *job = &s_job[i];
    c->tid = get16(c->buf);
    c->unit = c->buf[6];
    const uint8_t *pdu = c->buf + 7;
    size_t plen = len - 7;
    const listener_t *l = &s_lst[c->lst];
    int ei = l->entry;
    for (int k = 0; ei < 0 && k < s_cfg->n; k++) {
        if (s_cfg->e[k].unit && s_cfg->e[k].unit == c->unit) {
            ei = k;
        }
    }
    c->requests++;
    if (ei < 0) {
        send_exception(c, pdu[0], 0x0a); /* no device with this unit id */
        return;
    }
    entry_t *e = &s_cfg->e[ei];
    e->requests++;
    int err = mb_pdu_parse(pdu, plen, &job->req, job->regs);
    if (err) {
        e->refused++;
        send_exception(c, pdu[0], (uint8_t)err);
        return;
    }
    if (job->req.bits) {
        memcpy(job->bits, job->req.bits, (job->req.count + 7) / 8);
        job->req.bits = job->bits;
    }
    if (job->req.fc >= MB_FC_WRITE_COIL && !may_write(e, c->ip)) {
        e->refused++;
        send_exception(c, job->req.fc, 1);
        return;
    }
    uint8_t ans[MB_PDU_MAX];
    size_t dlen = sizeof(job->data);
    if (job->req.fc <= MB_FC_READ_INPUT && modbus_srv_cache(e->dev, &job->req, job->data, &dlen) == MB_OK) {
        size_t n = mb_pdu_answer(&job->req, job->data, dlen, ans, sizeof(ans));
        if (n) {
            e->cached++;
            send_pdu(c, ans, n);
            return;
        }
    }
    int64_t now = esp_timer_get_time();
    e->tokens += e->rate * (now - e->refill_us) / 1e6;
    e->refill_us = now;
    if (e->tokens > e->rate) {
        e->tokens = e->rate;
    }
    if (e->tokens < 1) {
        e->refused++;
        send_exception(c, job->req.fc, 6); /* busy: over the rate of this device */
        return;
    }
    e->tokens -= 1;
    strlcpy(job->dev, e->dev, sizeof(job->dev));
    err = modbus_srv_submit(job);
    if (err) {
        e->failed++;
        send_exception(c, job->req.fc, (uint8_t)err);
        return;
    }
    e->passed++;
    c->pending = true;
    c->entry = ei;
}

/* the job of client slot i is done; under s_lock */
static void finish(int i)
{
    client_t *c = &s_cli[i];
    const mb_srv_job_t *job = &s_job[i];
    c->pending = false;
    if (c->sock < 0) {
        return; /* gone meanwhile */
    }
    entry_t *e = &s_cfg->e[c->entry];
    uint8_t ans[MB_PDU_MAX];
    size_t n = job->result == MB_OK ? mb_pdu_answer(&job->req, job->data, job->len, ans, sizeof(ans)) : 0;
    if (n) {
        send_pdu(c, ans, n);
        return;
    }
    if (job->result <= 0) {
        e->failed++;
    }
    int code = job->result > 0 ? job->result : job->result == MB_ERR_IO ? 0x0a : 0x0b;
    send_exception(c, job->req.fc, (uint8_t)code);
}

/* complete requests in the buffer of slot i, while no job runs; under s_lock */
static void drain(int i)
{
    client_t *c = &s_cli[i];
    while (c->sock >= 0 && !c->pending && c->got >= 7) {
        size_t want = mb_tcp_expected(c->buf);
        if (!want || want > sizeof(c->buf)) {
            close_client(c); /* not Modbus TCP */
            return;
        }
        if (c->got < want) {
            return;
        }
        handle(i, want);
        memmove(c->buf, c->buf + want, c->got - want);
        c->got -= want;
    }
}

static void accept_client(int li)
{
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int s = accept(s_lst[li].sock, (struct sockaddr *)&peer, &plen);
    if (s < 0) {
        return;
    }
    int on_port = 0, slot = -1;
    for (int i = 0; i < CLIENTS_MAX; i++) {
        on_port += s_cli[i].sock >= 0 && s_cli[i].lst == li;
        if (slot < 0 && s_cli[i].sock < 0 && !s_cli[i].pending) {
            slot = i;
        }
    }
    if (slot < 0 || on_port >= CLIENTS_PER_PORT) {
        s_turned_away++;
        close(s);
        return;
    }
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    struct timeval tv = {.tv_sec = 2};
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    client_t *c = &s_cli[slot];
    *c = (client_t){.sock = s, .lst = li, .ip = peer.sin_addr.s_addr, .since_us = esp_timer_get_time()};
    c->last_us = c->since_us;
}

static void task(void *arg)
{
    uint32_t gen = 0;
    while (true) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (gen != s_gen) {
            gen = s_gen;
            rebuild();
        }
        fd_set rd;
        FD_ZERO(&rd);
        int maxfd = -1;
        bool pending = false;
        for (int i = 0; i < s_nlst; i++) {
            if (s_lst[i].sock >= 0) {
                FD_SET(s_lst[i].sock, &rd);
                maxfd = s_lst[i].sock > maxfd ? s_lst[i].sock : maxfd;
            }
        }
        for (int i = 0; i < CLIENTS_MAX; i++) {
            const client_t *c = &s_cli[i];
            pending |= c->pending;
            if (c->sock >= 0 && !c->pending) {
                FD_SET(c->sock, &rd);
                maxfd = c->sock > maxfd ? c->sock : maxfd;
            }
        }
        xSemaphoreGive(s_lock);
        struct timeval tv = {.tv_usec = pending ? 5000 : 500000};
        int n = maxfd >= 0 ? select(maxfd + 1, &rd, NULL, NULL, &tv) : (vTaskDelay(pdMS_TO_TICKS(500)), 0);
        int64_t now = esp_timer_get_time();
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (gen != s_gen) {
            xSemaphoreGive(s_lock);
            continue; /* the sockets of the sets may be gone */
        }
        for (int i = 0; n > 0 && i < s_nlst; i++) {
            if (s_lst[i].sock >= 0 && FD_ISSET(s_lst[i].sock, &rd)) {
                accept_client(i);
            }
        }
        for (int i = 0; i < CLIENTS_MAX; i++) {
            client_t *c = &s_cli[i];
            if (c->pending && s_job[i].done) {
                finish(i);
                drain(i);
            }
            if (n > 0 && c->sock >= 0 && !c->pending && FD_ISSET(c->sock, &rd)) {
                /* nothing to read: a client accepted just now on the number of a socket closed before */
                int r = recv(c->sock, c->buf + c->got, sizeof(c->buf) - c->got, MSG_DONTWAIT);
                if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                if (r <= 0) {
                    close_client(c);
                    continue;
                }
                c->got += r;
                c->last_us = now;
                drain(i);
            }
            if (c->sock >= 0 && !c->pending && now - c->last_us > IDLE_US) {
                close_client(c);
            }
        }
        xSemaphoreGive(s_lock);
    }
}

static bool start_task(void)
{
    if (!s_task && s_cfg->n && xTaskCreate(task, "mb_server", 4096, NULL, 5, &s_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the task");
        return false;
    }
    return true;
}

/* ---------------------------------------------------------------- HTTP */

static esp_err_t server_get(httpd_req_t *req)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *j = config_json(s_cfg, true);
    cJSON *ports = cJSON_AddArrayToObject(j, "ports");
    int64_t now = esp_timer_get_time();
    int clients = 0;
    for (int i = 0; i < s_nlst; i++) {
        const listener_t *l = &s_lst[i];
        cJSON *p = cJSON_CreateObject();
        cJSON_AddNumberToObject(p, "port", l->port);
        if (l->entry >= 0) {
            cJSON_AddStringToObject(p, "device", s_cfg->e[l->entry].dev);
        }
        cJSON_AddBoolToObject(p, "ok", l->sock >= 0);
        cJSON_AddStringToObject(p, "error", l->sock >= 0 ? "" : l->error);
        cJSON *cl = cJSON_AddArrayToObject(p, "clients");
        for (int k = 0; k < CLIENTS_MAX; k++) {
            const client_t *c = &s_cli[k];
            if (c->sock >= 0 && c->lst == i) {
                char ip[16];
                inet_ntoa_r(*(struct in_addr *)&c->ip, ip, sizeof(ip));
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "address", ip);
                cJSON_AddNumberToObject(o, "requests", c->requests);
                cJSON_AddNumberToObject(o, "connected_s", (double)((now - c->since_us) / 1000000));
                cJSON_AddItemToArray(cl, o);
                clients++;
            }
        }
        cJSON_AddItemToArray(ports, p);
    }
    cJSON_AddNumberToObject(j, "clients", clients);
    cJSON_AddNumberToObject(j, "turned_away", s_turned_away);
    cJSON *lim = cJSON_AddObjectToObject(j, "limits");
    cJSON_AddNumberToObject(lim, "devices", ENTRIES_MAX);
    cJSON_AddNumberToObject(lim, "ports", LISTEN_MAX);
    cJSON_AddNumberToObject(lim, "clients_per_port", CLIENTS_PER_PORT);
    cJSON_AddNumberToObject(lim, "clients", CLIENTS_MAX);
    cJSON_AddNumberToObject(lim, "rate", RATE_MAX);
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

static esp_err_t server_put(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json_max(req, 4096);
    if (!body) {
        return ESP_OK;
    }
    /* what was not sent stays */
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON *cfg = config_json(s_cfg, false);
    xSemaphoreGive(s_lock);
    const char *keys[] = {"enabled", "port", "devices"};
    for (int i = 0; i < 3; i++) {
        const cJSON *v = cJSON_GetObjectItem(body, keys[i]);
        if (v) {
            cJSON_ReplaceItemInObject(cfg, keys[i], cJSON_Duplicate(v, true));
        }
    }
    cJSON_Delete(body);
    cfg_t *next = heap_caps_calloc(1, sizeof(cfg_t), MALLOC_CAP_SPIRAM);
    const char *why = next ? parse(cfg, next, true) : "no memory";
    if (why) {
        cJSON_Delete(cfg);
        free(next);
        return gw_http_send_error(req, 400, why);
    }
    char *text = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    if (text) {
        gw_config_set_str(KEY_SRV, text);
        cJSON_free(text);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < next->n; i++) {
        /* the counters of a device stay */
        for (int k = 0; k < s_cfg->n; k++) {
            const entry_t *o = &s_cfg->e[k];
            entry_t *e = &next->e[i];
            if (!strcmp(o->dev, e->dev)) {
                e->requests = o->requests, e->cached = o->cached, e->passed = o->passed;
                e->refused = o->refused, e->failed = o->failed;
            }
        }
    }
    /* clients with a job running keep their slot until it is done; their answer is dropped */
    memcpy(s_cfg, next, sizeof(cfg_t));
    s_gen++;
    xSemaphoreGive(s_lock);
    free(next);
    start_task();
    vTaskDelay(pdMS_TO_TICKS(600)); /* the task opens the ports */
    return server_get(req);
}

/* ---------------------------------------------------------------- start */

void mb_server_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    s_cfg = heap_caps_calloc(1, sizeof(cfg_t), MALLOC_CAP_SPIRAM);
    s_job = heap_caps_calloc(CLIENTS_MAX, sizeof(mb_srv_job_t), MALLOC_CAP_SPIRAM);
    if (!s_lock || !s_cfg || !s_job) {
        ESP_LOGE(TAG, "no memory");
        return;
    }
    for (int i = 0; i < CLIENTS_MAX; i++) {
        s_cli[i].sock = -1;
    }
    s_cfg->enabled = true;
    s_cfg->port = 502;
    char *text = heap_caps_malloc(4096, MALLOC_CAP_SPIRAM);
    cJSON *j = text && gw_config_get_str(KEY_SRV, text, 4096) ? cJSON_Parse(text) : NULL;
    free(text);
    if (j) {
        cfg_t *c = heap_caps_calloc(1, sizeof(cfg_t), MALLOC_CAP_SPIRAM);
        const char *why = c ? parse(j, c, false) : "no memory";
        if (why) {
            ESP_LOGW(TAG, "stored configuration: %s", why);
        } else {
            memcpy(s_cfg, c, sizeof(cfg_t));
        }
        free(c);
        cJSON_Delete(j);
    }
    s_gen = 1;
    gw_http_register("/api/modbus/server", HTTP_GET, server_get);
    gw_http_register("/api/modbus/server", HTTP_PUT, server_put);
    gw_psram_note("modbus server", sizeof(cfg_t) + CLIENTS_MAX * sizeof(mb_srv_job_t));
    start_task();
}
