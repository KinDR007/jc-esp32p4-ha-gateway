#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "freertos/event_groups.h"
#include "lwip/ip6_addr.h"
#include "lwip/netif.h"
#include "lwip/pbuf.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "gw.h"
#include "sdkconfig.h"

static const char *TAG = "gw_eth";

#define BIT_IP4 BIT0
#define BIT_IP6 BIT1

static esp_netif_t *s_netif;
static esp_eth_handle_t s_eth;
static EventGroupHandle_t s_events;
static bool s_link_up;
static uint32_t s_link_changes;

/* ---- traffic counters ----
 * The lwIP input and link output functions of the Ethernet netif are wrapped: every frame adds its
 * length to the totals and to one service, told by the TCP/UDP port. A few header reads per frame,
 * nothing is copied or delayed. IPv6 packets that are neither from nor to this gateway are the
 * Thread traffic it routes. */
enum { SVC_WEB, SVC_OTBR, SVC_ZIGBEE, SVC_MQTT, SVC_MDNS, SVC_DNS_NTP, SVC_HTTPS, SVC_THREAD, SVC_OTHER, SVC_COUNT };
static const char *const s_svc_names[SVC_COUNT] = {"web", "otbr_rest", "zigbee", "mqtt", "mdns",
                                                   "dns_ntp", "https", "thread", "other"};
typedef struct {
    uint64_t bytes;
    uint32_t packets;
} counter_t;
static counter_t s_rx[SVC_COUNT], s_tx[SVC_COUNT];
static struct netif *s_lwip;
static netif_input_fn s_input;
static netif_linkoutput_fn s_linkoutput;

static bool own_ip6(const uint8_t *addr)
{
    for (int i = 0; i < LWIP_IPV6_NUM_ADDRESSES; i++) {
        if (ip6_addr_isvalid(netif_ip6_addr_state(s_lwip, i)) &&
            memcmp(netif_ip6_addr(s_lwip, i)->addr, addr, 16) == 0) {
            return true;
        }
    }
    return false;
}

/* routed: unicast, not link-local, and not an address of this gateway */
static bool routed_ip6(const uint8_t *addr)
{
    return addr[0] != 0xff && !(addr[0] == 0xfe && (addr[1] & 0xc0) == 0x80) && addr[0] | addr[1] && !own_ip6(addr);
}

static int classify(const uint8_t *f, size_t len, bool rx)
{
    const uint8_t *l4;
    uint8_t proto;
    if (len < 14) {
        return SVC_OTHER;
    }
    uint16_t type = f[12] << 8 | f[13];
    if (type == 0x0800 && len >= 34) {
        size_t ihl = (f[14] & 0x0f) * 4;
        if (((f[20] & 0x1f) | f[21]) || len < 14 + ihl + 4) {
            return SVC_OTHER; /* a later fragment has no ports */
        }
        proto = f[23];
        l4 = f + 14 + ihl;
    } else if (type == 0x86dd && len >= 58) {
        if (routed_ip6(rx ? f + 38 : f + 22)) {
            return SVC_THREAD;
        }
        proto = f[20];
        l4 = f + 54;
    } else {
        return SVC_OTHER;
    }
    if (proto != 6 && proto != 17) {
        return SVC_OTHER;
    }
    uint16_t sport = l4[0] << 8 | l4[1], dport = l4[2] << 8 | l4[3];
    uint16_t local = rx ? dport : sport, remote = rx ? sport : dport;
    if (local == 80) {
        return SVC_WEB;
    } else if (local == CONFIG_GW_OTBR_REST_PORT) {
        return SVC_OTBR;
    } else if (local == CONFIG_GW_ZIGBEE_TCP_PORT) {
        return SVC_ZIGBEE;
    } else if (local == 5353 || remote == 5353) {
        return SVC_MDNS;
    } else if (remote == 1883 || remote == 8883) {
        return SVC_MQTT;
    } else if (remote == 53 || remote == 123) {
        return SVC_DNS_NTP;
    } else if (remote == 443) {
        return SVC_HTTPS;
    }
    return SVC_OTHER;
}

static void count(counter_t *c, const struct pbuf *p, bool rx)
{
    int svc = classify(p->payload, p->len, rx);
    c[svc].bytes += p->tot_len;
    c[svc].packets++;
}

static err_t count_input(struct pbuf *p, struct netif *netif)
{
    count(s_rx, p, true);
    return s_input(p, netif);
}

