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
#include "lwip/udp.h"
#include "role_wifi.h"

static const char *TAG = "role_wifi";

#define KEY_CFG "upw"       /* str JSON {"ssid", "mode": "dhcp"|"static", "ip", "mask", "gw", "dns1", "dns2"} */
#define KEY_PASS "upw_pass" /* secret setting (gw_config) */
#define KEY_ESPH_SSID "espw_ssid" /* the ESPHome Wi-Fi, used while the uplink has none of its own */
#define KEY_ESPH_PASS "espw_pass"

#define UART_BAUD 921600  /* firmware/c6/wifi_uplink/sdkconfig.overlay */
#define POLL_MS 5000      /* signal and channel refresh while connected */
#define BRINGUP_RETRY_S 30
#define RPC_FAILS_MAX 3   /* polls without an answer from the radio before it is restarted */
#define RETRY_MAX_S 60

#define EV_APPLY BIT0   /* worker task notifications: settings changed */
#define EV_RESTART BIT1 /* restart the radio (API) */

/* Setup access point (captive portal): open, only after a start without LAN and without a Wi-Fi
 * connection, and only for a few minutes, longer while a phone is on it. */
#define PORTAL_AFTER_S 45   /* gateway uptime before it may open */
#define PORTAL_UNTIL_S 600  /* opens only this long after the start, closes then without a client */
#define PORTAL_MAX_S 1800   /* closes in any case */

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
    uint32_t connects, disconnects, retries, restarts;
    int rpc_fails;
    int hang_test_s;      /* API: hold the radio in reset this long without telling the host */
    int64_t connected_us, retry_at_us;
    bool has_ip;
    bool fixed;           /* static IPv4 */
} s_wifi = {.state = "off"};

