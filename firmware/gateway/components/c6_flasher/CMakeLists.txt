idf_component_register(SRCS "c6_flasher.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES esp_mm esp_driver_gpio esp_partition mbedtls esp-serial-flasher esp_rom nvs_flash esp_timer)
