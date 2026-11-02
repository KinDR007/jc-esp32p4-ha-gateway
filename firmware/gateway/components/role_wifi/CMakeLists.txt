idf_component_register(SRCS "role_wifi.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_flasher
                       PRIV_REQUIRES gw_core esp_driver_uart esp_netif esp_wifi esp_timer json lwip espressif__esp_hosted espressif__esp_wifi_remote)
