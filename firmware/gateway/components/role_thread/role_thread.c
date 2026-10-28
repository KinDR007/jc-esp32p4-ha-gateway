#include "role_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_br_web.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_openthread.h"
#include "esp_openthread_border_router.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_openthread_spinel.h"
#include "esp_openthread_types.h"
#include "esp_system.h"
#include "esp_vfs_eventfd.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gw.h"
#include "openthread/border_agent.h"
#include "openthread/border_routing.h"
#include "openthread/cli.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/nat64.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "sdkconfig.h"

static const char *TAG = "role_thread";

#define CLI_BUF_SIZE 8192
#define OT_LOCK_TIMEOUT pdMS_TO_TICKS(2000)

static bool s_border_router;
static bool s_started;
static char s_phase[64] = "starting";
static uint32_t s_rcp_failures;

static SemaphoreHandle_t s_cli_lock;  /* one web CLI command at a time */
static SemaphoreHandle_t s_cli_buf_lock;
static char *s_cli_buf;
static size_t s_cli_len;
static bool s_cli_capture;

static esp_openthread_config_t s_config = {
    .netif_config = ESP_NETIF_DEFAULT_OPENTHREAD(),
    .platform_config =
        {
            .radio_config =
                {
                    .radio_mode = RADIO_MODE_UART_RCP,
                    .radio_uart_config =
                        {
                            .port = CONFIG_GW_C6_UART_PORT,
                            .uart_config =
                                {
                                    .baud_rate = CONFIG_GW_C6_UART_BAUD,
                                    .data_bits = UART_DATA_8_BITS,
                                    .parity = UART_PARITY_DISABLE,
                                    .stop_bits = UART_STOP_BITS_1,
                                    .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                                    .source_clk = UART_SCLK_DEFAULT,
                                },
                            .rx_pin = CONFIG_GW_C6_UART_RX,
                            .tx_pin = CONFIG_GW_C6_UART_TX,
                        },
                },
            .host_config = {.host_connection_mode = HOST_CONNECTION_MODE_NONE},
            .port_config = {.storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10},
        },
};

/* ---- helpers ---- */

static void hex_encode(const uint8_t *data, size_t len, char *out)
{
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = digits[data[i] >> 4];
        out[2 * i + 1] = digits[data[i] & 0xf];
    }
    out[2 * len] = '\0';
}

static int hex_decode(const char *hex, uint8_t *out, size_t max)
{
    size_t len = strlen(hex);
    if (len % 2 || len / 2 > max) {
        return -1;
    }
    for (size_t i = 0; i < len / 2; i++) {
        unsigned value;
        if (sscanf(hex + 2 * i, "%2x", &value) != 1) {
            return -1;
        }
        out[i] = value;
    }
    return len / 2;
}

static void add_hex(cJSON *obj, const char *key, const uint8_t *data, size_t len)
{
    char buf[2 * OT_OPERATIONAL_DATASET_MAX_LENGTH + 1];
    hex_encode(data, len, buf);
    cJSON_AddStringToObject(obj, key, buf);
}

static void add_prefix(cJSON *obj, const char *key, const otIp6Prefix *prefix)
{
    char buf[OT_IP6_PREFIX_STRING_SIZE];
    otIp6PrefixToString(prefix, buf, sizeof(buf));
    cJSON_AddStringToObject(obj, key, buf);
}

/* ---- CLI capture ---- */

