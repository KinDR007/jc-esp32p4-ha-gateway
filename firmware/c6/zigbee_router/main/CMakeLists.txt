idf_component_register(SRCS "zigbee_router.c"
                       PRIV_REQUIRES esp_driver_uart esp_app_format nvs_flash vfs esp_timer)
