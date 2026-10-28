#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Values are stored in NVS and read by the ESPHome image, never renumber. */
typedef enum {
    GW_ROLE_NONE = 0,          /* maintenance: web UI only, C6 untouched */
    GW_ROLE_THREAD_BR = 1,     /* OpenThread border router, C6 = RCP */
    GW_ROLE_ZIGBEE_COORD = 2,  /* ZBOSS NCP bridged to TCP for Z2M / ZHA */
    GW_ROLE_ESPHOME = 3,       /* ESPHome BLE proxy image, C6 = esp-hosted */
    GW_ROLE_THREAD_ROUTER = 4, /* Thread router joined to an existing network, C6 = RCP */
    GW_ROLE_ZIGBEE_ROUTER = 5, /* standalone Zigbee router on the C6, P4 monitors it */
    GW_ROLE_MAX,
} gw_role_t;

const char *gw_role_name(gw_role_t role);
bool gw_role_from_name(const char *name, gw_role_t *role);
/* Name of the c6fw bundle the role runs on the C6, NULL for none. */
const char *gw_role_c6_bundle(gw_role_t role);

/* ---- persistent settings (NVS namespace "p4gw", also read by the ESPHome image) ---- */
#define GW_NVS_NAMESPACE "p4gw"
#define GW_KEY_ROLE "role"          /* u8 gw_role_t, desired role */
#define GW_KEY_PASSWORD "password"  /* str, empty = no auth */
#define GW_KEY_HOSTNAME "hostname"  /* str, empty = default */
#define GW_KEY_BOOT_COUNT "bootcnt" /* u8, boots without reaching stable uptime */
#define GW_KEY_GW_SLOT "gw_slot"    /* u8, ota index of the last running gateway image */
#define GW_KEY_C6_BUNDLE "c6_name"  /* str, bundle flashed on the C6 */
#define GW_KEY_C6_SHA "c6_sha"      /* blob[32], its sha256 */
#define GW_KEY_C6_FORCE "c6_force"  /* u8, reflash on next boot (RCP failure) */
#define GW_KEY_C6_TRIES "c6_tries"  /* u8, consecutive flash attempts */
#define GW_KEY_C6_ADOPTED "c6_ok"   /* u8, the user allowed overwriting the C6 factory firmware */
#define GW_KEY_RCP_TEL_OFF "rcp_tel_off" /* str, RCP version that hung on the telemetry request */
#define GW_KEY_MATTER_URL "matter_url" /* str, Matter Server WebSocket for device names in the Thread map */
#define GW_KEY_FAIL_RST "fail_rst"   /* u8, the last software restart was a recovery from a failure */
#define GW_KEY_ESPHOME_TRY "esph_try" /* u8, set before booting ESPHome, cleared by its on_boot */
#define GW_KEY_TR_DATASET "tr_dataset" /* blob, dataset TLVs for the Thread router role */
#define GW_KEY_BR_DATASET "br_dataset" /* blob, dataset TLVs of the Thread border router role */

esp_err_t gw_config_init(void);
uint8_t gw_config_get_u8(const char *key, uint8_t def);
esp_err_t gw_config_set_u8(const char *key, uint8_t value);
/* Returns false and an empty string when missing. */
bool gw_config_get_str(const char *key, char *out, size_t len);
esp_err_t gw_config_set_str(const char *key, const char *value);
bool gw_config_get_blob(const char *key, void *out, size_t len);
esp_err_t gw_config_set_blob(const char *key, const void *value, size_t len);
esp_err_t gw_config_blob_size(const char *key, size_t *len);
esp_err_t gw_config_erase(const char *key);

/* ---- runtime state ---- */
typedef struct {
    gw_role_t desired;          /* from NVS */
    gw_role_t active;           /* what actually runs */
    char maintenance[96];       /* why the gateway fell back to GW_ROLE_NONE */
    char hostname[32];
    bool stable;                /* survived the stability window */
} gw_state_t;