static int cli_output(void *context, const char *format, va_list args)
{
    char line[256];
    va_list copy;
    va_copy(copy, args);
    int len = vsnprintf(line, sizeof(line), format, copy);
    va_end(copy);
    if (len <= 0) {
        return len;
    }
    if (len >= (int)sizeof(line)) {
        len = sizeof(line) - 1;
    }
    if (s_cli_capture && xSemaphoreTake(s_cli_buf_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        size_t n = len < (int)(CLI_BUF_SIZE - 1 - s_cli_len) ? (size_t)len : CLI_BUF_SIZE - 1 - s_cli_len;
        memcpy(s_cli_buf + s_cli_len, line, n);
        s_cli_len += n;
        xSemaphoreGive(s_cli_buf_lock);
    }
    fwrite(line, 1, len, stdout);
    return len;
}

/* ---- network control ---- */

static void on_rcp_failure(void)
{
    /* The C6 stopped answering Spinel: reflash it on the next boot, the boot loop guard limits retries. */
    s_rcp_failures++;
    ESP_LOGE(TAG, "RCP failure, reflashing the C6 after restart");
    gw_config_set_u8(GW_KEY_C6_FORCE, 1);
    esp_restart();
}

static otError apply_dataset(otInstance *instance, const otOperationalDatasetTlvs *tlvs)
{
    otThreadSetEnabled(instance, false);
    otError error = otDatasetSetActiveTlvs(instance, tlvs);
    if (error == OT_ERROR_NONE) {
        otIp6SetEnabled(instance, true);
        error = otThreadSetEnabled(instance, true);
    }
    return error;
}

static void start_border_router(otInstance *instance)
{
    otOperationalDatasetTlvs dataset;
    esp_openthread_set_backbone_netif(gw_eth_netif());
    ESP_ERROR_CHECK(esp_openthread_border_router_init());

    if (otDatasetGetActiveTlvs(instance, &dataset) != OT_ERROR_NONE) {
        otOperationalDataset fresh;
        uint8_t mac[6];
        ESP_ERROR_CHECK(otDatasetCreateNewNetwork(instance, &fresh) == OT_ERROR_NONE ? ESP_OK : ESP_FAIL);
        esp_read_mac(mac, ESP_MAC_BASE);
        snprintf(fresh.mNetworkName.m8, sizeof(fresh.mNetworkName.m8), "P4GW-%02X%02X", mac[4], mac[5]);
        fresh.mComponents.mIsNetworkNamePresent = true;
        otDatasetConvertToTlvs(&fresh, &dataset);
        ESP_LOGI(TAG, "created Thread network %s", fresh.mNetworkName.m8);
    }
    ESP_ERROR_CHECK(esp_openthread_auto_start(&dataset));
}

static void start_router(otInstance *instance)
{
    otOperationalDatasetTlvs dataset;
    if (gw_config_get_blob(GW_KEY_TR_DATASET, &dataset, sizeof(dataset)) && dataset.mLength > 0) {
        otError error = apply_dataset(instance, &dataset);
        snprintf(s_phase, sizeof(s_phase), error == OT_ERROR_NONE ? "joining" : "dataset rejected: %s",
                 otThreadErrorToString(error));
    } else {
        snprintf(s_phase, sizeof(s_phase), "waiting for a dataset");
        ESP_LOGW(TAG, "router mode needs the network dataset, set it in the web UI");
    }
}

static void thread_task(void *arg)
{
    esp_vfs_eventfd_config_t eventfd_config = {.max_fds = 3};
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&eventfd_config));
    esp_openthread_register_rcp_failure_handler(on_rcp_failure);

    snprintf(s_phase, sizeof(s_phase), "starting OpenThread");
    ESP_ERROR_CHECK(esp_openthread_start(&s_config));
    otInstance *instance = esp_openthread_get_instance();

    esp_openthread_lock_acquire(portMAX_DELAY);
    otCliInit(instance, cli_output, NULL);
    esp_openthread_lock_release();

    if (s_border_router) {
        snprintf(s_phase, sizeof(s_phase), "waiting for the Ethernet backbone");
        while (!gw_eth_wait_ip(pdMS_TO_TICKS(30000))) {
            ESP_LOGW(TAG, "no IP on Ethernet yet, border router start deferred");
        }
        esp_openthread_lock_acquire(portMAX_DELAY);
        start_border_router(instance);
        esp_openthread_lock_release();
        esp_br_web_start_server("/spiffs");
        snprintf(s_phase, sizeof(s_phase), "running");
    } else {
        esp_openthread_lock_acquire(portMAX_DELAY);
        start_router(instance);
        esp_openthread_lock_release();
    }
    s_started = true;
    vTaskDelete(NULL);
}

/* ---- status ---- */

