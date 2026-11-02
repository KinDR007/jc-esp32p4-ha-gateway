#include <string.h>

#include "cJSON.h"
#include "driver/uart.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_hosted_transport_config.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"
#include "lwip/ip6_addr.h"
#include "role_wifi.h"

static const char *TAG = "role_wifi";

#define KEY_CFG "upw"       /* str JSON {"ssid", "mode": "dhcp"|"static", "ip", "mask", "gw", "dns1", "dns2"} */
#define KEY_PASS "upw_pass" /* secret setting (gw_config) */
#define KEY_ESPH_SSID "espw_ssid" /* the ESPHome Wi-Fi, used while the uplink has none of its own */
#define KEY_ESPH_PASS "espw_pass"

#define UART_BAUD 921600  /* firmware/c6/wifi_uplink/sdkconfig.overlay */
#define POLL_MS 5000      /* signal and channel refresh while connected */
#define BRINGUP_RETRY_S 30
#define RETRY_MAX_S 60

#define EV_APPLY BIT0 /* worker task notification: settings changed */

static struct {
    c6_radio_t radio;
    TaskHandle_t task;
    esp_netif_t *netif;
    bool up;              /* host and Wi-Fi driver running */
    const char *state;    /* off, starting, radio_unreachable, no_settings, connecting, connected, disconnected */
    char error[96];
    char cp_version[24];
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    char phy[8];
    uint8_t reason;       /* last disconnect reason (wifi_err_reason_t) */
    uint32_t connects, disconnects, retries;
    int64_t connected_us, retry_at_us;
    bool has_ip;
    bool fixed;           /* static IPv4 */
} s_wifi = {.state = "off"};

/* ---- settings ---- */

typedef struct {
    char ssid[33];
    char pass[65];
    bool own;   /* the uplink's own setting, else the ESPHome one */
    bool fixed;
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t dns[2];
} wifi_cfg_t;

static void cfg_load(wifi_cfg_t *cfg)
{
    char text[256], err[64];
    memset(cfg, 0, sizeof(*cfg));
    cJSON *json = gw_config_get_str(KEY_CFG, text, sizeof(text)) ? cJSON_Parse(text) : NULL;
    const char *ssid = GW_JSON_STR(json, "ssid");
    if (ssid && ssid[0]) {
        cfg->own = true;
        strlcpy(cfg->ssid, ssid, sizeof(cfg->ssid));
        gw_config_get_str(KEY_PASS, cfg->pass, sizeof(cfg->pass));
    } else {
        gw_config_get_str(KEY_ESPH_SSID, cfg->ssid, sizeof(cfg->ssid));
        gw_config_get_str(KEY_ESPH_PASS, cfg->pass, sizeof(cfg->pass));
    }
    if (!json || !gw_net_parse_ipv4(json, &cfg->fixed, &cfg->ip, cfg->dns, err, sizeof(err))) {
        cfg->fixed = false;
    }
    cJSON_Delete(json);
}

static cJSON *cfg_json(const wifi_cfg_t *cfg)
{
    char buf[16];
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "ssid", cfg->ssid);
    cJSON_AddBoolToObject(json, "password_set", cfg->pass[0] != '\0');
    cJSON_AddBoolToObject(json, "from_esphome", !cfg->own && cfg->ssid[0]);
    cJSON_AddStringToObject(json, "mode", cfg->fixed ? "static" : "dhcp");
    if (cfg->fixed) {
        const esp_ip4_addr_t *addrs[] = {&cfg->ip.ip, &cfg->ip.netmask, &cfg->ip.gw, &cfg->dns[0], &cfg->dns[1]};
        const char *keys[] = {"ip", "mask", "gw", "dns1", "dns2"};
        for (int i = 0; i < 5; i++) {
            if (addrs[i]->addr) {
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(addrs[i]));
                cJSON_AddStringToObject(json, keys[i], buf);
            }
        }
    }
    return json;
}

/* ---- the station ---- */

static const char *reason_text(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_NO_AP_FOUND:
    case WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY:
    case WIFI_REASON_NO_AP_FOUND_IN_AUTHMODE_THRESHOLD:
    case WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        return "network not found";
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return "wrong password";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "signal lost";
    case WIFI_REASON_ASSOC_LEAVE:
        return "left the network";
    default:
        return "connection failed";
    }
}

