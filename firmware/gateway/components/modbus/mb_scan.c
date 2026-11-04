#include "mb_scan.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "lwip/sockets.h"
#include "mb_bus.h"

/* Modbus TCP search in an IP network (docs/modbus.md): connects to the chosen ports of every address of a
 * range, asks each open port a few unit ids, and names the devices it knows. Runs in its own task, so the
 * devices keep being read. GoodWe Wi-Fi dongles answer Modbus on UDP 8899 instead: asked too. */

static const char *TAG = "mb_scan";

#define HOSTS_MAX 1024 /* /22 */
#define PARALLEL 4     /* sockets at once: the gateway has CONFIG_LWIP_MAX_SOCKETS for everything */
#define CONNECT_MS 300
#define REQUEST_MS 1000
#define FOUND_MAX 32
#define UDP_MAX 16
#define GOODWE_UDP_PORT 8899
#define GOODWE_DISCOVERY_PORT 48899

typedef struct {
    const char *manufacturer, *model, *template_id;
    uint8_t units[4]; /* tried in this order, 0 ends */
    uint16_t addr, count;
    bool serial; /* the registers are an ASCII serial number */
} fingerprint_t;

/* a device that answers this read with a serial number (at least 8 characters: devices that ignore the unit
 * id answer every read) is that device; the first match wins. VE.Bus is asked only on a GX device. */
static const fingerprint_t FINGERPRINTS[] = {
    {"Victron Energy", "GX device", "victron_gx_system", {100}, 800, 6, true},
    {"Victron Energy", "VE.Bus inverter/charger", "victron_vebus", {228, 227, 242, 246}, 3, 1, false},
    {"GoodWe", "ET/EH/BT hybrid inverter", NULL, {247}, 35003, 8, true},
    {"GoodWe", "DT/XS/D-NS inverter", NULL, {127, 247}, 30004, 8, true},
};
#define NFP (sizeof(FINGERPRINTS) / sizeof(FINGERPRINTS[0]))

typedef struct {
    uint32_t ip; /* network order */
    uint16_t port;
    uint8_t unit;   /* the first unit that answered */
    int8_t answer;  /* MB_OK or the exception code */
    int8_t fp;      /* FINGERPRINTS index, -1 none */
    uint8_t fp_unit;
    char serial[20];
    bool vebus; /* the GX also has a VE.Bus device */
    uint8_t vebus_unit;
} found_t;

static SemaphoreHandle_t s_lock;
static struct {
    bool running, stop;
    uint32_t net, mask; /* host order */
    int hosts, done;
    uint16_t ports[4];
    int nports;
    uint8_t units[8];
    int nunits;
    bool udp;
    int64_t started, finished;
    int open;
    int nfound;
    found_t found[FOUND_MAX];
    int nudp;
    struct {
        uint32_t ip;
        char info[48];
    } udp_found[UDP_MAX];
} s_scan;

static uint32_t host_ip(int i)
{
    return htonl(s_scan.net + (s_scan.mask == 0xffffffffu || s_scan.mask == 0xfffffffeu ? 0 : 1) + i);
}

