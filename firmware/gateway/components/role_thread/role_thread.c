#include "role_thread.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
#include "esp_timer.h"
#include "esp_vfs_eventfd.h"
#include "freertos/semphr.h"
#include "lwip/sockets.h"
#include "freertos/task.h"
#include "gw.h"
#include "netscan.h"
#include "openthread/border_agent.h"
#include "openthread/border_routing.h"
#include "openthread/cli.h"
#include "openthread/dataset.h"
#include "openthread/dataset_ftd.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/link.h"
#include "openthread/mesh_diag.h"
#include "openthread/nat64.h"
#include "openthread/srp_server.h"
#include "openthread/thread.h"
#include "openthread/thread_ftd.h"
#include "sdkconfig.h"

static const char *TAG = "role_thread";

#define CLI_BUF_SIZE 8192
#define TELEMETRY_GUARD_US (3 * 1000 * 1000) /* a telemetry request normally takes a few ms */
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
    gw_restart_on_failure();
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

/*
 * The router role and the border router role share one OpenThread active dataset in NVS.
 * The border router keeps its own copy so that a detour through the router role (which joins
 * whatever network it was given) never moves the border router to another network.
 */
static void save_br_dataset(otInstance *instance)
{
    otOperationalDatasetTlvs active, saved;
    if (otDatasetGetActiveTlvs(instance, &active) != OT_ERROR_NONE) {
        return;
    }
    if (gw_config_get_blob(GW_KEY_BR_DATASET, &saved, sizeof(saved)) && saved.mLength == active.mLength &&
        memcmp(saved.mTlvs, active.mTlvs, active.mLength) == 0) {
        return;
    }
    gw_config_set_blob(GW_KEY_BR_DATASET, &active, sizeof(active));
    ESP_LOGI(TAG, "border router dataset saved (%s)", otThreadGetNetworkName(instance));
}

