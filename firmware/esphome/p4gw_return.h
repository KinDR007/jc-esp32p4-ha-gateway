#pragma once
// Hand-over between the ESPHome image (ota_2) and the P4 gateway (ota_0/ota_1).
// Keys and role numbers must match firmware/gateway/components/gw_core/include/gw.h.
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "nvs.h"

namespace p4gw {

enum Role : uint8_t { THREAD_BR = 1, ZIGBEE_COORDINATOR = 2, THREAD_ROUTER = 4, ZIGBEE_ROUTER = 5 };

// Tells the gateway that ESPHome booted, so it does not treat the next gateway boot as a rollback.
inline void confirm_boot() {
  nvs_handle_t h;
  if (nvs_open("p4gw", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_key(h, "esph_try");
    nvs_commit(h);
    nvs_close(h);
  }
}

// Selects the gateway slot for the next boot with the given role. The caller restarts.
inline bool return_to_gateway(uint8_t role) {
  nvs_handle_t h;
  uint8_t slot = 0;
  if (nvs_open("p4gw", NVS_READWRITE, &h) != ESP_OK)
    return false;
  nvs_get_u8(h, "gw_slot", &slot);
  nvs_set_u8(h, "role", role);
  nvs_set_u8(h, "c6_tries", 0);
  nvs_commit(h);
  nvs_close(h);
  auto subtype = static_cast<esp_partition_subtype_t>(ESP_PARTITION_SUBTYPE_APP_OTA_MIN + (slot & 1));
  const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_APP, subtype, nullptr);
  esp_app_desc_t desc;
  if (part == nullptr || esp_ota_get_partition_description(part, &desc) != ESP_OK)
    return false;
  return esp_ota_set_boot_partition(part) == ESP_OK;
}

inline uint8_t role_from_option(const std::string &option) {
  if (option == "Thread router")
    return THREAD_ROUTER;
  if (option == "Zigbee coordinator")
    return ZIGBEE_COORDINATOR;
  if (option == "Zigbee router")
    return ZIGBEE_ROUTER;
  return THREAD_BR;
}

}  // namespace p4gw