static struct {
    bool on, done;        /* done: opened once since the start, not again */
    esp_netif_t *netif;
    char ssid[33];
    uint32_t ip;          /* network order */
    int clients;
} s_portal;

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
    } else if (id == WIFI_EVENT_AP_STACONNECTED) {
        s_portal.clients++;
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        s_portal.clients -= s_portal.clients > 0;
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *e = data;
        bool was_connected = s_wifi.connected_us != 0;
        s_wifi.reason = e->reason;
        s_wifi.rssi = 0;
        s_wifi.connected_us = 0;
        s_wifi.has_ip = false;
        gw_net_uplink_ip(s_wifi.netif, false);
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
        gw_net_uplink_ip(s_wifi.netif, true);
        ESP_LOGI(TAG, "IPv4 " IPSTR, IP2STR(&e->ip_info.ip));
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_wifi.has_ip = false;
        gw_net_uplink_ip(s_wifi.netif, false);
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
        gw_net_uplink_ip(s_wifi.netif, false);
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

/* ---- setup access point ---- */

/* Every name resolves to the gateway, so a phone's connectivity check lands on its web UI. A raw lwIP
 * PCB bound to the access point's address: port 53 is also held by the Thread border router's DNS
 * server (any address), lwIP binds a second PCB to a port only when both allow address reuse, and
 * then prefers the one bound to the address the query went to. */
static struct udp_pcb *s_dns_pcb;
static uint32_t s_dns_answers;

static void dns_recv(void *arg, struct udp_pcb *pcb, struct pbuf *p, const ip_addr_t *addr, u16_t port)
{
    static uint8_t buf[512]; /* the lwIP thread is the only caller */
    int n = pbuf_copy_partial(p, buf, sizeof(buf) - 16, 0);
    pbuf_free(p);
    if (n < 17 || (buf[2] & 0x80)) {
        return;
    }
    size_t q = 12;
    while (q < (size_t)n && buf[q]) {
        q += buf[q] + 1;
    }
    q += 5; /* end of the name, type, class */
    if (q > (size_t)n) {
        return;
    }
    bool a = buf[q - 4] == 0 && buf[q - 3] == 1;
    buf[2] = 0x84 | (buf[2] & 0x01); /* answer, authoritative, recursion desired as asked */
    buf[3] = 0x80;
    memset(buf + 6, 0, 6);
    buf[7] = a;
    size_t len = q;
    if (a) {
        static const uint8_t rr[] = {0xc0, 0x0c, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4};
        memcpy(buf + len, rr, sizeof(rr));
        memcpy(buf + len + sizeof(rr), &s_portal.ip, 4);
        len += sizeof(rr) + 4;
    }
    struct pbuf *r = pbuf_alloc(PBUF_TRANSPORT, len, PBUF_RAM);
    if (r) {
        memcpy(r->payload, buf, len);
        s_dns_answers += udp_sendto(pcb, r, addr, port) == ERR_OK;
        pbuf_free(r);
    }
}

static esp_err_t dns_open(void *ctx)
{
    for (struct udp_pcb *o = udp_pcbs; o; o = o->next) {
        if (o->local_port == 53) {
            ESP_LOGI(TAG, "port 53 also bound to %s, netif %u", ipaddr_ntoa(&o->local_ip), o->netif_idx);
            ip_set_option(o, SOF_REUSEADDR);
        }
    }
    s_dns_pcb = udp_new();
    if (!s_dns_pcb) {
        return ESP_ERR_NO_MEM;
    }
    ip_set_option(s_dns_pcb, SOF_REUSEADDR);
    ip_addr_t ip = IPADDR4_INIT(s_portal.ip);
    if (udp_bind(s_dns_pcb, &ip, 53) != ERR_OK) {
        udp_remove(s_dns_pcb);
        s_dns_pcb = NULL;
        return ESP_FAIL;
    }
    udp_recv(s_dns_pcb, dns_recv, NULL);
    return ESP_OK;
}

static esp_err_t dns_close(void *ctx)
{
    if (s_dns_pcb) {
        udp_remove(s_dns_pcb);
        s_dns_pcb = NULL;
    }
    return ESP_OK;
}

/* Unknown pages (Android generate_204, Apple hotspot-detect.html, Windows connecttest.txt) open the
 * web UI while the access point is up; a plain 404 otherwise. */
static esp_err_t portal_404(httpd_req_t *req, httpd_err_code_t err)
{
    if (!s_portal.on) {
        return httpd_resp_send_err(req, err, NULL);
    }
    char url[40];
    snprintf(url, sizeof(url), "http://" IPSTR "/#system/wifi", IP2STR((esp_ip4_addr_t *)&s_portal.ip));
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", url);
    return httpd_resp_send(req, NULL, 0);
}

static void portal_start(void)
{
    if (!s_portal.netif) {
        s_portal.netif = esp_netif_create_default_wifi_ap();
        httpd_register_err_handler(gw_http_server(), HTTPD_404_NOT_FOUND, portal_404);
    }
    esp_netif_ip_info_t ip;
    esp_netif_get_ip_info(s_portal.netif, &ip);
    s_portal.ip = ip.ip.addr;
    /* the phones get the gateway as their DNS server */
    uint8_t offer = 1;
    esp_netif_dhcps_stop(s_portal.netif);
    esp_netif_dhcps_option(s_portal.netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer, sizeof(offer));
    esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4 = ip.ip};
    esp_netif_set_dns_info(s_portal.netif, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_start(s_portal.netif);

    wifi_config_t ap = {.ap = {.authmode = WIFI_AUTH_OPEN, .max_connection = 4, .channel = 0}};
    strlcpy(s_portal.ssid, gw_state()->hostname, sizeof(s_portal.ssid));
    memcpy(ap.ap.ssid, s_portal.ssid, strlen(s_portal.ssid));
    ap.ap.ssid_len = strlen(s_portal.ssid);
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "setup access point: %s", esp_err_to_name(err));
        esp_wifi_set_mode(WIFI_MODE_STA);
        s_portal.done = true;
        return;
    }
    s_portal.on = true;
    s_portal.clients = 0;
    if (esp_netif_tcpip_exec(dns_open, NULL) != ESP_OK) {
        ESP_LOGW(TAG, "portal DNS not started");
    }
    ESP_LOGW(TAG, "no LAN and no Wi-Fi: setup access point %s open at " IPSTR, s_portal.ssid, IP2STR(&ip.ip));
    gw_notify("network", GW_NOTIFY_WARNING, "No LAN and no Wi-Fi: setup access point %s open for a few minutes", s_portal.ssid);
}