static void start_border_router(otInstance *instance)
{
    otOperationalDatasetTlvs dataset;
    esp_openthread_set_backbone_netif(gw_eth_netif());
    ESP_ERROR_CHECK(esp_openthread_border_router_init());

    if (gw_config_get_blob(GW_KEY_BR_DATASET, &dataset, sizeof(dataset)) && dataset.mLength > 0) {
        ESP_LOGI(TAG, "using the saved border router dataset");
    } else if (otDatasetGetActiveTlvs(instance, &dataset) != OT_ERROR_NONE) {
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

static esp_timer_handle_t s_tel_guard;
static char s_rcp_version[96];
static bool s_tel_off;

/*
 * The telemetry request did not finish. When the RCP fails on a request, OpenThread recovers it and
 * sends the request again, holding the OpenThread lock: the gateway would hang. Never ask this RCP
 * image again and restart.
 */
static void telemetry_guard(void *arg)
{
    gw_config_set_str(GW_KEY_RCP_TEL_OFF, s_rcp_version);
    ESP_LOGE(TAG, "RCP hangs on the telemetry request, telemetry off for \"%s\", restarting", s_rcp_version);
    gw_restart_on_failure();
}

/* Telemetry of the C6 from a vendor spinel property; an RCP without it answers "not found". */
static void read_rcp_telemetry(otInstance *instance)
{
    if (s_tel_off) {
        return;
    }
    if (!s_rcp_version[0]) {
        char off[sizeof(s_rcp_version)];
        strlcpy(s_rcp_version, otGetRadioVersionString(instance), sizeof(s_rcp_version));
        const esp_timer_create_args_t guard = {.callback = telemetry_guard, .name = "rcp_tel_guard"};
        if ((gw_config_get_str(GW_KEY_RCP_TEL_OFF, off, sizeof(off)) && strcmp(off, s_rcp_version) == 0) ||
            esp_timer_create(&guard, &s_tel_guard) != ESP_OK) {
            ESP_LOGW(TAG, "C6 telemetry off for this RCP image");
            s_tel_off = true;
            return;
        }
    }
    char buf[GW_C6_TELEMETRY_MAX];
    size_t len = sizeof(buf) - 1;
    esp_timer_start_once(s_tel_guard, TELEMETRY_GUARD_US);
    esp_err_t err = esp_openthread_rcp_get_vendor_data(GW_C6_TELEMETRY_SPINEL_PROP, (uint8_t *)buf, &len);
    esp_timer_stop(s_tel_guard);
    if (err == ESP_OK) {
        buf[len] = '\0';
        gw_c6_telemetry_set(buf);
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
    /* Every 30 s: follow dataset changes made by Home Assistant (REST) or the web UI, read the C6 telemetry. */
    while (true) {
        esp_openthread_lock_acquire(portMAX_DELAY);
        if (s_border_router) {
            save_br_dataset(instance);
        }
        read_rcp_telemetry(instance);
        esp_openthread_lock_release();
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
}

/* ---- quick scan of the radio surroundings (System > Network surroundings) ---- */

#define SCAN_ALL_CHANNELS 0x07fff800 /* 11..26 */
#define SCAN_WAIT pdMS_TO_TICKS(30000)

typedef struct {
    SemaphoreHandle_t done;
    cJSON *list;
} scan_ctx_t;

static void hex_into(char *out, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        sprintf(out + 2 * i, "%02x", p[i]);
    }
}

static void on_energy(otEnergyScanResult *r, void *arg)
{
    scan_ctx_t *ctx = arg;
    if (!r) {
        xSemaphoreGive(ctx->done);
        return;
    }
    cJSON *ch = cJSON_CreateObject();
    cJSON_AddNumberToObject(ch, "ch", r->mChannel);
    cJSON_AddNumberToObject(ch, "ed_max", r->mMaxRssi);
    cJSON_AddItemToArray(ctx->list, ch);
}

static void on_scan(otActiveScanResult *r, void *arg)
{
    scan_ctx_t *ctx = arg;
    if (!r) {
        xSemaphoreGive(ctx->done);
        return;
    }
    char buf[20];
    cJSON *n = cJSON_CreateObject();
    cJSON_AddNumberToObject(n, "ch", r->mChannel);
    snprintf(buf, sizeof(buf), "0x%04x", r->mPanId);
    cJSON_AddStringToObject(n, "pan", buf);
    hex_into(buf, r->mExtAddress.m8, sizeof(r->mExtAddress.m8));
    cJSON_AddStringToObject(n, "src", buf);
    cJSON_AddNumberToObject(n, "rssi", r->mRssi);
    cJSON_AddNumberToObject(n, "lqi", r->mLqi);
    if (r->mNetworkName.m8[0]) {
        cJSON_AddStringToObject(n, "type", "thread");
        cJSON_AddStringToObject(n, "name", r->mNetworkName.m8);
        hex_into(buf, r->mExtendedPanId.m8, sizeof(r->mExtendedPanId.m8));
        cJSON_AddStringToObject(n, "epid", buf);
        cJSON_AddBoolToObject(n, "joinable", r->mIsJoinable);
    } else {
        cJSON_AddStringToObject(n, "type", r->mDiscover ? "thread" : "no_payload");
    }
    cJSON_AddItemToArray(ctx->list, n);
}

/* One OpenThread scan: started with the lock held, waited for without it. */
static esp_err_t run_scan(const char *what, otError (*start)(otInstance *, scan_ctx_t *), scan_ctx_t *ctx,
                          char *err, size_t err_len)
{
    if (!esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        snprintf(err, err_len, "%s: OpenThread busy", what);
        return ESP_ERR_TIMEOUT;
    }
    otError e = start(esp_openthread_get_instance(), ctx);
    esp_openthread_lock_release();
    if (e != OT_ERROR_NONE) {
        snprintf(err, err_len, "%s: %s", what, otThreadErrorToString(e));
        return ESP_FAIL;
    }
    if (xSemaphoreTake(ctx->done, SCAN_WAIT) != pdTRUE) {
        snprintf(err, err_len, "%s: no result in time", what);
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

static otError start_energy(otInstance *ot, scan_ctx_t *ctx)
{
    return otLinkEnergyScan(ot, SCAN_ALL_CHANNELS, 200, on_energy, ctx);
}

static otError start_active(otInstance *ot, scan_ctx_t *ctx)
{
    return otLinkActiveScan(ot, SCAN_ALL_CHANNELS, 0, on_scan, ctx);
}

static otError start_discover(otInstance *ot, scan_ctx_t *ctx)
{
    return otThreadDiscover(ot, SCAN_ALL_CHANNELS, 0xffff, false, false, on_scan, ctx);
}

static esp_err_t quick_scan(cJSON *result, char *err, size_t err_len)
{
    if (!s_started) {
        snprintf(err, err_len, "OpenThread is not running yet");
        return ESP_ERR_INVALID_STATE;
    }
    scan_ctx_t ctx = {.done = xSemaphoreCreateBinary()};
    if (!ctx.done) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret;
    ctx.list = cJSON_AddArrayToObject(result, "channels");
    ret = run_scan("energy scan", start_energy, &ctx, err, err_len);
    if (ret == ESP_OK) {
        ctx.list = cJSON_AddArrayToObject(result, "beacons");
        ret = run_scan("active scan", start_active, &ctx, err, err_len);
    }
    if (ret == ESP_OK) {
        ctx.list = cJSON_AddArrayToObject(result, "thread");
        ret = run_scan("Thread discovery", start_discover, &ctx, err, err_len);
    }
    vSemaphoreDelete(ctx.done);
    return ret;
}

/* ---- topology map (Thread > Topology) ---- */

static SemaphoreHandle_t s_topo_busy;
static SemaphoreHandle_t s_topo_done;
static cJSON *s_topo_routers;
static otError s_topo_error;

/* IPv6 addresses of a router or child, lets the web UI match it with a Matter node */
static void add_ip6_list(cJSON *obj, otMeshDiagIp6AddrIterator *it)
{
    cJSON *list = cJSON_AddArrayToObject(obj, "ip6");
    otIp6Address addr;
    char buf[OT_IP6_ADDRESS_STRING_SIZE];
    while (it && otMeshDiagGetNextIp6Address(it, &addr) == OT_ERROR_NONE) {
        otIp6AddressToString(&addr, buf, sizeof(buf));
        cJSON_AddItemToArray(list, cJSON_CreateString(buf));
    }
}

static void add_ext(cJSON *obj, const char *key, const otExtAddress *ext)
{
    char buf[17];
    hex_into(buf, ext->m8, sizeof(ext->m8));
    cJSON_AddStringToObject(obj, key, buf);
}

/* Runs in the OpenThread task, once per router; error OT_ERROR_PENDING means more routers follow. */
static void on_topology(otError error, otMeshDiagRouterInfo *r, void *ctx)
{
    if (r) {
        char buf[8];
        cJSON *n = cJSON_CreateObject();
        snprintf(buf, sizeof(buf), "0x%04x", r->mRloc16);
        cJSON_AddStringToObject(n, "rloc16", buf);
        cJSON_AddNumberToObject(n, "id", r->mRouterId);
        add_ext(n, "ext", &r->mExtAddress);
        if (r->mVersion != OT_MESH_DIAG_VERSION_UNKNOWN) {
            cJSON_AddNumberToObject(n, "version", r->mVersion);
        }
        cJSON_AddBoolToObject(n, "leader", r->mIsLeader);
        cJSON_AddBoolToObject(n, "br", r->mIsBorderRouter);
        cJSON_AddBoolToObject(n, "this", r->mIsThisDevice);
        add_ip6_list(n, r->mIp6AddrIterator);
        cJSON *links = cJSON_AddArrayToObject(n, "links");
        for (int id = 0; id <= OT_NETWORK_MAX_ROUTER_ID; id++) {
            if (r->mLinkQualities[id]) {
                cJSON *l = cJSON_CreateObject();
                cJSON_AddNumberToObject(l, "id", id);
                cJSON_AddNumberToObject(l, "lq", r->mLinkQualities[id]);
                cJSON_AddItemToArray(links, l);
            }
        }
        cJSON *children = cJSON_AddArrayToObject(n, "children");
        otMeshDiagChildInfo c;
        while (r->mChildIterator && otMeshDiagGetNextChildInfo(r->mChildIterator, &c) == OT_ERROR_NONE) {
            cJSON *ch = cJSON_CreateObject();
            snprintf(buf, sizeof(buf), "0x%04x", c.mRloc16);
            cJSON_AddStringToObject(ch, "rloc16", buf);
            cJSON_AddNumberToObject(ch, "lq", c.mLinkQuality);
            cJSON_AddBoolToObject(ch, "rx_on", c.mMode.mRxOnWhenIdle);
            cJSON_AddBoolToObject(ch, "ftd", c.mMode.mDeviceType);
            cJSON_AddBoolToObject(ch, "br", c.mIsBorderRouter);
            cJSON_AddItemToArray(children, ch);
        }
        otMeshDiagTlvInfo tlv;
        while (r->mTlvIterator && otMeshDiagGetNextTlvInfo(r->mTlvIterator, &tlv) == OT_ERROR_NONE) {
            if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_NAME && tlv.mData.mVendorName[0]) {
                cJSON_AddStringToObject(n, "vendor", tlv.mData.mVendorName);
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_MODEL && tlv.mData.mVendorModel[0]) {
                cJSON_AddStringToObject(n, "model", tlv.mData.mVendorModel);
            } else if (tlv.mType == OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_SW_VERSION && tlv.mData.mVendorSwVersion[0]) {
                cJSON_AddStringToObject(n, "sw", tlv.mData.mVendorSwVersion);
            }
        }
        cJSON_AddItemToArray(s_topo_routers, n);
    }
    if (error != OT_ERROR_PENDING) {
        s_topo_error = error;
        xSemaphoreGive(s_topo_done);
    }
}

/* IPv6 addresses of the MTD children of one parent, added to the children of that router in the map */
static cJSON *s_topo_children;

static void on_child_ip6(otError error, uint16_t rloc16, otMeshDiagIp6AddrIterator *it, void *ctx)
{
    if (it) {
        char buf[8];
        snprintf(buf, sizeof(buf), "0x%04x", rloc16);
        cJSON *c;
        cJSON_ArrayForEach(c, s_topo_children)
        {
            const char *r = GW_JSON_STR(c, "rloc16");
            if (r && strcmp(r, buf) == 0 && !cJSON_GetObjectItem(c, "ip6")) {
                add_ip6_list(c, it);
            }
        }
    }
    if (error != OT_ERROR_PENDING) {
        xSemaphoreGive(s_topo_done);
    }
}

static void query_children_ip6(cJSON *routers)
{
    cJSON *r;
    cJSON_ArrayForEach(r, routers)
    {
        cJSON *children = cJSON_GetObjectItem(r, "children");
        const char *rloc = GW_JSON_STR(r, "rloc16");
        if (!rloc || cJSON_GetArraySize(children) == 0) {
            continue;
        }
        s_topo_children = children;
        xSemaphoreTake(s_topo_done, 0);
        otError e = OT_ERROR_BUSY;
        if (esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
            e = otMeshDiagQueryChildrenIp6Addrs(esp_openthread_get_instance(), (uint16_t)strtol(rloc, NULL, 16),
                                                on_child_ip6, NULL);
            esp_openthread_lock_release();
        }
        if (e == OT_ERROR_NONE && xSemaphoreTake(s_topo_done, pdMS_TO_TICKS(12000)) != pdTRUE) {
            esp_openthread_lock_acquire(portMAX_DELAY);
            otMeshDiagCancel(esp_openthread_get_instance());
            esp_openthread_lock_release();
        }
    }
    s_topo_children = NULL;
}

static esp_err_t topology_get(httpd_req_t *req)
{
    if (!s_started) {
        return gw_http_send_error(req, 409, "OpenThread not running");
    }
    if (xSemaphoreTake(s_topo_busy, 0) != pdTRUE) {
        return gw_http_send_error(req, 409, "a topology discovery is running");
    }
    static const uint8_t extra[] = {OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_NAME, OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_MODEL,
                                    OT_NETWORK_DIAGNOSTIC_TLV_VENDOR_SW_VERSION};
    const otMeshDiagDiscoverConfig cfg = {
        .mDiscoverIp6Addresses = true,
        .mDiscoverChildTable = true,
        .mExtraTlvTypes = extra,
        .mExtraTlvTypesLength = sizeof(extra)};
    int64_t start = esp_timer_get_time();
    cJSON *json = cJSON_CreateObject();
    s_topo_routers = cJSON_AddArrayToObject(json, "routers");
    xSemaphoreTake(s_topo_done, 0);
    otError e = OT_ERROR_BUSY;
    if (esp_openthread_lock_acquire(OT_LOCK_TIMEOUT)) {
        /* routers far away in a busy mesh need more than the default 5 s */
        otMeshDiagSetResponseTimeout(esp_openthread_get_instance(), 10000);
        e = otMeshDiagDiscoverTopology(esp_openthread_get_instance(), &cfg, on_topology, NULL);
        esp_openthread_lock_release();
    }
    if (e == OT_ERROR_NONE && xSemaphoreTake(s_topo_done, pdMS_TO_TICKS(30000)) != pdTRUE) {
        /* stop the callbacks before the result is sent and freed */
        esp_openthread_lock_acquire(portMAX_DELAY);
        otMeshDiagCancel(esp_openthread_get_instance());
        esp_openthread_lock_release();
        s_topo_error = OT_ERROR_RESPONSE_TIMEOUT;
    } else if (e != OT_ERROR_NONE) {
        s_topo_error = e;
    }
    query_children_ip6(s_topo_routers);
    if (s_topo_error != OT_ERROR_NONE) {
        /* a router that does not answer leaves the map incomplete, the rest is still useful */
        cJSON_AddStringToObject(json, "error", otThreadErrorToString(s_topo_error));
    }
    cJSON_AddNumberToObject(json, "ms", (double)((esp_timer_get_time() - start) / 1000));
    s_topo_routers = NULL;
    xSemaphoreGive(s_topo_busy);
    return gw_http_send_json(req, json);
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
/* Last client of the REST API (Home Assistant OTBR integration): shown in the header and used to
 * suggest the Matter Server address (the add-on runs on the same host). */
static portMUX_TYPE s_rest_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_rest_client[48];
static int64_t s_rest_client_us;

esp_err_t esp_br_web_client_open(httpd_handle_t hd, int sockfd)
{
    struct sockaddr_storage peer;
    socklen_t len = sizeof(peer);
    char ip[48] = "";
    if (getpeername(sockfd, (struct sockaddr *)&peer, &len) == 0) {
        if (peer.ss_family == AF_INET6) {
            const struct sockaddr_in6 *a = (const struct sockaddr_in6 *)&peer;
            /* IPv4 clients reach the dual-stack socket as ::ffff:a.b.c.d */
            if (IN6_IS_ADDR_V4MAPPED(&a->sin6_addr)) {
                inet_ntop(AF_INET, &a->sin6_addr.s6_addr[12], ip, sizeof(ip));
            } else {
                inet_ntop(AF_INET6, &a->sin6_addr, ip, sizeof(ip));
            }
        } else if (peer.ss_family == AF_INET) {
            inet_ntop(AF_INET, &((const struct sockaddr_in *)&peer)->sin_addr, ip, sizeof(ip));
        }
    }
    if (ip[0]) {
        portENTER_CRITICAL(&s_rest_mux);
        strlcpy(s_rest_client, ip, sizeof(s_rest_client));
        s_rest_client_us = esp_timer_get_time();
        portEXIT_CRITICAL(&s_rest_mux);
        /* keep it across restarts: written when the address changes or once an hour */
        static gw_ha_seen_t saved;
        static bool loaded;
        if (!loaded) {
            gw_config_get_blob(GW_KEY_HA_SEEN, &saved, sizeof(saved));
            loaded = true;
        }
        time_t now = time(NULL);
        if (strcmp(saved.ip, ip) != 0 || (now > 1700000000 && now - saved.unix_time > 3600)) {
            strlcpy(saved.ip, ip, sizeof(saved.ip));
            saved.unix_time = now > 1700000000 ? now : 0;
            gw_config_set_blob(GW_KEY_HA_SEEN, &saved, sizeof(saved));
        }
    }
    return ESP_OK; /* anything else would close the connection */
}

static void add_rest_client(cJSON *json)
{
    char ip[48];
    int64_t at;
    portENTER_CRITICAL(&s_rest_mux);
    strlcpy(ip, s_rest_client, sizeof(ip));
    at = s_rest_client_us;
    portEXIT_CRITICAL(&s_rest_mux);
    if (ip[0]) {
        cJSON_AddStringToObject(json, "rest_client", ip);
        cJSON_AddNumberToObject(json, "rest_client_age_s", (double)((esp_timer_get_time() - at) / 1000000));
        return;
    }
    gw_ha_seen_t saved;
    if (gw_config_get_blob(GW_KEY_HA_SEEN, &saved, sizeof(saved)) && saved.ip[0]) {
        /* not seen since this start: the last contact before it */
        cJSON_AddStringToObject(json, "rest_client", saved.ip);
        cJSON_AddNumberToObject(json, "rest_client_at", (double)saved.unix_time);
    }
}

static cJSON *thread_summary(void)
{
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "mode", s_border_router ? "border_router" : "router");
    cJSON_AddStringToObject(json, "phase", s_phase);
    add_rest_client(json);
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
    s_topo_busy = xSemaphoreCreateMutex();
    s_topo_done = xSemaphoreCreateBinary();
    gw_http_register("/api/thread/topology", HTTP_GET, topology_get);
    gw_api_set_role_status(thread_summary);
    netscan_set_quick(quick_scan);

    return xTaskCreate(thread_task, "thread_start", 6144, NULL, 5, NULL) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