static cJSON *thread_status(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", s_border_router ? "border_router" : "router");
    cJSON_AddStringToObject(json, "phase", s_phase);
    cJSON_AddNumberToObject(json, "rcp_failures", s_rcp_failures);
    if (s_border_router) {
        cJSON_AddNumberToObject(json, "rest_port", CONFIG_GW_OTBR_REST_PORT);
    }
    if (!s_started || !esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        return json;
    }
    otInstance *ot = esp_openthread_get_instance();
    char buf[64];

    cJSON_AddStringToObject(json, "role", otThreadDeviceRoleToString(otThreadGetDeviceRole(ot)));
    cJSON_AddStringToObject(json, "rcp_version", otGetRadioVersionString(ot));
    cJSON_AddStringToObject(json, "network_name", otThreadGetNetworkName(ot));
    cJSON_AddNumberToObject(json, "channel", otLinkGetChannel(ot));
    snprintf(buf, sizeof(buf), "0x%04x", otLinkGetPanId(ot));
    cJSON_AddStringToObject(json, "pan_id", buf);
    add_hex(json, "ext_pan_id", otThreadGetExtendedPanId(ot)->m8, OT_EXT_PAN_ID_SIZE);
    add_hex(json, "ext_address", otLinkGetExtendedAddress(ot)->m8, OT_EXT_ADDRESS_SIZE);
    snprintf(buf, sizeof(buf), "0x%04x", otThreadGetRloc16(ot));
    cJSON_AddStringToObject(json, "rloc16", buf);
    cJSON_AddNumberToObject(json, "partition_id", otThreadGetPartitionId(ot));
    cJSON_AddNumberToObject(json, "leader_router_id", otThreadGetLeaderRouterId(ot));
    otIp6Prefix ml_prefix = {.mLength = 64};
    memcpy(ml_prefix.mPrefix.mFields.m8, otThreadGetMeshLocalPrefix(ot)->m8, OT_MESH_LOCAL_PREFIX_SIZE);
    add_prefix(json, "mesh_local_prefix", &ml_prefix);

    if (s_border_router) {
        cJSON *br = cJSON_AddObjectToObject(json, "border_router");
        static const char *const br_states[] = {"uninitialized", "disabled", "stopped", "running"};
        otBorderRoutingState state = otBorderRoutingGetState(ot);
        cJSON_AddStringToObject(br, "routing", state <= OT_BORDER_ROUTING_STATE_RUNNING ? br_states[state] : "?");
        otIp6Prefix prefix;
        if (otBorderRoutingGetOmrPrefix(ot, &prefix) == OT_ERROR_NONE) {
            add_prefix(br, "omr_prefix", &prefix);
        }
        if (otBorderRoutingGetOnLinkPrefix(ot, &prefix) == OT_ERROR_NONE) {
            add_prefix(br, "on_link_prefix", &prefix);
        }
        static const char *const nat64_states[] = {"disabled", "not running", "idle", "active"};
        otNat64State nat64 = otNat64GetPrefixManagerState(ot);
        cJSON_AddStringToObject(br, "nat64", nat64 <= OT_NAT64_STATE_ACTIVE ? nat64_states[nat64] : "?");
        cJSON_AddBoolToObject(br, "border_agent", otBorderAgentIsActive(ot));
        static const char *const srp_states[] = {"disabled", "running", "stopped"};
        otSrpServerState srp = otSrpServerGetState(ot);
        cJSON_AddStringToObject(br, "srp_server", srp <= OT_SRP_SERVER_STATE_STOPPED ? srp_states[srp] : "?");
        int hosts = 0, services = 0;
        for (const otSrpServerHost *h = otSrpServerGetNextHost(ot, NULL); h; h = otSrpServerGetNextHost(ot, h)) {
            if (otSrpServerHostIsDeleted(h)) {
                continue;
            }
            hosts++;
            for (const otSrpServerService *s = otSrpServerHostGetNextService(h, NULL); s;
                 s = otSrpServerHostGetNextService(h, s)) {
                services++;
            }
        }
        cJSON_AddNumberToObject(br, "srp_hosts", hosts);
        cJSON_AddNumberToObject(br, "srp_services", services);
    }

    cJSON *neighbors = cJSON_AddArrayToObject(json, "neighbors");
    otNeighborInfoIterator it = OT_NEIGHBOR_INFO_ITERATOR_INIT;
    otNeighborInfo n;
    while (otThreadGetNextNeighborInfo(ot, &it, &n) == OT_ERROR_NONE) {
        cJSON *item = cJSON_CreateObject();
        add_hex(item, "ext_address", n.mExtAddress.m8, OT_EXT_ADDRESS_SIZE);
        snprintf(buf, sizeof(buf), "0x%04x", n.mRloc16);
        cJSON_AddStringToObject(item, "rloc16", buf);
        cJSON_AddStringToObject(item, "type", n.mIsChild ? (n.mRxOnWhenIdle ? "child" : "sleepy child") : "router");
        cJSON_AddNumberToObject(item, "avg_rssi", n.mAverageRssi);
        cJSON_AddNumberToObject(item, "last_rssi", n.mLastRssi);
        cJSON_AddNumberToObject(item, "lqi_in", n.mLinkQualityIn);
        cJSON_AddNumberToObject(item, "age_s", n.mAge);
        cJSON_AddNumberToObject(item, "frame_error_pct", n.mFrameErrorRate * 100.0 / 0xffff);
        cJSON_AddItemToArray(neighbors, item);
    }

    cJSON *routers = cJSON_AddArrayToObject(json, "routers");
    for (uint8_t id = 0; id <= otThreadGetMaxRouterId(ot); id++) {
        otRouterInfo r;
        if (otThreadGetRouterInfo(ot, id, &r) != OT_ERROR_NONE || !r.mAllocated) {
            continue;
        }
        cJSON *item = cJSON_CreateObject();
        cJSON_AddNumberToObject(item, "id", r.mRouterId);
        snprintf(buf, sizeof(buf), "0x%04x", r.mRloc16);
        cJSON_AddStringToObject(item, "rloc16", buf);
        cJSON_AddNumberToObject(item, "next_hop", r.mNextHop);
        cJSON_AddNumberToObject(item, "path_cost", r.mPathCost);
        cJSON_AddNumberToObject(item, "lq_in", r.mLinkQualityIn);
        cJSON_AddNumberToObject(item, "lq_out", r.mLinkQualityOut);
        cJSON_AddBoolToObject(item, "link", r.mLinkEstablished);
        cJSON_AddItemToArray(routers, item);
    }

    const otMacCounters *mac = otLinkGetCounters(ot);
    cJSON *counters = cJSON_AddObjectToObject(json, "mac_counters");
    cJSON_AddNumberToObject(counters, "tx_total", mac->mTxTotal);
    cJSON_AddNumberToObject(counters, "tx_retry", mac->mTxRetry);
    cJSON_AddNumberToObject(counters, "tx_err_cca", mac->mTxErrCca);
    cJSON_AddNumberToObject(counters, "tx_err_abort", mac->mTxErrAbort);
    cJSON_AddNumberToObject(counters, "rx_total", mac->mRxTotal);
    cJSON_AddNumberToObject(counters, "rx_err_fcs", mac->mRxErrFcs);
    cJSON_AddNumberToObject(counters, "rx_err_sec", mac->mRxErrSec);
    const otMleCounters *mle = otThreadGetMleCounters(ot);
    cJSON_AddNumberToObject(counters, "attach_attempts", mle->mAttachAttempts);
    cJSON_AddNumberToObject(counters, "partition_changes", mle->mPartitionIdChanges);

    esp_openthread_lock_release();
    return json;
}