static void portal_stop(const char *why)
{
    s_portal.on = false;
    esp_netif_tcpip_exec(dns_close, NULL);
    s_portal.done = true;
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup access point closed: %s", why);
}

static void portal_check(void)
{
    int64_t up_s = esp_timer_get_time() / 1000000;
    bool online = s_wifi.has_ip || gw_eth_has_ip();
    if (!s_portal.on) {
        if (!s_portal.done && !online && up_s >= PORTAL_AFTER_S && up_s < PORTAL_UNTIL_S) {
            portal_start();
        }
    } else if (up_s >= PORTAL_MAX_S) {
        portal_stop("time is up");
    } else if (!s_portal.clients && (online || up_s >= PORTAL_UNTIL_S)) {
        portal_stop(online ? "the gateway is online" : "time is up");
    }
}

static void bring_up(void)
{
    s_wifi.state = "starting";
    while (host_up() != ESP_OK) {
        s_wifi.state = "radio_unreachable";
        ESP_LOGW(TAG, "radio %c: %s", 'A' + s_wifi.radio, s_wifi.error);
        vTaskDelay(pdMS_TO_TICKS(BRINGUP_RETRY_S * 1000));
        s_wifi.state = "starting";
    }
    s_wifi.up = true;
    s_wifi.rpc_fails = 0;
}

/* The radio stopped answering (seen after the setup access point closed): the host goes down, the
 * radio is reset and everything comes up again. */
static void restart_radio(const char *why)
{
    ESP_LOGW(TAG, "radio %c restarted: %s", 'A' + s_wifi.radio, why);
    gw_notify("network", GW_NOTIFY_WARNING, "Wi-Fi uplink: radio %c restarted (%s)", 'A' + s_wifi.radio, why);
    s_wifi.up = false;
    s_wifi.restarts++;
    s_wifi.has_ip = false;
    s_wifi.connected_us = 0;
    gw_net_uplink_ip(s_wifi.netif, false);
    esp_netif_action_disconnected(s_wifi.netif, NULL, 0, NULL);
    if (s_portal.on) {
        portal_stop("radio restart");
    }
    esp_wifi_deinit(); /* frees the host's channels also when the radio does not answer */
    esp_hosted_deinit();
    /* the radio's start adds the station netif to lwIP again: out of it first (a stop event of a
     * radio that still answered comes within the pause) */
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_netif_action_stop(s_wifi.netif, NULL, 0, NULL);
    bring_up();
    station_apply();
}

