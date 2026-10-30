#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_partition.h"
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
#define GW_KEY_HA_SEEN "ha_seen"       /* blob gw_ha_seen_t, last client of the OTBR REST API */
#define GW_KEY_MATTER_URL "matter_url" /* str, Matter Server WebSocket for device names in the Thread map */
#define GW_KEY_ZR_TXPOWER "zr_txpower" /* i8 as u8+128, Zigbee router transmit power (restored after a reflash) */
#define GW_KEY_THR_TXPOWER "thr_txpower" /* i8 as u8+128, Thread radio transmit power, 0 = default */
#define GW_KEY_FAIL_RST "fail_rst"   /* u8, the last software restart was a recovery from a failure */
#define GW_KEY_ESPHOME_TRY "esph_try" /* u8, set before booting ESPHome, cleared by its on_boot */
#define GW_KEY_TR_DATASET "tr_dataset" /* blob, dataset TLVs for the Thread router role */
#define GW_KEY_BR_DATASET "br_dataset" /* blob, dataset TLVs of the Thread border router role */
#define GW_KEY_UPDATE_URL "upd_url" /* str, manifest of the online update, empty = off */
#define GW_KEY_LAN "lan"            /* str JSON, LAN settings (gw_lan_set), none = DHCP */
#define GW_KEY_ZB_SNAP_DAYS "zb_snap_days" /* u8, automatic Zigbee network backup every n days, 0 = off */
#define GW_ZB_SNAP_DAYS_DEFAULT 7
#define GW_KEY_ZB_SNAP_REQ "zb_snap_req" /* u8, back up the Zigbee network data on the next start */

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
/* Settings backup: the "p4gw" namespace without per-board state (boot counters, C6 state, last
 * scan), passwords only with secrets. {"<key>": {"u8": n} | {"str": "..."} | {"blob": "<base64>"}} */
cJSON *gw_config_export(bool secrets);
/* Writes the keys of an export over the current ones (keys missing in it stay); per-board state
 * keys are ignored. err names the first bad entry. */
esp_err_t gw_config_import(const cJSON *keys, char *err, size_t err_len);

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
/* Same, and resets the C6 just before (a restart of the whole hardware). */
void gw_restart_later_full(uint32_t delay_ms, bool c6);
/* Restart to recover from a failure (C6 radio lost). Counts as an unstable boot for gw_boot_loop_detected(),
 * unlike a requested restart, so a failure that comes back after every start ends in maintenance mode. */
void gw_restart_on_failure(void) __attribute__((noreturn));

/* ---- Ethernet ---- */
esp_err_t gw_eth_init(const char *hostname);
esp_netif_t *gw_eth_netif(void);
/* Waits for an IPv4 or global IPv6 address. */
bool gw_eth_wait_ip(TickType_t timeout);
cJSON *gw_eth_status(void);
/* Bytes and packets through the Ethernet interface since boot, in total and per service. */
cJSON *gw_eth_traffic(void);
/* LAN settings: DHCP or static IPv4, link mode. A change applies at once and must be confirmed
 * (gw_lan_confirm) within 120 s, else the stored settings come back. */
esp_err_t gw_lan_set(const cJSON *json, char *err, size_t err_len);
esp_err_t gw_lan_confirm(void);
cJSON *gw_lan_status(void);

/* ---- internet reachability (TCP to github.com:443 every minute) ---- */
void gw_inet_start(void);
/* -1 not checked yet, 0 unreachable, 1 reachable */
int gw_inet_state(void);
/* seconds since the last check, -1 before the first */
double gw_inet_age_s(void);

/* ---- log ring buffer ---- */
esp_err_t gw_log_init(size_t size);
/* Copies log text starting at *seq, advances *seq. Returns bytes copied. */
size_t gw_log_read(uint32_t *seq, char *out, size_t len);
/* Last 16 KiB of log of the run before the last restart (not after a power-on); 0 when none. */
size_t gw_log_previous(const char **text);

/* ---- users and permissions (gw_auth.c) ---- */
void gw_auth_init(void);
void gw_auth_register(void);
/* true when at least one user exists (else the gateway is open) */
bool gw_auth_enabled(void);
/* Identifies the client of req and checks its right to the registered uri; sends 401/403 and
 * returns false when it has none. Runs in the HTTP server task. */
bool gw_auth_check(httpd_req_t *req, const char *uri);
/* name of the user or API token of the request being handled, "" when open */
const char *gw_auth_who(void);
int gw_auth_request_count(void);

