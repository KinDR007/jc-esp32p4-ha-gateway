#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gw.h"

static const char *TAG = "gw_boot";

/* Boots that end before gw_boot_mark_stable() in a row before the gateway stops starting roles. */
#define BOOT_LOOP_LIMIT 4

bool gw_boot_loop_detected(void)
{
    uint8_t count = gw_config_get_u8(GW_KEY_BOOT_COUNT, 0);
    esp_reset_reason_t reason = esp_reset_reason();

    /* A clean power-on or a requested restart is not a crash. */
    if (reason == ESP_RST_POWERON || reason == ESP_RST_SW) {
        count = 0;
    }
    if (count < UINT8_MAX) {
        count++;
    }
    gw_config_set_u8(GW_KEY_BOOT_COUNT, count);
    ESP_LOGI(TAG, "reset reason %d, unstable boot %u/%u", reason, count, BOOT_LOOP_LIMIT);
    return count > BOOT_LOOP_LIMIT;
}

void gw_boot_mark_stable(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    /* Only while the running image is also the boot image: the call acts on the boot partition,
     * which after an upload is the new, never started image. */
    if (running == esp_ota_get_boot_partition() && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "new image confirmed");
        esp_ota_mark_app_valid_cancel_rollback();
    }
    gw_config_set_u8(GW_KEY_BOOT_COUNT, 0);
    /* The ESPHome image boots back into this slot. */
    gw_config_set_u8(GW_KEY_GW_SLOT, running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    gw_state()->stable = true;
}

esp_err_t gw_boot_select_esphome(void)
{
    const esp_partition_t *part =
        esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_2, NULL);
    esp_app_desc_t desc;

    if (!part || esp_ota_get_partition_description(part, &desc) != ESP_OK) {
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "next boot: %s %s", desc.project_name, desc.version);
    return esp_ota_set_boot_partition(part);
}

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    esp_restart();
}

void gw_restart_later(uint32_t delay_ms)
{
    /* A requested restart proves the new image runs well enough to serve requests; without
     * this, a restart within the first minute after an update would roll it back. */
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    /* Only while the running image is also the boot image: the call acts on the boot partition,
     * which after an upload is the new, never started image. */
    if (running == esp_ota_get_boot_partition() && esp_ota_get_state_partition(running, &state) == ESP_OK &&
        state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "image confirmed before the requested restart");
    }
    ESP_LOGW(TAG, "restart in %lu ms", (unsigned long)delay_ms);
    xTaskCreate(restart_task, "restart", 2048, (void *)(uintptr_t)delay_ms, 20, NULL);
}