static void apply_ip(const wifi_cfg_t *cfg)
{
    s_wifi.fixed = cfg->fixed;
    if (!cfg->fixed) {
        esp_netif_dhcpc_start(s_wifi.netif);
        return;
    }
    esp_netif_dhcpc_stop(s_wifi.netif);
    esp_netif_set_ip_info(s_wifi.netif, &cfg->ip);
    for (int i = 0; i < 2; i++) {
        if (cfg->dns[i].addr) {
            esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4 = cfg->dns[i]};
            esp_netif_set_dns_info(s_wifi.netif, i ? ESP_NETIF_DNS_BACKUP : ESP_NETIF_DNS_MAIN, &dns);
        }
    }
}

/* Takes the settings and joins the network (or stays off without an SSID). */
static void station_apply(void)
{
    wifi_cfg_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        return;
    }
    cfg_load(cfg);
    esp_wifi_disconnect();
    strlcpy(s_wifi.ssid, cfg->ssid, sizeof(s_wifi.ssid));
    if (!cfg->ssid[0]) {
        s_wifi.state = "no_settings";
        strlcpy(s_wifi.error, "no Wi-Fi network set", sizeof(s_wifi.error));
        free(cfg);
        return;
    }
    wifi_config_t wc = {0};
    memcpy(wc.sta.ssid, cfg->ssid, strlen(cfg->ssid));
    memcpy(wc.sta.password, cfg->pass, strlen(cfg->pass));
    wc.sta.threshold.authmode = cfg->pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wc.sta.scan_method = WIFI_ALL_CHANNEL_SCAN; /* the strongest access point of the network */
    wc.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wc.sta.pmf_cfg.capable = true;
    wc.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &wc);
    apply_ip(cfg);
    memset(cfg->pass, 0, sizeof(cfg->pass));
    memset(&wc, 0, sizeof(wc));
    free(cfg);
    if (err != ESP_OK) {
        s_wifi.state = "disconnected";
        snprintf(s_wifi.error, sizeof(s_wifi.error), "settings refused by the radio: %s", esp_err_to_name(err));
        return;
    }
    s_wifi.state = "connecting";
    s_wifi.error[0] = '\0';
    s_wifi.retries = 0;
    s_wifi.retry_at_us = 0;
    esp_wifi_connect();
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_CONNECTED) {
        wifi_event_sta_connected_t *e = data;
        memcpy(s_wifi.bssid, e->bssid, sizeof(s_wifi.bssid));
        s_wifi.channel = e->channel;
        s_wifi.connects++;
        s_wifi.connected_us = esp_timer_get_time();
        s_wifi.retries = 0;
        s_wifi.error[0] = '\0';
        s_wifi.state = "connected";
        gw_traffic_hook(GW_TRAFFIC_WIFI, s_wifi.netif); /* the lwIP netif has its input function once up */
        esp_netif_create_ip6_linklocal(s_wifi.netif);
        ESP_LOGI(TAG, "joined %s, channel %u", s_wifi.ssid, e->channel);
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        bool was_connected = s_wifi.connected_us != 0;
        s_wifi.reason = e->reason;
        s_wifi.rssi = 0;
        s_wifi.connected_us = 0;
        s_wifi.has_ip = false;
        gw_net_uplink_ip(false);
        if (strcmp(s_wifi.state, "no_settings") == 0 || e->reason == WIFI_REASON_ASSOC_LEAVE) {
            return; /* left on purpose: new settings */
        }
        s_wifi.state = "disconnected";
        snprintf(s_wifi.error, sizeof(s_wifi.error), "%s (reason %u)", reason_text(e->reason), e->reason);
        if (was_connected) {
            s_wifi.disconnects++;
            gw_notify("network", GW_NOTIFY_WARNING, "Wi-Fi uplink lost %s: %s", s_wifi.ssid, s_wifi.error);
        }
        /* back off: 2, 4, 8 ... 60 s */
        uint32_t delay_s = s_wifi.retries < 5 ? 2u << s_wifi.retries : RETRY_MAX_S;
        s_wifi.retries++;
        s_wifi.retry_at_us = esp_timer_get_time() + (int64_t)(delay_s > RETRY_MAX_S ? RETRY_MAX_S : delay_s) * 1000000;
        ESP_LOGW(TAG, "%s: %s, retry in %lu s", s_wifi.ssid, s_wifi.error, (unsigned long)delay_s);
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        s_wifi.has_ip = true;
        gw_net_uplink_ip(true);
        ESP_LOGI(TAG, "IPv4 " IPSTR, IP2STR(&e->ip_info.ip));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_wifi.has_ip = false;
        gw_net_uplink_ip(false);
    } else if (id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *e = data;
        if (e->esp_netif == s_wifi.netif) {
            char addr[48];
            ip6addr_ntoa_r((const ip6_addr_t *)&e->ip6_info.ip, addr, sizeof(addr));
            ESP_LOGI(TAG, "IPv6 %s", addr);
        }
    }
}