static void ip_text(uint32_t ip, char *out, size_t len)
{
    const uint8_t *b = (const uint8_t *)&ip;
    snprintf(out, len, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
}

/* ---------------------------------------------------------------- TCP */

/* Connects to up to PARALLEL addresses at once; open[i] tells which accepted. */
static void connect_batch(const uint32_t *ip, const uint16_t *port, int n, bool *open)
{
    int s[PARALLEL];
    int64_t until = esp_timer_get_time() + CONNECT_MS * 1000;
    for (int i = 0; i < n; i++) {
        open[i] = false;
        s[i] = socket(AF_INET, SOCK_STREAM, 0);
        if (s[i] < 0) {
            continue;
        }
        fcntl(s[i], F_SETFL, fcntl(s[i], F_GETFL, 0) | O_NONBLOCK);
        struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port[i]), .sin_addr.s_addr = ip[i]};
        if (connect(s[i], (struct sockaddr *)&a, sizeof(a)) == 0) {
            open[i] = true;
        } else if (errno != EINPROGRESS) {
            close(s[i]);
            s[i] = -1;
        }
    }
    while (true) {
        fd_set wr;
        FD_ZERO(&wr);
        int maxfd = -1;
        for (int i = 0; i < n; i++) {
            if (s[i] >= 0 && !open[i]) {
                FD_SET(s[i], &wr);
                maxfd = s[i] > maxfd ? s[i] : maxfd;
            }
        }
        int64_t left = until - esp_timer_get_time();
        if (maxfd < 0 || left <= 0) {
            break;
        }
        struct timeval tv = {.tv_sec = 0, .tv_usec = (long)left};
        if (select(maxfd + 1, NULL, &wr, NULL, &tv) <= 0) {
            break;
        }
        for (int i = 0; i < n; i++) {
            if (s[i] >= 0 && FD_ISSET(s[i], &wr)) {
                int err = 0;
                socklen_t len = sizeof(err);
                getsockopt(s[i], SOL_SOCKET, SO_ERROR, &err, &len);
                if (err == 0) {
                    open[i] = true;
                } else {
                    close(s[i]); /* refused */
                    s[i] = -1;
                }
            }
        }
    }
    for (int i = 0; i < n; i++) {
        if (s[i] >= 0) {
            close(s[i]);
        }
    }
}

static int read_regs(mb_tcp_t *c, const char *host, uint16_t port, uint8_t unit, uint16_t addr, uint16_t count, uint8_t *data,
                     size_t *len)
{
    mb_req_t req = {.fc = MB_FC_READ_HOLDING, .addr = addr, .count = count};
    return mb_tcp_request(c, host, port, unit, &req, REQUEST_MS, data, len);
}

/* Asks an open port the unit ids, then the fingerprints; keeps it when something speaks Modbus. */
static void probe_port(uint32_t ip, uint16_t port)
{
    char host[16];
    ip_text(ip, host, sizeof(host));
    found_t f = {.ip = ip, .port = port, .fp = -1};
    mb_tcp_t c;
    mb_tcp_init(&c);
    uint8_t data[2 * 8];
    bool modbus = false;
    for (int i = 0; i < s_scan.nunits && !modbus && !s_scan.stop; i++) {
        size_t len = sizeof(data);
        int r = read_regs(&c, host, port, s_scan.units[i], 0, 1, data, &len);
        /* an exception is an answer too; 10 and 11 are a gateway without this unit, still Modbus */
        if (r >= 0) {
            modbus = true;
            f.unit = s_scan.units[i];
            f.answer = (int8_t)r;
        } else if (r == MB_ERR_IO && c.sock < 0) {
            break; /* closed on us: not Modbus */
        }
    }
    for (int i = 0; modbus && i < (int)NFP && f.fp < 0 && !s_scan.stop; i++) {
        const fingerprint_t *p = &FINGERPRINTS[i];
        for (int u = 0; p->serial && u < 4 && p->units[u] && f.fp < 0; u++) {
            size_t len = sizeof(data), n = 0;
            if (read_regs(&c, host, port, p->units[u], p->addr, p->count, data, &len) != MB_OK) {
                continue;
            }
            for (size_t k = 0; k < len && n + 1 < sizeof(f.serial); k++) {
                if (data[k] > ' ' && data[k] < 0x7f) {
                    f.serial[n++] = (char)data[k];
                }
            }
            f.serial[n] = '\0';
            if (n >= 8) {
                f.fp = (int8_t)i;
                f.fp_unit = p->units[u];
            }
        }
    }
    if (f.fp < 0) {
        f.serial[0] = '\0';
    }
    /* a GX device: its VE.Bus inverter/charger too */
    if (f.fp == 0) {
        const fingerprint_t *p = &FINGERPRINTS[1];
        for (int u = 0; u < 4 && p->units[u] && !f.vebus; u++) {
            size_t len = sizeof(data);
            if (read_regs(&c, host, port, p->units[u], p->addr, p->count, data, &len) == MB_OK) {
                f.vebus = true;
                f.vebus_unit = p->units[u];
            }
        }
    }
    mb_tcp_close(&c);
    if (modbus) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (s_scan.nfound < FOUND_MAX) {
            s_scan.found[s_scan.nfound++] = f;
        }
        xSemaphoreGive(s_lock);
    }
}