static err_t count_linkoutput(struct netif *netif, struct pbuf *p)
{
    count(s_tx, p, false);
    return s_linkoutput(netif, p);
}

static void hook_counters(void)
{
    struct netif *n = esp_netif_get_netif_impl(s_netif);
    if (!n || !n->input || !n->linkoutput || n->input == count_input) {
        return;
    }
    s_lwip = n;
    s_input = n->input;
    s_linkoutput = n->linkoutput;
    n->input = count_input;
    n->linkoutput = count_linkoutput;
}

cJSON *gw_eth_traffic(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddNumberToObject(json, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON *svcs = cJSON_AddObjectToObject(json, "services");
    counter_t rx = {0}, tx = {0};
    for (int i = 0; i < SVC_COUNT; i++) {
        cJSON *svc = cJSON_AddObjectToObject(svcs, s_svc_names[i]);
        cJSON_AddNumberToObject(svc, "rx", (double)s_rx[i].bytes);
        cJSON_AddNumberToObject(svc, "tx", (double)s_tx[i].bytes);
        cJSON_AddNumberToObject(svc, "rx_packets", s_rx[i].packets);
        cJSON_AddNumberToObject(svc, "tx_packets", s_tx[i].packets);
        rx.bytes += s_rx[i].bytes;
        rx.packets += s_rx[i].packets;
        tx.bytes += s_tx[i].bytes;
        tx.packets += s_tx[i].packets;
    }
    cJSON_AddNumberToObject(json, "rx", (double)rx.bytes);
    cJSON_AddNumberToObject(json, "tx", (double)tx.bytes);
    cJSON_AddNumberToObject(json, "rx_packets", rx.packets);
    cJSON_AddNumberToObject(json, "tx_packets", tx.packets);
    return json;
}

/* ---- LAN settings ----
 * GW_KEY_LAN holds {"mode": "dhcp"|"static", "ip", "mask", "gw", "dns1", "dns2",
 * "link": "auto"|"100full"|"100half"|"10full"|"10half"}; none means DHCP and autonegotiation.
 * A change applies at once but is stored only when confirmed (from the new address) within
 * LAN_CONFIRM_S; otherwise the previous settings come back, also after a restart. */
#define LAN_CONFIRM_S 120

typedef struct {
    bool fixed; /* static IPv4 */
    esp_netif_ip_info_t ip;
    esp_ip4_addr_t dns[2];
    char link[8];
} lan_cfg_t;

static lan_cfg_t s_lan;          /* in effect */
static lan_cfg_t s_lan_saved;    /* stored, restored when a change is not confirmed */
static int64_t s_lan_deadline_us; /* 0 when no change waits for confirmation */
static esp_timer_handle_t s_lan_timer;

static bool parse_ip(const cJSON *json, const char *key, esp_ip4_addr_t *out, bool required)
{
    const char *text = GW_JSON_STR(json, key);
    out->addr = 0;
    if (!text || !text[0]) {
        return !required;
    }
    return esp_netif_str_to_ip4(text, out) == ESP_OK;
}

static bool link_valid(const char *link)
{
    static const char *const modes[] = {"auto", "100full", "100half", "10full", "10half"};
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        if (strcmp(link, modes[i]) == 0) {
            return true;
        }
    }
    return false;
}

static bool lan_parse(const cJSON *json, lan_cfg_t *cfg, char *err, size_t err_len)
{
    memset(cfg, 0, sizeof(*cfg));
    const char *mode = GW_JSON_STR(json, "mode");
    const char *link = GW_JSON_STR(json, "link");
    strlcpy(cfg->link, link && link[0] ? link : "auto", sizeof(cfg->link));
    if (!link_valid(cfg->link)) {
        snprintf(err, err_len, "link: auto, 100full, 100half, 10full or 10half");
        return false;
    }
    if (!mode || strcmp(mode, "dhcp") == 0) {
        return true;
    }
    if (strcmp(mode, "static") != 0) {
        snprintf(err, err_len, "mode: dhcp or static");
        return false;
    }
    cfg->fixed = true;
    if (!parse_ip(json, "ip", &cfg->ip.ip, true) || !parse_ip(json, "mask", &cfg->ip.netmask, true)) {
        snprintf(err, err_len, "address and netmask are required");
        return false;
    }
    if (!parse_ip(json, "gw", &cfg->ip.gw, false) || !parse_ip(json, "dns1", &cfg->dns[0], false) ||
        !parse_ip(json, "dns2", &cfg->dns[1], false)) {
        snprintf(err, err_len, "invalid gateway or DNS address");
        return false;
    }
    uint32_t mask = ntohl(cfg->ip.netmask.addr), ip = ntohl(cfg->ip.ip.addr);
    if (mask == 0 || mask == 0xffffffff || (~mask & (~mask + 1)) != 0) {
        snprintf(err, err_len, "invalid netmask");
        return false;
    }
    if ((ip & ~mask) == 0 || (ip & ~mask) == ~mask || (ip >> 24) == 127 || (ip >> 28) >= 14) {
        snprintf(err, err_len, "invalid address for this netmask");
        return false;
    }
    if (cfg->ip.gw.addr && ((ntohl(cfg->ip.gw.addr) ^ ip) & mask)) {
        snprintf(err, err_len, "the gateway is not in the subnet of the address");
        return false;
    }
    return true;
}

