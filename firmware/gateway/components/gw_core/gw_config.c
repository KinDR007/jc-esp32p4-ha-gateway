#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "gw.h"
#include "mbedtls/base64.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "gw_config";

static nvs_handle_t s_nvs;
static gw_state_t s_state;

static const char *const s_role_names[GW_ROLE_MAX] = {
    [GW_ROLE_NONE] = "none",
    [GW_ROLE_THREAD_BR] = "thread_br",
    [GW_ROLE_ZIGBEE_COORD] = "zigbee_coordinator",
    [GW_ROLE_ESPHOME] = "esphome",
    [GW_ROLE_THREAD_ROUTER] = "thread_router",
    [GW_ROLE_ZIGBEE_ROUTER] = "zigbee_router",
};

const char *gw_role_name(gw_role_t role)
{
    return role < GW_ROLE_MAX ? s_role_names[role] : "invalid";
}

bool gw_role_from_name(const char *name, gw_role_t *role)
{
    for (int i = 0; name && i < GW_ROLE_MAX; i++) {
        if (strcmp(name, s_role_names[i]) == 0) {
            *role = (gw_role_t)i;
            return true;
        }
    }
    return false;
}

const char *gw_role_c6_bundle(gw_role_t role)
{
    switch (role) {
    case GW_ROLE_THREAD_BR:
    case GW_ROLE_THREAD_ROUTER:
        return "ot_rcp";
    case GW_ROLE_ZIGBEE_COORD:
        return "zigbee_ncp";
    case GW_ROLE_ZIGBEE_ROUTER:
        return "zigbee_router";
    case GW_ROLE_ESPHOME:
        return "esp_hosted";
    default:
        return NULL;
    }
}

esp_err_t gw_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS layout changed, erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        return err;
    }
    return nvs_open(GW_NVS_NAMESPACE, NVS_READWRITE, &s_nvs);
}

uint8_t gw_config_get_u8(const char *key, uint8_t def)
{
    uint8_t value;
    return nvs_get_u8(s_nvs, key, &value) == ESP_OK ? value : def;
}