static esp_err_t status_get(httpd_req_t *req)
{
    return gw_http_send_json(req, thread_status());
}

/* Summary for /api/status; the full detail comes from /api/thread/status. */
static cJSON *thread_summary(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", s_border_router ? "border_router" : "router");
    cJSON_AddStringToObject(json, "phase", s_phase);
    if (s_started && esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        otInstance *ot = esp_openthread_get_instance();
        cJSON_AddStringToObject(json, "role", otThreadDeviceRoleToString(otThreadGetDeviceRole(ot)));
        cJSON_AddStringToObject(json, "network_name", otThreadGetNetworkName(ot));
        cJSON_AddNumberToObject(json, "channel", otLinkGetChannel(ot));
        esp_openthread_lock_release();
    }
    return json;
}

/* ---- dataset ---- */

static esp_err_t dataset_get(httpd_req_t *req)
{
    char query[32], value[8];
    bool reveal = httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
                  httpd_query_key_value(query, "reveal", value, sizeof(value)) == ESP_OK && strcmp(value, "1") == 0;
    if (!s_started || !esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        return gw_http_send_error(req, 503, "OpenThread not running");
    }
    otInstance *ot = esp_openthread_get_instance();
    otOperationalDatasetTlvs tlvs;
    otOperationalDataset ds;
    otError error = otDatasetGetActiveTlvs(ot, &tlvs);
    if (error == OT_ERROR_NONE) {
        error = otDatasetParseTlvs(&tlvs, &ds);
    }
    esp_openthread_lock_release();
    if (error != OT_ERROR_NONE) {
        return gw_http_send_error(req, 404, "no active dataset");
    }

    cJSON *json = cJSON_CreateObject();
    char buf[16];
    if (reveal) {
        add_hex(json, "tlvs", tlvs.mTlvs, tlvs.mLength);
        add_hex(json, "network_key", ds.mNetworkKey.m8, OT_NETWORK_KEY_SIZE);
    } else {
        cJSON_AddStringToObject(json, "network_key", "hidden");
    }
    cJSON_AddStringToObject(json, "network_name", ds.mNetworkName.m8);
    cJSON_AddNumberToObject(json, "channel", ds.mChannel);
    snprintf(buf, sizeof(buf), "0x%04x", ds.mPanId);
    cJSON_AddStringToObject(json, "pan_id", buf);
    add_hex(json, "ext_pan_id", ds.mExtendedPanId.m8, OT_EXT_PAN_ID_SIZE);
    cJSON_AddNumberToObject(json, "active_timestamp", (double)ds.mActiveTimestamp.mSeconds);
    return gw_http_send_json(req, json);
}

