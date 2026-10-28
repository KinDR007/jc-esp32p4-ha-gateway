#include <string.h>

#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "freertos/event_groups.h"
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

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
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
        ESP_LOGI(TAG, "IPv6 " IPV6STR, IPV62STR(event->ip6_info.ip));
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
    }
    cJSON *ip6 = cJSON_AddArrayToObject(json, "ipv6");
    esp_ip6_addr_t addrs[CONFIG_LWIP_IPV6_NUM_ADDRESSES];
    int count = s_netif ? esp_netif_get_all_ip6(s_netif, addrs) : 0;
    for (int i = 0; i < count; i++) {
        snprintf(buf, sizeof(buf), IPV6STR, IPV62STR(addrs[i]));
        cJSON_AddItemToArray(ip6, cJSON_CreateString(buf));
    }
    return json;
}