/* ---- notifications (gw_notify.c): security, system, network, sound ---- */
typedef enum { GW_NOTIFY_INFO, GW_NOTIFY_WARNING, GW_NOTIFY_CRITICAL } gw_notify_level_t;
void gw_notify(const char *topic, gw_notify_level_t level, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

/* ---- HTTP ---- */
esp_err_t gw_http_start(void);
httpd_handle_t gw_http_server(void);
/* Registers an API handler behind the auth check. */
esp_err_t gw_http_register(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *));
/* Without sign-in (the login page). */
esp_err_t gw_http_register_public(const char *uri, httpd_method_t method, esp_err_t (*handler)(httpd_req_t *));
esp_err_t gw_http_send_json(httpd_req_t *req, cJSON *json);
/* Sends {"topic","payload"} to the clients of /api/ws (what goes to MQTT, without a broker). */
void gw_http_push(const char *topic, const char *payload);
/* Clients connected to /api/ws (the Home Assistant integration). Call from the server task. */
int gw_http_ws_clients(void);
esp_err_t gw_http_send_error(httpd_req_t *req, int status, const char *message);
/* Reads a JSON body (max 4 KiB). NULL after an error response was sent. */
cJSON *gw_http_read_json(httpd_req_t *req);
cJSON *gw_http_read_json_max(httpd_req_t *req, size_t max);
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

/* Long P4 flash writes and the sound model exclude each other: every flash write stalls both
 * cores, and a model run slowed down that way keeps CPU 1 busy until the task watchdog fires.
 * Uploads and C6 flashing hold the lock for the whole operation (after a model run in progress
 * ends); recognition runs the model only when it gets the lock at once. */
void gw_flash_lock(void);
void gw_flash_unlock(void);
bool gw_flash_try_lock(void);
/* Right to write a gateway image (uploads, online update), with the flash lock: false while another
 * upload or C6 flashing runs. */
bool gw_upload_begin(void);
void gw_upload_end(void);
/* The gateway slot (gw_a/gw_b) not running now: target of gateway updates. Never the esphome slot. */
const esp_partition_t *gw_other_gateway_slot(void);

/* Online update from a manifest URL (GW_KEY_UPDATE_URL):
 *   {"version": "v1.5", "p4gw": "p4gw.bin", "notes": "...", "release_url": "..."}
 * "p4gw" may be relative to the manifest. Checked every 6 h; offered to Home Assistant as an MQTT
 * update entity. */
void gw_update_start(void);
void gw_update_check_soon(void);
/* Starts the download and installation of the offered version; restarts when done. */
esp_err_t gw_update_install(void);
cJSON *gw_update_status(void);
/* MQTT: discovery of the update entity and its state (called by gw_mqtt on connect and on change). */
void gw_update_mqtt_discovery(void);
void gw_update_mqtt_state(void);

/* Identify: the web UI blinks an icon while it runs (the board has no LED it could drive). */
void gw_identify(int seconds);
int gw_identify_left(void);

/* Last Home Assistant contact over the OTBR REST API, kept across restarts. */
typedef struct {
    char ip[48];
    int64_t unix_time; /* 0 when the clock was not set yet */
} gw_ha_seen_t;

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
/* "off" (not configured or disabled), "connected" or "disconnected" */
const char *gw_mqtt_link(void);
/* Publishes to <base>/<suffix>, QoS 1. */
void gw_mqtt_publish(const char *suffix, const char *payload, bool retain);
const char *gw_mqtt_base_topic(void);

/* One listener for topics outside the gateway's own (Zigbee2MQTT). on_connect runs after every
 * (re)connect to subscribe; on_message gets whole messages, also those split into chunks. */
typedef struct {
    void (*on_connect)(void);
    void (*on_message)(const char *topic, const char *data, size_t len);
} gw_mqtt_listener_t;
void gw_mqtt_set_listener(const gw_mqtt_listener_t *listener);
esp_err_t gw_mqtt_subscribe(const char *filter);
esp_err_t gw_mqtt_unsubscribe(const char *filter);
esp_err_t gw_mqtt_publish_to(const char *full_topic, const char *payload);

#define GW_JSON_STR(obj, key) (cJSON_IsString(cJSON_GetObjectItem((obj), (key))) ? cJSON_GetObjectItem((obj), (key))->valuestring : NULL)

#ifdef __cplusplus
}
#endif
