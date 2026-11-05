#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "esp_log.h"
#include "esp_sntp.h"
#include "gw.h"

static const char *TAG = "gw_time";

/* Clock of the gateway: SNTP servers and the time zone, used by everything that shows or plans by
 * local time (Telegram quiet hours, the nightly Zigbee backup, the web UI through /api/status). */
#define DEFAULT_SERVER "pool.ntp.org"
#define DEFAULT_TZ "CET-1CEST,M3.5.0,M10.5.0/3"
#define DEFAULT_TZ_NAME "Europe/Prague"
#define SERVERS_MAX 3 /* CONFIG_LWIP_SNTP_MAX_SERVERS */

/* esp_sntp keeps the pointers: the names live here */
static char s_servers[SERVERS_MAX][64];
static int s_count;
static char s_tz[64], s_tz_name[48];
static volatile time_t s_synced;

static void synced(struct timeval *tv)
{
    s_synced = tv->tv_sec;
}

static void load(void)
{
    char list[SERVERS_MAX * 64];
    if (!gw_config_get_str(GW_KEY_NTP, list, sizeof(list))) {
        strlcpy(list, DEFAULT_SERVER, sizeof(list));
    }
    s_count = 0;
    for (char *save, *t = strtok_r(list, ",", &save); t && s_count < SERVERS_MAX; t = strtok_r(NULL, ",", &save)) {
        strlcpy(s_servers[s_count++], t, sizeof(s_servers[0]));
    }
    if (!gw_config_get_str(GW_KEY_TZ, s_tz, sizeof(s_tz)) || !s_tz[0]) {
        strlcpy(s_tz, DEFAULT_TZ, sizeof(s_tz));
    }
    /* a zone set before the name was stored (Telegram settings) has no name: the UI then uses its own */
    if (!gw_config_get_str(GW_KEY_TZ_NAME, s_tz_name, sizeof(s_tz_name)) && !strcmp(s_tz, DEFAULT_TZ)) {
        strlcpy(s_tz_name, DEFAULT_TZ_NAME, sizeof(s_tz_name));
    }
    setenv("TZ", s_tz, 1);
    tzset();
}

static void apply_servers(void)
{
    if (esp_sntp_enabled()) {
        esp_sntp_stop();
    }
    for (int i = 0; i < SERVERS_MAX; i++) {
        esp_sntp_setservername(i, i < s_count ? s_servers[i] : NULL);
    }
    if (s_count) {
        esp_sntp_init();
    }
}

const char *gw_time_tz_name(void)
{
    return s_tz_name;
}

esp_err_t gw_time_set_tz(const char *tz, const char *name)
{
    if (!tz[0] || strlen(tz) >= sizeof(s_tz) || strlen(name) >= sizeof(s_tz_name)) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = gw_config_set_str(GW_KEY_TZ, tz);
    if (err == ESP_OK) {
        err = gw_config_set_str(GW_KEY_TZ_NAME, name);
    }
    load();
    return err;
}

static bool valid_server(const char *s)
{
    size_t n = strlen(s);
    if (!n || n >= sizeof(s_servers[0])) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == ':')) {
            return false;
        }
    }
    return true;
}

static esp_err_t time_get(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    time_t now = time(NULL);
    cJSON_AddNumberToObject(j, "time", now > 1700000000 ? (double)now : 0);
    cJSON_AddNumberToObject(j, "synced", (double)s_synced);
    cJSON_AddStringToObject(j, "tz", s_tz);
    cJSON_AddStringToObject(j, "tz_name", s_tz_name);
    cJSON *list = cJSON_AddArrayToObject(j, "servers");
    for (int i = 0; i < s_count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s_servers[i]);
        const ip_addr_t *a = esp_sntp_getserver(i);
        char ip[48] = "";
        if (a && !ip_addr_isany(a)) {
            ipaddr_ntoa_r(a, ip, sizeof(ip));
        }
        cJSON_AddStringToObject(o, "address", ip);
        /* RFC 5905 register: bit 0 the last poll answered */
        cJSON_AddNumberToObject(o, "reach", esp_sntp_getreachability(i));
        cJSON_AddItemToArray(list, o);
    }
    return gw_http_send_json(req, j);
}

/* {"servers": [names], "tz", "tz_name"} */
static esp_err_t time_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const cJSON *servers = cJSON_GetObjectItem(body, "servers");
    const char *tz = GW_JSON_STR(body, "tz"), *name = GW_JSON_STR(body, "tz_name");
    char list[SERVERS_MAX * 64] = "";
    if (servers) {
        int n = cJSON_IsArray(servers) ? cJSON_GetArraySize(servers) : -1;
        bool ok = n >= 0 && n <= SERVERS_MAX;
        for (int i = 0; ok && i < n; i++) {
            const cJSON *s = cJSON_GetArrayItem(servers, i);
            ok = cJSON_IsString(s) && valid_server(s->valuestring);
            if (ok) {
                if (i) {
                    strlcat(list, ",", sizeof(list));
                }
                strlcat(list, s->valuestring, sizeof(list));
            }
        }
        if (!ok) {
            cJSON_Delete(body);
            return gw_http_send_error(req, 400, "servers: up to 3 host names or addresses, empty = no time sync");
        }
    }
    if (name && !tz) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "tz_name goes with tz");
    }
    if (tz && (!tz[0] || strlen(tz) >= sizeof(s_tz) || (name && strlen(name) >= sizeof(s_tz_name)))) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "tz: POSIX time zone, e.g. CET-1CEST,M3.5.0,M10.5.0/3, max 63; tz_name: IANA name, max 47");
    }
    esp_err_t err = ESP_OK;
    if (tz) {
        err = gw_time_set_tz(tz, name ? name : "");
    }
    if (servers && err == ESP_OK) {
        err = gw_config_set_str(GW_KEY_NTP, list);
        load();
        apply_servers();
    }
    cJSON_Delete(body);
    if (err != ESP_OK) {
        return gw_http_send_error(req, 500, esp_err_to_name(err));
    }
    ESP_LOGI(TAG, "%d servers, zone %s (%s)", s_count, s_tz, s_tz_name);
    gw_notify("system", GW_NOTIFY_INFO, "Time settings changed by %s", gw_auth_who());
    return time_get(req);
}

void gw_time_init(void)
{
    load();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    sntp_set_time_sync_notification_cb(synced);
    apply_servers();
    gw_http_register("/api/time", HTTP_GET, time_get);
    gw_http_register("/api/time", HTTP_POST, time_post);
}