gw_state_t *gw_state(void);
void gw_set_maintenance(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- boot control ---- */
/* Counts boots that crash before reaching stable uptime; true when the limit is hit. */
bool gw_boot_loop_detected(void);
/* Marks the image valid and resets the boot loop counter, called once the gateway is up. */
void gw_boot_mark_stable(void);
/* Boots the ESPHome slot on the next restart. */
esp_err_t gw_boot_select_esphome(void);
/* Restarts after a short delay so pending HTTP responses get out. */
void gw_restart_later(uint32_t delay_ms);
/* Restart to recover from a failure (C6 radio lost). Counts as an unstable boot for gw_boot_loop_detected(),
 * unlike a requested restart, so a failure that comes back after every start ends in maintenance mode. */
void gw_restart_on_failure(void) __attribute__((noreturn));

/* ---- Ethernet ---- */
esp_err_t gw_eth_init(const char *hostname);
esp_netif_t *gw_eth_netif(void);
/* Waits for an IPv4 or global IPv6 address. */
bool gw_eth_wait_ip(TickType_t timeout);
cJSON *gw_eth_status(void);

/* ---- log ring buffer ---- */
esp_err_t gw_log_init(size_t size);
/* Copies log text starting at *seq, advances *seq. Returns bytes copied. */
size_t gw_log_read(uint32_t *seq, char *out, size_t len);

/* ---- HTTP ---- */
esp_err_t gw_http_start(void);
httpd_handle_t gw_http_server(void);
/* Registers an API handler behind the auth check. */
esp_err_t gw_http_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *));
esp_err_t gw_http_send_json(httpd_req_t *req, cJSON *json);
esp_err_t gw_http_send_error(httpd_req_t *req, int status, const char *message);
/* Reads a JSON body (max 4 KiB). NULL after an error response was sent. */
cJSON *gw_http_read_json(httpd_req_t *req);
/* Streams the request body to sink in full 4 KiB blocks (the last one may be shorter). */
typedef esp_err_t (*gw_sink_fn)(void *ctx, uint32_t offset, const void *data, size_t len);
esp_err_t gw_http_receive(httpd_req_t *req, gw_sink_fn sink, void *ctx);
/* System API: status, logs, role switching, OTA, settings, tools. */
esp_err_t gw_api_register(void);
/* The running role adds its summary to /api/status under "role_status". */
void gw_api_set_role_status(cJSON *(*provider)(void));
cJSON *gw_api_role_status(void);
bool gw_api_temperature(float *celsius);

/*
 * Runtime telemetry of the C6 (one JSON object from firmware/c6/common/p4gw_telemetry), delivered by the
 * role over its C6 link: spinel vendor property (Thread RCP), telemetry frame (Zigbee NCP), @ZT line (router).
 */
#define GW_C6_TELEMETRY_MAX 256
#define GW_C6_TELEMETRY_SPINEL_PROP 0x3cf0 /* P4GW_SPINEL_PROP_TELEMETRY of the C6 side */
void gw_c6_telemetry_set(const char *json);

/* Optional MQTT link with Home Assistant discovery, configured via /api/mqtt. */
esp_err_t gw_mqtt_start(void);
/* Extra discovery entities (called on every connect) and extra keys in the state JSON. */
void gw_mqtt_set_extension(void (*discovery)(void), void (*state)(cJSON *state));
/* One retained discovery config; value_template reads the shared state topic, extra adds fields. */
void gw_mqtt_entity(const char *component, const char *object_id, const char *name, const char *value_template,
                    void (*extra)(cJSON *cfg));
/* Publishes a discovery config (unique_id, availability and device are added); cfg is consumed.
 * cfg NULL removes the entity. component "device_automation" for device triggers. */
void gw_mqtt_discovery(const char *component, const char *object_id, cJSON *cfg);
bool gw_mqtt_connected(void);
/* Publishes to <base>/<suffix>, QoS 1. */
void gw_mqtt_publish(const char *suffix, const char *payload, bool retain);
const char *gw_mqtt_base_topic(void);

#define GW_JSON_STR(obj, key) (cJSON_IsString(cJSON_GetObjectItem((obj), (key))) ? cJSON_GetObjectItem((obj), (key))->valuestring : NULL)

#ifdef __cplusplus
}
#endif