esp_err_t gw_config_set_u8(const char *key, uint8_t value)
{
    uint8_t current;
    if (nvs_get_u8(s_nvs, key, &current) == ESP_OK && current == value) {
        return ESP_OK;
    }
    esp_err_t err = nvs_set_u8(s_nvs, key, value);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

bool gw_config_get_str(const char *key, char *out, size_t len)
{
    size_t size = len;
    if (nvs_get_str(s_nvs, key, out, &size) == ESP_OK) {
        return true;
    }
    if (len) {
        out[0] = '\0';
    }
    return false;
}

esp_err_t gw_config_set_str(const char *key, const char *value)
{
    esp_err_t err = nvs_set_str(s_nvs, key, value);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

bool gw_config_get_blob(const char *key, void *out, size_t len)
{
    size_t size = len;
    return nvs_get_blob(s_nvs, key, out, &size) == ESP_OK && size == len;
}

esp_err_t gw_config_blob_size(const char *key, size_t *len)
{
    return nvs_get_blob(s_nvs, key, NULL, len);
}

esp_err_t gw_config_set_blob(const char *key, const void *value, size_t len)
{
    esp_err_t err = nvs_set_blob(s_nvs, key, value, len);
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

esp_err_t gw_config_erase(const char *key)
{
    esp_err_t err = nvs_erase_key(s_nvs, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    return err == ESP_OK ? nvs_commit(s_nvs) : err;
}

const gw_radio_keys_t *gw_radio_keys(int radio)
{
    static const gw_radio_keys_t keys[] = {
        {GW_KEY_C6_BUNDLE, GW_KEY_C6_SHA, GW_KEY_C6_FORCE, GW_KEY_C6_TRIES, GW_KEY_C6_ADOPTED},
        {"c6b_name", "c6b_sha", "c6b_force", "c6b_tries", "c6b_ok"},
    };
    return &keys[radio == 1 ? 1 : 0];
}

/* Keys that describe this board and its radios, not the user's settings: never exported or imported. */
static const char *const s_board_keys[] = {
    GW_KEY_BOOT_COUNT, GW_KEY_GW_SLOT, GW_KEY_C6_BUNDLE, GW_KEY_C6_SHA, GW_KEY_C6_FORCE, GW_KEY_C6_TRIES,
    GW_KEY_C6_ADOPTED, "c6b_*", GW_KEY_RCP_TEL_OFF, GW_KEY_HA_SEEN, GW_KEY_FAIL_RST, GW_KEY_ESPHOME_TRY,
    GW_KEY_ZB_SNAP_REQ, "netscan", "netscan_req", "auth_ses", "auth_req",
};
/* users (password hashes, 2FA secrets) and API tokens go only into a backup with passwords */
static const char *const s_secret_keys[] = {GW_KEY_PASSWORD, "mqtt_pass", "usr*", "auth_tok", "tg_token"};

/* an entry ending with '*' matches every key with that prefix */
static bool key_in(const char *key, const char *const *list, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        size_t n = strlen(list[i]);
        if (list[i][n - 1] == '*' ? strncmp(key, list[i], n - 1) == 0 : strcmp(key, list[i]) == 0) {
            return true;
        }
    }
    return false;
}

#define IS_BOARD_KEY(k) key_in((k), s_board_keys, sizeof(s_board_keys) / sizeof(s_board_keys[0]))
#define IS_SECRET_KEY(k) key_in((k), s_secret_keys, sizeof(s_secret_keys) / sizeof(s_secret_keys[0]))

static cJSON *export_entry(const nvs_entry_info_t *info)
{
    cJSON *entry = NULL;
    size_t len = 0;
    if (info->type == NVS_TYPE_U8) {
        uint8_t value;
        if (nvs_get_u8(s_nvs, info->key, &value) == ESP_OK) {
            entry = cJSON_CreateObject();
            cJSON_AddNumberToObject(entry, "u8", value);
        }
    } else if (info->type == NVS_TYPE_STR && nvs_get_str(s_nvs, info->key, NULL, &len) == ESP_OK) {
        char *value = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (value && nvs_get_str(s_nvs, info->key, value, &len) == ESP_OK) {
            entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "str", value);
        }
        free(value);
    } else if (info->type == NVS_TYPE_BLOB && nvs_get_blob(s_nvs, info->key, NULL, &len) == ESP_OK) {
        size_t out_len = 4 * ((len + 2) / 3) + 1;
        uint8_t *value = heap_caps_malloc(len ? len : 1, MALLOC_CAP_SPIRAM);
        unsigned char *text = heap_caps_malloc(out_len, MALLOC_CAP_SPIRAM);
        if (value && text && nvs_get_blob(s_nvs, info->key, value, &len) == ESP_OK &&
            mbedtls_base64_encode(text, out_len, &out_len, value, len) == 0) {
            entry = cJSON_CreateObject();
            cJSON_AddStringToObject(entry, "blob", (const char *)text);
        }
        free(value);
        free(text);
    }
    return entry;
}

cJSON *gw_config_export(bool secrets)
{
    cJSON *keys = cJSON_CreateObject();
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(NVS_DEFAULT_PART_NAME, GW_NVS_NAMESPACE, NVS_TYPE_ANY, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        if (!IS_BOARD_KEY(info.key) && (secrets || !IS_SECRET_KEY(info.key))) {
            cJSON *entry = export_entry(&info);
            if (entry) {
                cJSON_AddItemToObject(keys, info.key, entry);
            }
        }
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return keys;
}

/* Checks one entry and, when write is set, stores it (without commit). */
static esp_err_t import_entry(const cJSON *entry, bool write, char *err, size_t err_len)
{
    const char *key = entry->string;
    const cJSON *u8 = cJSON_GetObjectItem(entry, "u8");
    const cJSON *str = cJSON_GetObjectItem(entry, "str");
    const cJSON *blob = cJSON_GetObjectItem(entry, "blob");

    if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE || !cJSON_IsObject(entry)) {
        snprintf(err, err_len, "%s: not a settings entry", key);
        return ESP_ERR_INVALID_ARG;
    }
    if (cJSON_IsNumber(u8) && u8->valuedouble >= 0 && u8->valuedouble <= UINT8_MAX) {
        if (strcmp(key, GW_KEY_ROLE) == 0 && u8->valueint >= GW_ROLE_MAX) {
            snprintf(err, err_len, "%s: unknown role %d", key, u8->valueint);
            return ESP_ERR_INVALID_ARG;
        }
        return write ? nvs_set_u8(s_nvs, key, (uint8_t)u8->valueint) : ESP_OK;
    }
    if (cJSON_IsString(str)) {
        return write ? nvs_set_str(s_nvs, key, str->valuestring) : ESP_OK;
    }
    if (cJSON_IsString(blob)) {
        size_t in_len = strlen(blob->valuestring), len = 0;
        uint8_t *value = heap_caps_malloc(in_len / 4 * 3 + 3, MALLOC_CAP_SPIRAM);
        esp_err_t ret = ESP_ERR_NO_MEM;
        if (value) {
            if (mbedtls_base64_decode(value, in_len / 4 * 3 + 3, &len, (const unsigned char *)blob->valuestring,
                                      in_len) != 0) {
                snprintf(err, err_len, "%s: invalid base64", key);
                ret = ESP_ERR_INVALID_ARG;
            } else {
                ret = write ? nvs_set_blob(s_nvs, key, value, len) : ESP_OK;
            }
        }
        free(value);
        return ret;
    }
    snprintf(err, err_len, "%s: expected u8, str or blob", key);
    return ESP_ERR_INVALID_ARG;
}

esp_err_t gw_config_import(const cJSON *keys, char *err, size_t err_len)
{
    const cJSON *entry;
    if (!cJSON_IsObject(keys)) {
        snprintf(err, err_len, "keys: expected an object");
        return ESP_ERR_INVALID_ARG;
    }
    /* Everything is checked before the first write, so a bad file changes nothing. */
    for (int pass = 0; pass < 2; pass++) {
        cJSON_ArrayForEach(entry, keys)
        {
            if (IS_BOARD_KEY(entry->string)) {
                continue;
            }
            esp_err_t ret = import_entry(entry, pass == 1, err, err_len);
            if (ret != ESP_OK) {
                if (pass == 1) {
                    snprintf(err, err_len, "%s: %s", entry->string, esp_err_to_name(ret));
                }
                return ret;
            }
        }
    }
    ESP_LOGI(TAG, "settings imported");
    return nvs_commit(s_nvs);
}

gw_state_t *gw_state(void)
{
    return &s_state;
}

void gw_set_maintenance(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vsnprintf(s_state.maintenance, sizeof(s_state.maintenance), fmt, args);
    va_end(args);
    s_state.active = GW_ROLE_NONE;
    ESP_LOGE(TAG, "maintenance mode: %s", s_state.maintenance);
    gw_notify("system", GW_NOTIFY_CRITICAL, "Maintenance mode, no role runs: %s", s_state.maintenance);
}