static cJSON *lan_json(const lan_cfg_t *cfg)
{
    char buf[16];
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", cfg->fixed ? "static" : "dhcp");
    cJSON_AddStringToObject(json, "link", cfg->link);
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

static void lan_load(lan_cfg_t *cfg)
{
    char text[200], err[64];
    cJSON *json = gw_config_get_str(GW_KEY_LAN, text, sizeof(text)) ? cJSON_Parse(text) : NULL;
    if (!json || !lan_parse(json, cfg, err, sizeof(err))) {
        lan_parse(NULL, cfg, err, sizeof(err)); /* DHCP, autonegotiation */
    }
    cJSON_Delete(json);
}

/* Autonegotiation or a fixed speed and duplex; the PHY takes them while the driver is stopped. */
static void set_link(const char *link)
{
    bool auto_neg = strcmp(link, "auto") == 0;
    eth_speed_t speed = strncmp(link, "100", 3) == 0 ? ETH_SPEED_100M : ETH_SPEED_10M;
    eth_duplex_t duplex = strstr(link, "full") ? ETH_DUPLEX_FULL : ETH_DUPLEX_HALF;
    esp_eth_ioctl(s_eth, ETH_CMD_S_AUTONEGO, &auto_neg);
    if (!auto_neg) {
        esp_eth_ioctl(s_eth, ETH_CMD_S_SPEED, &speed);
        esp_eth_ioctl(s_eth, ETH_CMD_S_DUPLEX_MODE, &duplex);
    }
}

/* Applies cfg to the running interface; only what changed (a new link mode restarts the link). */
static void lan_apply(const lan_cfg_t *cfg, bool started)
{
    if (cfg->fixed) {
        esp_netif_dhcpc_stop(s_netif);
        esp_netif_set_ip_info(s_netif, &cfg->ip);
        /* lwIP has one DNS list for all interfaces and the OpenThread DNS64 client overwrites its
         * first entry with the NAT64 server of the Thread network, so the LAN servers also go to the
         * backup and fallback entries (DHCP fills the backup entry itself). */
        const esp_ip4_addr_t servers[] = {cfg->dns[0], cfg->dns[1].addr ? cfg->dns[1] : cfg->dns[0], cfg->dns[0]};
        const esp_netif_dns_type_t slots[] = {ESP_NETIF_DNS_MAIN, ESP_NETIF_DNS_BACKUP, ESP_NETIF_DNS_FALLBACK};
        for (int i = 0; i < 3; i++) {
            if (servers[i].addr) {
                esp_netif_dns_info_t dns = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4 = servers[i]};
                esp_netif_set_dns_info(s_netif, slots[i], &dns);
            }
        }
    } else if (s_lan.fixed || !started) {
        esp_netif_dhcpc_start(s_netif);
    }
    bool relink = started ? strcmp(cfg->link, s_lan.link) != 0 : strcmp(cfg->link, "auto") != 0;
    s_lan = *cfg;
    if (relink) {
        ESP_LOGI(TAG, "link mode %s", cfg->link);
        if (started) {
            esp_eth_stop(s_eth);
        }
        set_link(cfg->link);
        if (started) {
            esp_eth_start(s_eth);
        }
    }
}

static void revert_task(void *arg)
{
    if (s_lan_deadline_us) {
        ESP_LOGW(TAG, "LAN change not confirmed in %d s, previous settings restored", LAN_CONFIRM_S);
        s_lan_deadline_us = 0;
        lan_apply(&s_lan_saved, true);
    }
    vTaskDelete(NULL);
}