static esp_err_t dataset_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    otOperationalDatasetTlvs tlvs = {0};
    const char *hex = GW_JSON_STR(body, "tlvs");
    int len = hex ? hex_decode(hex, tlvs.mTlvs, sizeof(tlvs.mTlvs)) : -1;
    cJSON_Delete(body);
    otOperationalDataset parsed;
    if (len <= 0) {
        return gw_http_send_error(req, 400, "tlvs must be the hex dataset from Home Assistant");
    }
    tlvs.mLength = len;
    if (otDatasetParseTlvs(&tlvs, &parsed) != OT_ERROR_NONE || !parsed.mComponents.mIsNetworkKeyPresent) {
        return gw_http_send_error(req, 400, "not a valid active dataset");
    }
    if (!s_border_router) {
        gw_config_set_blob(GW_KEY_TR_DATASET, &tlvs, sizeof(tlvs));
    }
    if (!s_started || !esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        return gw_http_send_error(req, 503, "OpenThread not running");
    }
    otError error = apply_dataset(esp_openthread_get_instance(), &tlvs);
    esp_openthread_lock_release();
    if (error != OT_ERROR_NONE) {
        return gw_http_send_error(req, 400, otThreadErrorToString(error));
    }
    snprintf(s_phase, sizeof(s_phase), s_border_router ? "running" : "joining");
    ESP_LOGI(TAG, "dataset applied: %s channel %u", parsed.mNetworkName.m8, parsed.mChannel);
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "network_name", parsed.mNetworkName.m8);
    return gw_http_send_json(req, json);
}

/* ---- web CLI ---- */

static esp_err_t cli_post(httpd_req_t *req)
{
    cJSON *body = gw_http_read_json(req);
    if (!body) {
        return ESP_OK;
    }
    const char *cmd = GW_JSON_STR(body, "cmd");
    cJSON *wait_item = cJSON_GetObjectItem(body, "wait_ms");
    int wait_ms = cJSON_IsNumber(wait_item) ? wait_item->valueint : 600;
    wait_ms = wait_ms < 100 ? 100 : wait_ms > 15000 ? 15000 : wait_ms;
    if (!cmd || !cmd[0] || strlen(cmd) > 200) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 400, "cmd required");
    }
    if (!s_started) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 503, "OpenThread not running");
    }
    if (xSemaphoreTake(s_cli_lock, pdMS_TO_TICKS(100)) != pdTRUE) {
        cJSON_Delete(body);
        return gw_http_send_error(req, 409, "another CLI command is running");
    }
    char line[208];
    strlcpy(line, cmd, sizeof(line));
    cJSON_Delete(body);

    s_cli_len = 0;
    s_cli_capture = true;
    if (esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        otCliInputLine(line);
        esp_openthread_lock_release();
        /* Many commands (ping, scan, discover) answer asynchronously. */
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
    }
    s_cli_capture = false;

    xSemaphoreTake(s_cli_buf_lock, portMAX_DELAY);
    s_cli_buf[s_cli_len] = '\0';
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "output", s_cli_buf);
    xSemaphoreGive(s_cli_buf_lock);
    xSemaphoreGive(s_cli_lock);
    return gw_http_send_json(req, json);
}

esp_err_t role_thread_start(bool border_router)
{
    s_border_router = border_router;
    s_cli_lock = xSemaphoreCreateMutex();
    s_cli_buf_lock = xSemaphoreCreateMutex();
    s_cli_buf = heap_caps_malloc(CLI_BUF_SIZE, MALLOC_CAP_SPIRAM);
    ESP_RETURN_ON_FALSE(s_cli_lock && s_cli_buf_lock && s_cli_buf, ESP_ERR_NO_MEM, TAG, "no mem");

    gw_http_register("/api/thread/status", HTTP_GET, status_get);
    gw_http_register("/api/thread/dataset", HTTP_GET, dataset_get);
    gw_http_register("/api/thread/dataset", HTTP_POST, dataset_post);
    gw_http_register("/api/thread/cli", HTTP_POST, cli_post);
    gw_api_set_role_status(thread_summary);

    return xTaskCreate(thread_task, "thread_start", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
