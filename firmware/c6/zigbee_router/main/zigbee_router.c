/*
 * Standalone Zigbee router (range extender) for the C6 of the P4 gateway.
 *
 * UART0 (460800 Bd) carries a line protocol to the P4:
 *   C6 -> P4: "@ZR {json}" status lines every 10 s and after every state change
 *   P4 -> C6: "status", "steer", "permit", "leave", "factory_reset"
 */
#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "driver/uart_vfs.h"
#include "esp_app_desc.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "zr";

#define ENDPOINT 1
#define STEER_RETRY_MS 30000
#define STATUS_INTERVAL_MS 10000
#define PERMIT_JOIN_S 180

static volatile bool s_joined;
static volatile bool s_steering;
static volatile int64_t s_permit_until_us;
static volatile bool s_stack_ready; /* esp_zb_lock exists only after esp_zb_init() */

static void print_status(void)
{
    uint8_t ext_pan[8], ieee[8];
    char ext_hex[17], ieee_hex[17];

    esp_zb_get_extended_pan_id(ext_pan);
    esp_zb_get_long_address(ieee);
    for (int i = 0; i < 8; i++) {
        /* Both are little endian in memory, print them the usual way round. */
        sprintf(ext_hex + 2 * i, "%02x", ext_pan[7 - i]);
        sprintf(ieee_hex + 2 * i, "%02x", ieee[7 - i]);
    }
    bool permit = esp_timer_get_time() < s_permit_until_us;
    printf("@ZR {\"state\":\"%s\",\"joined\":%s,\"pan_id\":\"0x%04x\",\"ext_pan_id\":\"%s\",\"channel\":%u,"
           "\"short_addr\":\"0x%04x\",\"ieee\":\"%s\",\"permit_join\":%s,\"version\":\"%s\"}\n",
           s_joined ? "joined" : s_steering ? "steering" : "idle", s_joined ? "true" : "false",
           esp_zb_get_pan_id(), ext_hex, esp_zb_get_current_channel(), esp_zb_get_short_address(), ieee_hex,
           permit ? "true" : "false", esp_app_get_description()->version);
    fflush(stdout);
}

static void start_steering(uint8_t unused)
{
    if (s_joined) {
        return;
    }
    s_steering = true;
    esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *p_sg_p = signal_struct->p_app_signal;
    esp_err_t status = signal_struct->esp_err_status;
    esp_zb_app_signal_type_t type = *p_sg_p;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status != ESP_OK) {
            ESP_LOGW(TAG, "stack start failed: %s", esp_err_to_name(status));
            esp_zb_scheduler_alarm(start_steering, 0, 1000);
        } else if (esp_zb_bdb_is_factory_new()) {
            start_steering(0);
        } else {
            /* Rejoined the network stored in zb_storage. */
            s_joined = true;
        }
        print_status();
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        s_steering = false;
        if (status == ESP_OK) {
            s_joined = true;
            ESP_LOGW(TAG, "joined PAN 0x%04x channel %d", esp_zb_get_pan_id(), esp_zb_get_current_channel());
        } else {
            esp_zb_scheduler_alarm(start_steering, 0, STEER_RETRY_MS);
        }
        print_status();
        break;
    case ESP_ZB_ZDO_SIGNAL_LEAVE:
        s_joined = false;
        print_status();
        break;
    default:
        break;
    }
}

static void handle_command(const char *cmd)
{
    if (strcmp(cmd, "steer") == 0) {
        esp_zb_lock_acquire(portMAX_DELAY);
        start_steering(0);
        esp_zb_lock_release();
    } else if (strcmp(cmd, "permit") == 0 && s_joined) {
        esp_zb_lock_acquire(portMAX_DELAY);
        if (esp_zb_bdb_open_network(PERMIT_JOIN_S) == ESP_OK) {
            s_permit_until_us = esp_timer_get_time() + PERMIT_JOIN_S * 1000000LL;
        }
        esp_zb_lock_release();
    } else if (strcmp(cmd, "leave") == 0 || strcmp(cmd, "factory_reset") == 0) {
        /* Clears zb_storage and restarts factory new; steering then waits for a command. */
        esp_zb_lock_acquire(portMAX_DELAY);
        esp_zb_factory_reset();
        esp_zb_lock_release();
        return;
    }
    esp_zb_lock_acquire(portMAX_DELAY);
    print_status();
    esp_zb_lock_release();
}

static void uart_task(void *arg)
{
    char line[64];
    size_t len = 0;
    uint8_t c;
    TickType_t last_status = xTaskGetTickCount();

    while (!s_stack_ready) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    while (true) {
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(500)) == 1) {
            if (c == '\n' || c == '\r') {
                line[len] = '\0';
                if (len) {
                    handle_command(line);
                }
                len = 0;
            } else if (len < sizeof(line) - 1) {
                line[len++] = c;
            }
        }
        if (xTaskGetTickCount() - last_status >= pdMS_TO_TICKS(STATUS_INTERVAL_MS)) {
            last_status = xTaskGetTickCount();
            esp_zb_lock_acquire(portMAX_DELAY);
            print_status();
            esp_zb_lock_release();
        }
    }
}

static void zigbee_task(void *arg)
{
    esp_zb_cfg_t cfg = {
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ROUTER,
        .install_code_policy = false,
        .nwk_cfg.zczr_cfg = {.max_children = 32},
    };
    esp_zb_init(&cfg);

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = 0x01, /* mains */
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, "\x07" "Guition");
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, "\x0f" "P4GW-ZB-ROUTER");
    esp_zb_identify_cluster_cfg_t identify_cfg = {.identify_time = 0};
    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    esp_zb_cluster_list_add_basic_cluster(clusters, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_cluster_list_add_identify_cluster(clusters, esp_zb_identify_cluster_create(&identify_cfg),
                                             ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_ep_list_t *eps = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_RANGE_EXTENDER_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_zb_ep_list_add_ep(eps, clusters, ep_cfg);
    esp_zb_device_register(eps);

    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    s_stack_ready = true;
    esp_zb_stack_main_loop();
}

void app_main(void)
{
    esp_zb_platform_config_t platform = {
        .radio_config = {.radio_mode = ZB_RADIO_MODE_NATIVE},
        .host_config = {.host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE},
    };
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_zb_platform_config(&platform));

    /* stdout keeps working through the driver once it is installed. */
    ESP_ERROR_CHECK(uart_driver_install(UART_NUM_0, 1024, 0, 0, NULL, 0));
    uart_vfs_dev_use_driver(UART_NUM_0);

    xTaskCreate(zigbee_task, "zigbee", 8192, NULL, 5, NULL);
    xTaskCreate(uart_task, "p4_link", 4096, NULL, 4, NULL);
}