static void wifi_task(void *arg)
{
    bring_up();
    ESP_LOGI(TAG, "radio %c: co-processor %s up", 'A' + s_wifi.radio, s_wifi.cp_version);
    station_apply();
    while (true) {
        uint32_t ev = 0;
        xTaskNotifyWait(0, UINT32_MAX, &ev, pdMS_TO_TICKS(POLL_MS));
        if (ev & EV_RESTART) {
            if (s_wifi.hang_test_s) {
                /* the radio held in reset looks hung to the host: the poll below finds it */
                c6_hold_reset(s_wifi.radio, true);
                vTaskDelay(pdMS_TO_TICKS(s_wifi.hang_test_s * 1000));
                c6_hold_reset(s_wifi.radio, false);
                s_wifi.hang_test_s = 0;
            } else {
                restart_radio("requested");
                continue;
            }
        }
        /* liveness: an RPC the co-processor answers in any state */
        esp_hosted_coprocessor_fwver_t ver;
        if (esp_hosted_get_coprocessor_fwversion(&ver) != 0) {
            if (++s_wifi.rpc_fails >= RPC_FAILS_MAX) {
                restart_radio("no answer");
                continue;
            }
        } else {
            s_wifi.rpc_fails = 0;
        }
        if (ev & EV_APPLY) {
            station_apply();
        } else if (s_wifi.retry_at_us && esp_timer_get_time() >= s_wifi.retry_at_us && !s_portal.clients) {
            /* no scans while a phone is on the setup access point: the radio would leave its channel */
            s_wifi.retry_at_us = 0;
            s_wifi.state = "connecting";
            esp_wifi_connect();
        }
        refresh();
        portal_check();
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
    cJSON_AddNumberToObject(j, "radio_restarts", s_wifi.restarts);
    cJSON_AddNumberToObject(j, "last_reason", s_wifi.reason);
    cJSON_AddStringToObject(j, "ip_mode", s_wifi.fixed ? "static" : "dhcp");
    cJSON_AddBoolToObject(j, "default_route", s_wifi.netif && esp_netif_get_default_netif() == s_wifi.netif);
    cJSON *portal = cJSON_AddObjectToObject(j, "portal");
    cJSON_AddBoolToObject(portal, "on", s_portal.on);
    if (s_portal.on) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR((esp_ip4_addr_t *)&s_portal.ip));
        cJSON_AddStringToObject(portal, "ssid", s_portal.ssid);
        cJSON_AddStringToObject(portal, "ip", buf);
        cJSON_AddNumberToObject(portal, "clients", s_portal.clients);
        cJSON_AddNumberToObject(portal, "dns_answers", s_dns_answers);
        int64_t left = PORTAL_UNTIL_S - esp_timer_get_time() / 1000000;
        cJSON_AddNumberToObject(portal, "closes_s", left > 0 ? (double)left : 0); /* 0: open while a phone is on it */
    }
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
 * applied at once when the uplink runs. "ssid": "" drops the own setting (the ESPHome Wi-Fi again). */
static esp_err_t wifi_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *ssid = GW_JSON_STR(body, "ssid"), *pass = GW_JSON_STR(body, "password");
    if (ssid && !ssid[0]) {
        cJSON_Delete(body);
        gw_config_erase(KEY_CFG);
        gw_config_erase(KEY_PASS);
        gw_notify("network", GW_NOTIFY_INFO, "Wi-Fi uplink settings dropped by %s: the ESPHome Wi-Fi is used",
                  gw_auth_who()[0] ? gw_auth_who() : "the web UI");
        if (s_wifi.task && s_wifi.up) {
            xTaskNotify(s_wifi.task, EV_APPLY, eSetBits);
        }
        return wifi_get(req);
    }
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

/* {} restarts the radio; {"hang_test_s": 20} holds it in reset without telling the host, a test of
 * the watchdog. */
static esp_err_t wifi_restart_post(httpd_req_t *req)
{
    cJSON *body = req->content_len ? gw_http_read_json(req) : NULL;
    if (req->content_len && !body) {
        return ESP_OK;
    }
    cJSON *hang = cJSON_GetObjectItem(body, "hang_test_s");
    int hang_s = cJSON_IsNumber(hang) ? hang->valueint : 0;
    cJSON_Delete(body);
    if (!s_wifi.task || !s_wifi.up) {
        return gw_http_send_error(req, 409, "the Wi-Fi uplink does not run");
    }
    if (hang_s < 0 || hang_s > 60) {
        return gw_http_send_error(req, 400, "hang_test_s: 0..60");
    }
    s_wifi.hang_test_s = hang_s;
    xTaskNotify(s_wifi.task, EV_RESTART, eSetBits);
    return gw_http_send_json(req, cJSON_CreateObject());
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
    gw_http_register("/api/net/wifi/restart", HTTP_POST, wifi_restart_post);
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
