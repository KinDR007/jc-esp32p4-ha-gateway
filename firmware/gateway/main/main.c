#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "c6_flasher.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"
#include "mdns.h"
#include "netscan.h"
#include "zigbee_net.h"
#include "role_thread.h"
#include "role_zigbee.h"
#include "sound.h"
#include "sdkconfig.h"

static const char *TAG = "main";

#define LOG_RING_SIZE (64 * 1024)
#define STABLE_AFTER_MS (60 * 1000)
#define C6_FLASH_MAX_TRIES 3

/* Makes sure the C6 runs the bundle the role needs; flashes it when not. */
static bool ensure_c6(const char *bundle)
{
    c6fw_entry_t entry;
    char current[16];
    uint8_t current_sha[32] = {0};

    esp_err_t err = c6fw_find(bundle, &entry);
    if (err != ESP_OK) {
        gw_set_maintenance("C6 image '%s' missing in the c6fw partition (%s), upload c6fw.bin", bundle,
                           esp_err_to_name(err));
        return false;
    }
    gw_config_get_str(GW_KEY_C6_BUNDLE, current, sizeof(current));
    gw_config_get_blob(GW_KEY_C6_SHA, current_sha, sizeof(current_sha));
    bool force = gw_config_get_u8(GW_KEY_C6_FORCE, 0);
    if (!force && strcmp(current, bundle) == 0 && memcmp(current_sha, entry.sha256, sizeof(current_sha)) == 0) {
        return true;
    }

    uint8_t tries = gw_config_get_u8(GW_KEY_C6_TRIES, 0);
    if (tries >= C6_FLASH_MAX_TRIES) {
        gw_set_maintenance("flashing the C6 failed %u times, check the JP1 jumpers and retry from the UI", tries);
        return false;
    }
    gw_config_set_u8(GW_KEY_C6_TRIES, tries + 1);
    ESP_LOGW(TAG, "C6: %s -> %s %s%s", current[0] ? current : "unknown", entry.name, entry.version,
             force ? " (forced)" : "");

    /* Flash only once the web UI is reachable (it polls once a second), so it shows the whole progress. */
    if (gw_eth_wait_ip(pdMS_TO_TICKS(15000))) {
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    err = c6_flash_bundle(&entry, current);
    if (err != ESP_OK) {
        gw_set_maintenance("%s", c6_progress()->last_error);
        return false;
    }
    gw_config_set_str(GW_KEY_C6_BUNDLE, entry.name);
    gw_config_set_blob(GW_KEY_C6_SHA, entry.sha256, sizeof(entry.sha256));
    gw_config_erase(GW_KEY_C6_FORCE);
    gw_config_set_u8(GW_KEY_C6_TRIES, 0);
    return true;
}

static void apply_role(gw_role_t role)
{
    gw_state_t *st = gw_state();

    if (role == GW_ROLE_NONE) {
        gw_set_maintenance("no role selected");
        return;
    }
    if (role == GW_ROLE_ESPHOME && gw_config_get_u8(GW_KEY_ESPHOME_TRY, 0)) {
        /* We are back although ESPHome was selected: its image did not boot (rollback). */
        gw_config_erase(GW_KEY_ESPHOME_TRY);
        gw_set_maintenance("the ESPHome image failed to boot, upload a working one or pick another role");
        return;
    }
    char flashed[16];
    bool factory_c6 = !gw_config_get_str(GW_KEY_C6_BUNDLE, flashed, sizeof(flashed));
    if (role == GW_ROLE_ESPHOME && factory_c6) {
        /* The factory C6 firmware is esp-hosted already; ESPHome updates it over SDIO itself,
         * so this role works without the JP1 jumpers. */
        ESP_LOGI(TAG, "C6 keeps its factory esp-hosted firmware");
    } else if (factory_c6 && !gw_config_get_u8(GW_KEY_C6_ADOPTED, 0)) {
        /* The factory firmware cannot be downloaded again: never overwrite it before a backup. */
        gw_set_maintenance("back up the C6 factory firmware first (System tab) or confirm skipping the backup");
        return;
    } else if (!ensure_c6(gw_role_c6_bundle(role))) {
        return;
    }

    esp_err_t err = ESP_OK;
    switch (role) {
    case GW_ROLE_THREAD_BR:
    case GW_ROLE_THREAD_ROUTER:
        err = role_thread_start(role == GW_ROLE_THREAD_BR);
        break;
    case GW_ROLE_ZIGBEE_COORD:
    case GW_ROLE_ZIGBEE_ROUTER:
        err = role_zigbee_start(role == GW_ROLE_ZIGBEE_COORD);
        break;
    case GW_ROLE_ESPHOME:
        err = gw_boot_select_esphome();
        if (err == ESP_OK) {
            /* ESPHome boots back into this slot, so record it now. */
            gw_boot_mark_stable();
            gw_config_set_u8(GW_KEY_ESPHOME_TRY, 1);
            ESP_LOGW(TAG, "handing over to ESPHome");
            gw_restart_later(1000);
        }
        break;
    default:
        err = ESP_ERR_INVALID_ARG;
        break;
    }
    if (err != ESP_OK) {
        gw_set_maintenance("starting %s failed: %s", gw_role_name(role), esp_err_to_name(err));
        return;
    }
    st->active = role;
}

/* Home Assistant instances in the LAN (mDNS _home-assistant._tcp): the web UI suggests the
 * Matter Server on the same host. */
static esp_err_t ha_discover_get(httpd_req_t *req)
{
    mdns_result_t *results = NULL;
    cJSON *list = cJSON_CreateArray();
    if (mdns_query_ptr("_home-assistant", "_tcp", 3000, 4, &results) == ESP_OK) {
        for (mdns_result_t *r = results; r; r = r->next) {
            cJSON *ha = cJSON_CreateObject();
            cJSON_AddStringToObject(ha, "name", r->instance_name ? r->instance_name : "");
            cJSON_AddNumberToObject(ha, "port", r->port);
            char ip[48] = "";
            for (mdns_ip_addr_t *a = r->addr; a; a = a->next) {
                if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                    snprintf(ip, sizeof(ip), IPSTR, IP2STR(&a->addr.u_addr.ip4));
                    break;
                }
            }
            cJSON_AddStringToObject(ha, "ip", ip);
            for (size_t i = 0; i < r->txt_count; i++) {
                const char *key = r->txt[i].key, *value = r->txt[i].value;
                if (value && (!strcmp(key, "version") || !strcmp(key, "internal_url") || !strcmp(key, "location_name"))) {
                    cJSON_AddStringToObject(ha, key, value);
                }
            }
            cJSON_AddItemToArray(list, ha);
        }
        mdns_query_results_free(results);
    }
    return gw_http_send_json(req, list);
}