static void on_hosted_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE && s_wifi.up) {
        s_wifi.state = "radio_unreachable";
        strlcpy(s_wifi.error, "the link to the radio failed", sizeof(s_wifi.error));
        s_wifi.has_ip = false;
        gw_net_uplink_ip(false);
        gw_notify("network", GW_NOTIFY_WARNING, "Wi-Fi uplink: the link to radio %c failed", 'A' + s_wifi.radio);
    }
}

/* The radio's ROM prints its boot banner at 115200 Bd on the same pins. At the transport's speed it is
 * noise that can hold the frame decoder (a false header waiting for bytes that never come), so it is
 * dropped up to the first quiet gap; the co-processor's INIT comes well after it. */
static void skip_boot_banner(int port)
{
    size_t len = 0, prev = 0;
    int64_t now = esp_timer_get_time(), end = now + 1000000, changed = now;
    while ((now = esp_timer_get_time()) < end) {
        vTaskDelay(pdMS_TO_TICKS(5));
        uart_get_buffered_data_len(port, &len);
        if (len != prev) {
            prev = len;
            changed = now;
        } else if (len && now - changed > 20000) {
            break;
        }
    }
    ESP_LOGI(TAG, "boot banner: %u B dropped", (unsigned)len);
    uart_flush_input(port);
}

/* The host on the radio's UART: the radio is held in reset until the UART listens, so its first
 * message (the co-processor's INIT) is not lost; the host itself has no reset line (radio B's reset is
 * open drain, the flasher drives it). */
static esp_err_t host_up(void)
{
    const c6_radio_pins_t *p = c6_radio_pins(s_wifi.radio);
    struct esp_hosted_uart_config cfg = INIT_DEFAULT_HOST_UART_CONFIG();
    cfg.port = p->uart_port;
    cfg.pin_tx.pin = p->tx;
    cfg.pin_rx.pin = p->rx;
    cfg.pin_reset.pin = -1;
    cfg.baud_rate = UART_BAUD;
    esp_hosted_transport_err_t terr = esp_hosted_uart_set_config(&cfg);
    if (terr != ESP_TRANSPORT_OK && terr != ESP_TRANSPORT_ERR_ALREADY_SET) {
        snprintf(s_wifi.error, sizeof(s_wifi.error), "UART settings refused (%d)", (int)terr);
        return ESP_FAIL;
    }
    c6_hold_reset(s_wifi.radio, true);
    int rc = esp_hosted_init();
    vTaskDelay(pdMS_TO_TICKS(20));
    c6_hold_reset(s_wifi.radio, false);
    if (rc == 0) {
        skip_boot_banner(p->uart_port);
        rc = esp_hosted_connect_to_slave();
    }
    if (rc != 0) {
        esp_hosted_deinit();
        snprintf(s_wifi.error, sizeof(s_wifi.error), "no answer from the radio (%d): does it run wifi_uplink?", rc);
        return ESP_FAIL;
    }
    esp_hosted_coprocessor_fwver_t ver = {0};
    if (esp_hosted_get_coprocessor_fwversion(&ver) == 0) {
        snprintf(s_wifi.cp_version, sizeof(s_wifi.cp_version), "%u.%u.%u", (unsigned)ver.major1, (unsigned)ver.minor1,
                 (unsigned)ver.patch1);
    }
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init);
    if (err == ESP_OK) {
        esp_wifi_set_storage(WIFI_STORAGE_RAM);
        err = esp_wifi_set_mode(WIFI_MODE_STA);
    }
    if (err == ESP_OK) {
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        snprintf(s_wifi.error, sizeof(s_wifi.error), "Wi-Fi on the radio did not start: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static void refresh(void)
{
    wifi_ap_record_t ap;
    if (s_wifi.connected_us && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        s_wifi.rssi = ap.rssi;
        s_wifi.channel = ap.primary;
        strlcpy(s_wifi.phy, ap.phy_11ax ? "11ax" : ap.phy_11ac ? "11ac" : ap.phy_11n ? "11n" : ap.phy_11g ? "11g" : "11b",
                sizeof(s_wifi.phy));
    }
}

static void wifi_task(void *arg)
{
    s_wifi.state = "starting";
    while (host_up() != ESP_OK) {
        s_wifi.state = "radio_unreachable";
        ESP_LOGW(TAG, "radio %c: %s", 'A' + s_wifi.radio, s_wifi.error);
        vTaskDelay(pdMS_TO_TICKS(BRINGUP_RETRY_S * 1000));
        s_wifi.state = "starting";
    }
    s_wifi.up = true;
    ESP_LOGI(TAG, "radio %c: co-processor %s up", 'A' + s_wifi.radio, s_wifi.cp_version);
    station_apply();
    while (true) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(POLL_MS));
        if (ev & EV_APPLY) {
            station_apply();
        } else if (s_wifi.retry_at_us && esp_timer_get_time() >= s_wifi.retry_at_us) {
            s_wifi.retry_at_us = 0;
            s_wifi.state = "connecting";
            esp_wifi_connect();
        }
        refresh();
    }
}

