idf_component_register(SRCS "main.c"
                       PRIV_REQUIRES gw_core c6_flasher role_thread role_zigbee nvs_flash esp_netif)