static void init_mdns(const char *hostname)
{
    ESP_ERROR_CHECK(mdns_init());
    mdns_hostname_set(hostname);
    mdns_instance_name_set("P4 gateway");
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
}

/* JSON trees in PSRAM: Zigbee2MQTT device lists and Thread maps are far bigger than internal RAM allows. */
static void *json_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

void app_main(void)
{
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = free};
    cJSON_InitHooks(&hooks);
    gw_state_t *st = gw_state();

    ESP_ERROR_CHECK(gw_config_init());
    gw_log_init(LOG_RING_SIZE);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    c6_init_pins();

    gw_role_t def_role = GW_ROLE_THREAD_BR;
    gw_role_from_name(CONFIG_GW_DEFAULT_ROLE, &def_role);
    st->desired = gw_config_get_u8(GW_KEY_ROLE, def_role);
    if (st->desired >= GW_ROLE_MAX) {
        st->desired = def_role;
    }
    if (!gw_config_get_str(GW_KEY_HOSTNAME, st->hostname, sizeof(st->hostname)) || !st->hostname[0]) {
        uint8_t mac[6];
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(st->hostname, sizeof(st->hostname), "%s-%02x%02x%02x", CONFIG_GW_HOSTNAME_PREFIX, mac[3], mac[4],
                 mac[5]);
    }
    ESP_LOGI(TAG, "%s, desired role %s", st->hostname, gw_role_name(st->desired));

    bool boot_loop = gw_boot_loop_detected();
    ESP_ERROR_CHECK(gw_eth_init(st->hostname));
    init_mdns(st->hostname);
    ESP_ERROR_CHECK(gw_http_start());
    ESP_ERROR_CHECK(gw_api_register());
    gw_http_register("/api/ha/discover", HTTP_GET, ha_discover_get);
    netscan_init();
    zigbee_net_init(); /* before MQTT connects: it subscribes on connect */
    sound_init();
    gw_mqtt_start();

    if (boot_loop) {
        gw_set_maintenance("repeated crashes during start, role %s not started", gw_role_name(st->desired));
    } else {
        if (netscan_full_pending()) {
            /* the scanner image runs on the C6 first; apply_role flashes the role image back */
            if (gw_eth_wait_ip(pdMS_TO_TICKS(15000))) {
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            netscan_run_full();
        }
        apply_role(st->desired);
    }

    vTaskDelay(pdMS_TO_TICKS(STABLE_AFTER_MS));
    if (!st->stable) {
        gw_boot_mark_stable();
        ESP_LOGI(TAG, "stable, role %s", gw_role_name(st->active));
    }
}