/* ---------------------------------------------------------------- UDP (GoodWe) */

static void udp_note(uint32_t ip, const char *info)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_scan.nudp; i++) {
        if (s_scan.udp_found[i].ip == ip) {
            xSemaphoreGive(s_lock);
            return;
        }
    }
    if (s_scan.nudp < UDP_MAX) {
        s_scan.udp_found[s_scan.nudp].ip = ip;
        strlcpy(s_scan.udp_found[s_scan.nudp].info, info, sizeof(s_scan.udp_found[0].info));
        s_scan.nudp++;
    }
    xSemaphoreGive(s_lock);
}

/* GoodWe: the discovery broadcast (answer "ip,mac,name") and a Modbus RTU read on UDP 8899 to every address
 * (DT/XS unit 0x7F register 30001, ET unit 0xF7 register 35000; the answer starts with AA 55). */
static void udp_probe(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return;
    }
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    struct sockaddr_in a = {.sin_family = AF_INET};
    static const char discovery[] = "WIFIKIT-214028-READ";
    a.sin_port = htons(GOODWE_DISCOVERY_PORT);
    a.sin_addr.s_addr = htonl(s_scan.net | ~s_scan.mask);
    sendto(s, discovery, sizeof(discovery) - 1, 0, (struct sockaddr *)&a, sizeof(a));
    uint8_t frames[2][8];
    const struct {
        uint8_t unit;
        uint16_t addr;
    } q[2] = {{0x7f, 30001}, {0xf7, 35000}};
    for (int k = 0; k < 2; k++) {
        uint8_t pdu[5] = {MB_FC_READ_HOLDING, q[k].addr >> 8, q[k].addr & 0xff, 0, 1};
        mb_rtu_frame(q[k].unit, pdu, sizeof(pdu), frames[k], sizeof(frames[k]));
    }
    a.sin_port = htons(GOODWE_UDP_PORT);
    int64_t until = 0;
    for (int i = 0; i <= s_scan.hosts && !s_scan.stop; i++) {
        if (i < s_scan.hosts) {
            a.sin_addr.s_addr = host_ip(i);
            for (int k = 0; k < 2; k++) {
                sendto(s, frames[k], sizeof(frames[k]), 0, (struct sockaddr *)&a, sizeof(a));
            }
        } else {
            until = esp_timer_get_time() + 2000000; /* answers after the last request */
        }
        do {
            uint8_t buf[128];
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            struct timeval tv = {.tv_sec = 0, .tv_usec = i < s_scan.hosts ? 5000 : 200000};
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
            int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) {
                continue;
            }
            buf[n] = '\0';
            if (ntohs(from.sin_port) == GOODWE_DISCOVERY_PORT) {
                udp_note(from.sin_addr.s_addr, (const char *)buf); /* "ip,mac,name" */
            } else if (n >= 2 && buf[0] == 0xaa && buf[1] == 0x55) {
                udp_note(from.sin_addr.s_addr, "Modbus RTU on UDP 8899");
            }
        } while (esp_timer_get_time() < until && !s_scan.stop);
    }
    close(s);
}

/* ---------------------------------------------------------------- task */

