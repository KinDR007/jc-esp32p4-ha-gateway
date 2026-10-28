idf_component_register(SRCS "netscan.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json
                       PRIV_REQUIRES gw_core c6_flasher esp_driver_uart esp_netif esp_timer)