static cJSON *wifi_status(void)
{
    char buf[48];
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "mode", "wifi_uplink");
    cJSON_AddStringToObject(j, "state", s_wifi.state);
    cJSON_AddStringToObject(j, "error", s_wifi.error);
    cJSON_AddStringToObject(j, "radio", s_wifi.radio == C6_RADIO_B ? "b" : "a");
    cJSON_AddStringToObject(j, "cp_version", s_wifi.cp_version);
    cJSON_AddNumberToObject(j, "baud", UART_BAUD);
    cJSON_AddStringToObject(j, "ssid", s_wifi.ssid);
    if (s_wifi.connected_us) {
        snprintf(buf, sizeof(buf), MACSTR, MAC2STR(s_wifi.bssid));
        cJSON_AddStringToObject(j, "bssid", buf);
        cJSON_AddNumberToObject(j, "rssi", s_wifi.rssi);
        cJSON_AddNumberToObject(j, "channel", s_wifi.channel);
        cJSON_AddStringToObject(j, "band", s_wifi.channel > 14 ? "5 GHz" : "2.4 GHz");
        cJSON_AddStringToObject(j, "phy", s_wifi.phy);
        cJSON_AddNumberToObject(j, "connected_s", (double)((esp_timer_get_time() - s_wifi.connected_us) / 1000000));
    }
    cJSON_AddNumberToObject(j, "connects", s_wifi.connects);
    cJSON_AddNumberToObject(j, "disconnects", s_wifi.disconnects);
    cJSON_AddNumberToObject(j, "last_reason", s_wifi.reason);
    cJSON_AddStringToObject(j, "ip_mode", s_wifi.fixed ? "static" : "dhcp");
    cJSON_AddBoolToObject(j, "default_route", s_wifi.netif && esp_netif_get_default_netif() == s_wifi.netif);
    if (s_wifi.netif) {
        uint8_t mac[6];
        if (esp_netif_get_mac(s_wifi.netif, mac) == ESP_OK) {
            snprintf(buf, sizeof(buf), MACSTR, MAC2STR(mac));
            cJSON_AddStringToObject(j, "mac", buf);
        }
        esp_netif_ip_info_t ip;
        if (s_wifi.has_ip && esp_netif_get_ip_info(s_wifi.netif, &ip) == ESP_OK) {
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
            cJSON_AddStringToObject(j, "ipv4", buf);
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.gw));
            cJSON_AddStringToObject(j, "gateway", buf);
            snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.netmask));
            cJSON_AddStringToObject(j, "netmask", buf);
        }
        cJSON *ip6 = cJSON_AddArrayToObject(j, "ipv6");
        esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
        int n = esp_netif_get_all_ip6(s_wifi.netif, addrs);
        for (int i = 0; i < n; i++) {
            ip6addr_ntoa_r((const ip6_addr_t *)&addrs[i], buf, sizeof(buf));
            cJSON_AddItemToArray(ip6, cJSON_CreateString(buf));
        }
    }
    return j;
}

/* ---- API ---- */

static esp_err_t wifi_get(httpd_req_t *req)
{
    wifi_cfg_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        return gw_http_send_error(req, 500, "no memory");
    }
    cfg_load(cfg);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddItemToObject(j, "settings", cfg_json(cfg));
    memset(cfg, 0, sizeof(*cfg));
    free(cfg);
    cJSON_AddItemToObject(j, "status", wifi_status());
    return gw_http_send_json(req, j);
}

/* {"ssid", "password" (left out: kept, "" open network), "mode", "ip", "mask", "gw", "dns1", "dns2"};
 * applied at once when the uplink runs. */