static void scan_task(void *arg)
{
    uint32_t ip[PARALLEL];
    uint16_t port[PARALLEL];
    bool open[PARALLEL];
    int total = s_scan.hosts * s_scan.nports, next = 0;
    while (next < total && !s_scan.stop) {
        int n = 0;
        while (n < PARALLEL && next < total) {
            ip[n] = host_ip(next / s_scan.nports);
            port[n] = s_scan.ports[next % s_scan.nports];
            next++, n++;
        }
        connect_batch(ip, port, n, open);
        for (int i = 0; i < n && !s_scan.stop; i++) {
            if (open[i]) {
                s_scan.open++;
                probe_port(ip[i], port[i]);
            }
        }
        s_scan.done = next;
    }
    if (s_scan.udp && !s_scan.stop) {
        udp_probe();
    }
    ESP_LOGI(TAG, "done: %d of %d addresses, %d open, %d Modbus, %d UDP", s_scan.done, total, s_scan.open, s_scan.nfound, s_scan.nudp);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_scan.finished = esp_timer_get_time();
    s_scan.running = false;
    xSemaphoreGive(s_lock);
    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------- API */

/* the network of the interface carrying the traffic, as "a.b.c.d/n" (at least /22) */
static void default_net(char *out, size_t len)
{
    esp_netif_ip_info_t info = {0};
    esp_netif_t *nif = esp_netif_get_default_netif();
    if (!nif || esp_netif_get_ip_info(nif, &info) != ESP_OK || !info.ip.addr) {
        out[0] = '\0';
        return;
    }
    uint32_t mask = ntohl(info.netmask.addr);
    int prefix = __builtin_popcount(mask);
    if (prefix < 22) {
        prefix = 22;
        mask = 0xffffffffu << 10;
    }
    char ip[16];
    ip_text(htonl(ntohl(info.ip.addr) & mask), ip, sizeof(ip));
    snprintf(out, len, "%s/%d", ip, prefix);
}

/* "a.b.c.d/n" or "a.b.c.d" (one address) */
static const char *parse_net(const char *text, uint32_t *net, uint32_t *mask)
{
    unsigned b[4], prefix = 32;
    char extra;
    int n = sscanf(text, "%u.%u.%u.%u/%u%c", &b[0], &b[1], &b[2], &b[3], &prefix, &extra);
    if ((n != 4 && n != 5) || b[0] > 255 || b[1] > 255 || b[2] > 255 || b[3] > 255) {
        return "net: a.b.c.d/n";
    }
    if (prefix < 22 || prefix > 32) {
        return "net: prefix 22-32 (up to 1024 addresses)";
    }
    *mask = 0xffffffffu << (32 - prefix);
    *net = (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]) & *mask;
    return NULL;
}

/* POST {"net": "192.168.1.0/24", "ports": [502], "units": [1, 247, 255], "udp": true} or {"stop": true} */
static esp_err_t scan_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItem(body, "stop"))) {
        s_scan.stop = true;
        cJSON_Delete(body);
        return gw_http_send_json(req, cJSON_CreateObject());
    }
    const char *net_text = GW_JSON_STR(body, "net");
    const cJSON *ports = cJSON_GetObjectItem(body, "ports"), *units = cJSON_GetObjectItem(body, "units"), *v;
    uint32_t net = 0, mask = 0;
    const char *why = s_scan.running ? "a search is running" : !net_text ? "net required" : parse_net(net_text, &net, &mask);
    if (!why && ports && (!cJSON_IsArray(ports) || cJSON_GetArraySize(ports) < 1 || cJSON_GetArraySize(ports) > 4)) {
        why = "ports: a list of 1-4 ports";
    }
    if (!why && units && (!cJSON_IsArray(units) || cJSON_GetArraySize(units) < 1 || cJSON_GetArraySize(units) > 8)) {
        why = "units: a list of 1-8 unit ids";
    }
    cJSON_ArrayForEach(v, ports)
    {
        why = why ? why : !cJSON_IsNumber(v) || v->valueint < 1 || v->valueint > 65535 ? "ports: 1-65535" : NULL;
    }
    cJSON_ArrayForEach(v, units)
    {
        why = why ? why : !cJSON_IsNumber(v) || v->valueint < 0 || v->valueint > 255 ? "units: 0-255" : NULL;
    }
    if (why) {
        cJSON_Delete(body);
        return gw_http_send_error(req, s_scan.running ? 409 : 400, why);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    memset(&s_scan, 0, sizeof(s_scan));
    s_scan.net = net, s_scan.mask = mask;
    uint32_t size = ~mask + 1u;
    s_scan.hosts = size <= 2 ? (int)size : (int)size - 2; /* not the network and broadcast addresses */
    cJSON_ArrayForEach(v, ports)
    {
        s_scan.ports[s_scan.nports++] = (uint16_t)v->valueint;
    }
    if (!s_scan.nports) {
        s_scan.ports[s_scan.nports++] = 502;
    }
    cJSON_ArrayForEach(v, units)
    {
        s_scan.units[s_scan.nunits++] = (uint8_t)v->valueint;
    }
    if (!s_scan.nunits) {
        static const uint8_t def[] = {1, 247, 255};
        memcpy(s_scan.units, def, sizeof(def));
        s_scan.nunits = sizeof(def);
    }
    const cJSON *udp = cJSON_GetObjectItem(body, "udp");
    s_scan.udp = !udp || cJSON_IsTrue(udp);
    s_scan.started = esp_timer_get_time();
    s_scan.running = true;
    xSemaphoreGive(s_lock);
    ESP_LOGI(TAG, "searching %s, %d ports", net_text, s_scan.nports);
    cJSON_Delete(body);
    if (xTaskCreate(scan_task, "mb_scan", 4096, NULL, 2, NULL) != pdPASS) {
        s_scan.running = false;
        return gw_http_send_error(req, 503, "no memory for the search");
    }
    return gw_http_send_json(req, cJSON_CreateObject());
}

