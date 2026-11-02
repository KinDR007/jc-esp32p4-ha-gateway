idf_component_register(SRCS "netscan.c"
                       INCLUDE_DIRS "include"
                       REQUIRES json c6_flasher
                       PRIV_REQUIRES gw_core esp_driver_uart esp_netif esp_timer)
