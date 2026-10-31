idf_component_register(SRCS "role_thread.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_flasher
                       PRIV_REQUIRES gw_core netscan openthread esp_ot_br_server vfs esp_netif nvs_flash)

# REST API port of the patched esp_ot_br_server (the web UI keeps port 80).
idf_component_get_property(br_server_lib esp_ot_br_server COMPONENT_LIB)
target_compile_definitions(${br_server_lib} PRIVATE ESP_BR_WEB_PORT=${CONFIG_GW_OTBR_REST_PORT})
# Upstream builds with -Os; at -O2 GCC 14 reports a false positive strcat overlap.
target_compile_options(${br_server_lib} PRIVATE -Wno-error=restrict)