static esp_err_t scan_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    char text[24];
    default_net(text, sizeof(text));
    cJSON_AddStringToObject(j, "default_net", text);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cJSON_AddBoolToObject(j, "running", s_scan.running);
    if (s_scan.started) {
        ip_text(htonl(s_scan.net), text, sizeof(text));
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "/%d", __builtin_popcount(s_scan.mask));
        cJSON_AddStringToObject(j, "net", text);
        int total = s_scan.hosts * s_scan.nports;
        cJSON_AddNumberToObject(j, "progress", total ? s_scan.done * 100 / total : 100);
        cJSON_AddNumberToObject(j, "addresses", s_scan.hosts);
        cJSON_AddNumberToObject(j, "open", s_scan.open);
        cJSON_AddBoolToObject(j, "stopped", s_scan.stop);
        int64_t end = s_scan.running ? esp_timer_get_time() : s_scan.finished;
        cJSON_AddNumberToObject(j, "duration_ms", (double)((end - s_scan.started) / 1000));
    }
    cJSON *found = cJSON_AddArrayToObject(j, "found");
    for (int i = 0; i < s_scan.nfound; i++) {
        const found_t *f = &s_scan.found[i];
        cJSON *o = cJSON_CreateObject();
        ip_text(f->ip, text, sizeof(text));
        cJSON_AddStringToObject(o, "host", text);
        cJSON_AddNumberToObject(o, "port", f->port);
        cJSON_AddNumberToObject(o, "unit", f->unit);
        cJSON_AddNumberToObject(o, "answer", f->answer); /* 0 data, else the exception code */
        if (f->fp >= 0) {
            const fingerprint_t *p = &FINGERPRINTS[f->fp];
            cJSON_AddStringToObject(o, "manufacturer", p->manufacturer);
            cJSON_AddStringToObject(o, "model", p->model);
            cJSON_AddNumberToObject(o, "device_unit", f->fp_unit);
            if (p->template_id) {
                cJSON_AddStringToObject(o, "template", p->template_id);
            }
            if (f->serial[0]) {
                cJSON_AddStringToObject(o, "serial", f->serial);
            }
        }
        if (f->vebus) {
            cJSON_AddNumberToObject(o, "vebus_unit", f->vebus_unit);
        }
        cJSON_AddItemToArray(found, o);
    }
    cJSON *udp = cJSON_AddArrayToObject(j, "udp");
    for (int i = 0; i < s_scan.nudp; i++) {
        cJSON *o = cJSON_CreateObject();
        ip_text(s_scan.udp_found[i].ip, text, sizeof(text));
        cJSON_AddStringToObject(o, "host", text);
        cJSON_AddStringToObject(o, "info", s_scan.udp_found[i].info);
        cJSON_AddItemToArray(udp, o);
    }
    xSemaphoreGive(s_lock);
    return gw_http_send_json(req, j);
}

void mb_scan_init(void)
{
    s_lock = xSemaphoreCreateMutex();
    gw_http_register("/api/modbus/scan", HTTP_GET, scan_get);
    gw_http_register("/api/modbus/scan", HTTP_POST, scan_post);
}