static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *ssid = GW_JSON_STR(body, "ssid"), *pass = GW_JSON_STR(body, "password");
    char err[80] = "ssid: 1-32 characters, password: 8-64 characters or empty for an open network";
    bool fixed;
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t dns[2];
    bool valid = ssid && ssid[0] && strlen(ssid) <= 32 && (!pass || (strlen(pass) <= 64 && (!pass[0] || strlen(pass) >= 8)));
    if (!valid || !gw_net_parse_ipv4(body, &fixed, &ip, dns, err, sizeof(err))) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, err);
    }
    if (pass) {
        gw_config_set_str(KEY_PASS, pass);
    } else {
        /* a new network without a password of its own would take the one of the previous network */
        wifi_cfg_t *old = calloc(1, sizeof(*old));
        if (old) {
            cfg_load(old);
            if (!old->own) {
                gw_config_set_str(KEY_PASS, old->pass);
            }
            memset(old, 0, sizeof(*old));
            free(old);
        }
    }
    cJSON *store = cJSON_CreateObject();
    cJSON_AddStringToObject(store, "ssid", ssid);
    cJSON_AddStringToObject(store, "mode", fixed ? "static" : "dhcp");
    static const char *const keys[] = {"ip", "mask", "gw", "dns1", "dns2"};
    for (int i = 0; fixed && i < 5; i++) {
        const char *v = GW_JSON_STR(body, keys[i]);
        if (v && v[0]) {
            cJSON_AddStringToObject(store, keys[i], v);
        }
    }
    char *text = cJSON_PrintUnformatted(store);
    cJSON_Delete(store);
    cJSON_Delete(body);
    if (!text) {
        return gw_http_send_error(req, 500, "no memory");
    }
    gw_config_set_str(KEY_CFG, text);
    cJSON_free(text);
    gw_notify("network", GW_NOTIFY_INFO, "Wi-Fi uplink settings changed by %s: %s", gw_auth_who()[0] ? gw_auth_who() : "the web UI",
              fixed ? "static address" : "DHCP");
    if (s_wifi.task && s_wifi.up) {
        xTaskNotify(s_wifi.task, EV_APPLY, eSetBits);
    }
    return wifi_get(req);
}

static esp_err_t wifi_traffic_get(httpd_req_t *req)
{
    return gw_http_send_json(req, gw_traffic_json(GW_TRAFFIC_WIFI));
}

/* Networks in range, from the radio (only while the uplink runs). */
static esp_err_t wifi_scan_get(httpd_req_t *req)
{
    if (!s_wifi.up) {
        return gw_http_send_error(req, 409, "the Wi-Fi uplink does not run");
    }
    wifi_scan_config_t sc = {.show_hidden = false};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        return gw_http_send_error(req, 500, esp_err_to_name(err));
    }
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n > 40) {
        n = 40;
    }
    wifi_ap_record_t *aps = calloc(n ? n : 1, sizeof(*aps));
    if (!aps) {
        esp_wifi_clear_ap_list();
        return gw_http_send_error(req, 500, "no memory");
    }
    esp_wifi_scan_get_ap_records(&n, aps);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", (const char *)aps[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddNumberToObject(a, "channel", aps[i].primary);
        cJSON_AddBoolToObject(a, "open", aps[i].authmode == WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(list, a);
    }
    free(aps);
    return gw_http_send_json(req, list);
}

void role_wifi_init(void)
{
    gw_http_register("/api/net/wifi", HTTP_GET, wifi_get);
    gw_http_register("/api/net/wifi", HTTP_POST, wifi_post);
    gw_http_register("/api/net/wifi/traffic", HTTP_GET, wifi_traffic_get);
    gw_http_register("/api/net/wifi/scan", HTTP_GET, wifi_scan_get);
}

esp_err_t role_wifi_start(c6_radio_t radio)
{
    if (s_wifi.task) {
        return ESP_ERR_INVALID_STATE;
    }
    s_wifi.radio = radio;
    /* Below Ethernet (route priority 50): the LAN keeps the default route while its link is up. */
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_WIFI_STA();
    base.route_prio = 20;
    esp_netif_config_t cfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_WIFI_STA};
    s_wifi.netif = esp_netif_new(&cfg);
    if (!s_wifi.netif) {
        return ESP_ERR_NO_MEM;
    }
    esp_netif_attach_wifi_station(s_wifi.netif);
    esp_wifi_set_default_wifi_sta_handlers();
    esp_netif_set_hostname(s_wifi.netif, gw_state()->hostname);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL);
    esp_event_handler_register(ESP_HOSTED_EVENT, ESP_EVENT_ANY_ID, on_hosted_event, NULL);
    gw_api_set_role_status(radio, wifi_status);
    if (xTaskCreate(wifi_task, "wifi_uplink", 6144, NULL, 5, &s_wifi.task) != pdPASS) {
        s_wifi.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
