idf_component_register(SRCS "role_esphome.c"
                       INCLUDE_DIRS "include"
                       REQUIRES c6_flasher
                       PRIV_REQUIRES gw_core esp_driver_uart esp_timer json)