static void revert_timer(void *arg)
{
    xTaskCreate(revert_task, "lan_revert", 3072, NULL, 5, NULL);
}

static void lan_task(void *arg)
{
    lan_cfg_t *cfg = arg;
    vTaskDelay(pdMS_TO_TICKS(500)); /* the HTTP answer leaves on the old settings */
    lan_apply(cfg, true);
    free(cfg);
    vTaskDelete(NULL);
}

esp_err_t gw_lan_set(const cJSON *json, char *err, size_t err_len)
{
    lan_cfg_t *cfg = malloc(sizeof(*cfg));
    if (!cfg) {
        snprintf(err, err_len, "no memory");
        return ESP_ERR_NO_MEM;
    }
    if (!lan_parse(json, cfg, err, err_len)) {
        free(cfg);
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_lan_timer) {
        const esp_timer_create_args_t args = {.callback = revert_timer, .name = "lan_revert"};
        esp_timer_create(&args, &s_lan_timer);
    }
    esp_timer_stop(s_lan_timer);
    s_lan_deadline_us = esp_timer_get_time() + LAN_CONFIRM_S * 1000000LL;
    esp_timer_start_once(s_lan_timer, LAN_CONFIRM_S * 1000000LL);
    xTaskCreate(lan_task, "lan_apply", 3072, cfg, 5, NULL);
    return ESP_OK;
}

esp_err_t gw_lan_confirm(void)
{
    if (!s_lan_deadline_us) {
        return ESP_ERR_INVALID_STATE;
    }
    esp_timer_stop(s_lan_timer);
    s_lan_deadline_us = 0;
    s_lan_saved = s_lan;
    cJSON *json = lan_json(&s_lan);
    char *text = cJSON_PrintUnformatted(json);
    cJSON_Delete(json);
    esp_err_t err = text ? gw_config_set_str(GW_KEY_LAN, text) : ESP_ERR_NO_MEM;
    cJSON_free(text);
    ESP_LOGI(TAG, "LAN settings confirmed");
    return err;
}

cJSON *gw_lan_status(void)
{
    cJSON *json = lan_json(&s_lan);
    cJSON_AddNumberToObject(json, "confirm_s",
                            s_lan_deadline_us ? (int)((s_lan_deadline_us - esp_timer_get_time()) / 1000000) : 0);
    return json;
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        hook_counters();
        s_link_up = true;
        s_link_changes++;
        ESP_LOGI(TAG, "link up");
        esp_netif_create_ip6_linklocal(s_netif);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        s_link_up = false;
        s_link_changes++;
        xEventGroupClearBits(s_events, BIT_IP4 | BIT_IP6);
        ESP_LOGW(TAG, "link down");
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = data;
        ESP_LOGI(TAG, "IPv4 " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(s_events, BIT_IP4);
    } else if (id == IP_EVENT_GOT_IP6) {
        ip_event_got_ip6_t *event = data;
        if (event->esp_netif != s_netif) {
            return;
        }
        char addr[48];
        ip6addr_ntoa_r((const ip6_addr_t *)&event->ip6_info.ip, addr, sizeof(addr));
        ESP_LOGI(TAG, "IPv6 %s", addr);
        if (esp_netif_ip6_get_addr_type(&event->ip6_info.ip) == ESP_IP6_ADDR_IS_GLOBAL ||
            esp_netif_ip6_get_addr_type(&event->ip6_info.ip) == ESP_IP6_ADDR_IS_UNIQUE_LOCAL) {
            xEventGroupSetBits(s_events, BIT_IP6);
        }
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        xEventGroupClearBits(s_events, BIT_IP4);
    }
}

