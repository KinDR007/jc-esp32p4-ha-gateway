#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "gw.h"
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
}
