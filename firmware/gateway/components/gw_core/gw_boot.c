#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "c6_flasher.h"
#include "gw.h"

static const char *TAG = "gw_boot";

/* Boots that end before gw_boot_mark_stable() in a row before the gateway stops starting roles. */
#define BOOT_LOOP_LIMIT 4

static bool s_after_failure;

bool gw_boot_after_failure(void)
{
    return s_after_failure;
}

bool gw_boot_loop_detected(void)
{
    uint8_t count = gw_config_get_u8(GW_KEY_BOOT_COUNT, 0);
    esp_reset_reason_t reason = esp_reset_reason();
    bool failure = gw_config_get_u8(GW_KEY_FAIL_RST, 0);
    s_after_failure = failure;

    if (failure) {
        gw_config_set_u8(GW_KEY_FAIL_RST, 0);
    }
    /* A clean power-on or a requested restart is not a crash; a recovery restart is. */
    if (reason == ESP_RST_POWERON || (reason == ESP_RST_SW && !failure)) {
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

void gw_restart_on_failure(void)
{
    gw_config_set_u8(GW_KEY_FAIL_RST, 1);
    esp_restart();
}

static bool s_restart_c6;

static void restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS((uint32_t)(uintptr_t)arg));
    if (s_restart_c6) {
        /* held in reset until the P4 restart releases the pin (CHIP_PU has its own pull-up) */
        ESP_LOGW(TAG, "C6 reset");
        c6_hold_reset(C6_RADIO_A, true);
        c6_hold_reset(C6_RADIO_B, true);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    esp_restart();
}

void gw_restart_later(uint32_t delay_ms)
{
    gw_restart_later_full(delay_ms, false);
}

void gw_restart_later_full(uint32_t delay_ms, bool c6)
{
    s_restart_c6 = c6;
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