esp_err_t gw_eth_init(const char *hostname)
{
    s_events = xEventGroupCreate();

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_FALSE(s_netif, ESP_FAIL, TAG, "netif");
    esp_netif_set_hostname(s_netif, hostname);

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    mac_cfg.rx_task_stack_size = 4096;
    mac_cfg.rx_task_prio = 18;
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = 31;
    emac_cfg.smi_gpio.mdio_num = 52;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)50;
    emac_cfg.emac_dataif_gpio.rmii.tx_en_num = 49;
    emac_cfg.emac_dataif_gpio.rmii.txd0_num = 34;
    emac_cfg.emac_dataif_gpio.rmii.txd1_num = 35;
    emac_cfg.emac_dataif_gpio.rmii.crs_dv_num = 28;
    emac_cfg.emac_dataif_gpio.rmii.rxd0_num = 29;
    emac_cfg.emac_dataif_gpio.rmii.rxd1_num = 30;
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = CONFIG_GW_ETH_PHY_ADDR;
    phy_cfg.reset_gpio_num = CONFIG_GW_ETH_PHY_RST_GPIO;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "mac/phy");

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&cfg, &s_eth), TAG, "driver");
    ESP_RETURN_ON_ERROR(esp_netif_attach(s_netif, esp_eth_new_netif_glue(s_eth)), TAG, "attach");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, on_ip_event, NULL));
    lan_load(&s_lan_saved);
    lan_apply(&s_lan_saved, false);
    return esp_eth_start(s_eth);
}

esp_netif_t *gw_eth_netif(void)
{
    return s_netif;
}

bool gw_eth_wait_ip(TickType_t timeout)
{
    return xEventGroupWaitBits(s_events, BIT_IP4 | BIT_IP6, pdFALSE, pdFALSE, timeout) != 0;
}

cJSON *gw_eth_status(void)
{
    cJSON *json = cJSON_CreateObject();
    uint8_t mac[6] = {0};
    char buf[48];

    cJSON_AddBoolToObject(json, "link", s_link_up);
    cJSON_AddNumberToObject(json, "link_changes", s_link_changes);
    if (s_eth) {
        eth_speed_t speed;
        eth_duplex_t duplex;
        esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, mac);
        if (s_link_up && esp_eth_ioctl(s_eth, ETH_CMD_G_SPEED, &speed) == ESP_OK &&
            esp_eth_ioctl(s_eth, ETH_CMD_G_DUPLEX_MODE, &duplex) == ESP_OK) {
            cJSON_AddNumberToObject(json, "speed", speed == ETH_SPEED_100M ? 100 : 10);
            cJSON_AddBoolToObject(json, "full_duplex", duplex == ETH_DUPLEX_FULL);
        }
    }
    snprintf(buf, sizeof(buf), MACSTR, MAC2STR(mac));
    cJSON_AddStringToObject(json, "mac", buf);

    esp_netif_ip_info_t ip;
    if (s_netif && esp_netif_get_ip_info(s_netif, &ip) == ESP_OK) {
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.ip));
        cJSON_AddStringToObject(json, "ipv4", buf);
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.gw));
        cJSON_AddStringToObject(json, "gateway", buf);
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip.netmask));
        cJSON_AddStringToObject(json, "netmask", buf);
        /* IPv4 servers of the shared DNS list, each once */
        cJSON *dns = cJSON_AddArrayToObject(json, "dns");
        uint32_t seen[3] = {0};
        for (int i = ESP_NETIF_DNS_MAIN; i <= ESP_NETIF_DNS_FALLBACK; i++) {
            esp_netif_dns_info_t info;
            if (esp_netif_get_dns_info(s_netif, i, &info) == ESP_OK && info.ip.type == ESP_IPADDR_TYPE_V4 &&
                info.ip.u_addr.ip4.addr && info.ip.u_addr.ip4.addr != seen[0] && info.ip.u_addr.ip4.addr != seen[1]) {
                seen[i] = info.ip.u_addr.ip4.addr;
                snprintf(buf, sizeof(buf), IPSTR, IP2STR(&info.ip.u_addr.ip4));
                cJSON_AddItemToArray(dns, cJSON_CreateString(buf));
            }
        }
    }
    cJSON_AddStringToObject(json, "mode", s_lan.fixed ? "static" : "dhcp");
    cJSON_AddStringToObject(json, "link_mode", s_lan.link);
    cJSON_AddNumberToObject(json, "confirm_s",
                            s_lan_deadline_us ? (int)((s_lan_deadline_us - esp_timer_get_time()) / 1000000) : 0);
    cJSON *ip6 = cJSON_AddArrayToObject(json, "ipv6");
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int count = s_netif ? esp_netif_get_all_ip6(s_netif, addrs) : 0;
    for (int i = 0; i < count; i++) {
        ip6addr_ntoa_r((const ip6_addr_t *)&addrs[i], buf, sizeof(buf));
        cJSON_AddItemToArray(ip6, cJSON_CreateString(buf));
    }
    return json;
}
