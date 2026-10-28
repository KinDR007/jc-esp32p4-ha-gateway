idf_component_register(SRCS "role_zigbee.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core c6_flasher esp_driver_uart esp_timer lwip)
