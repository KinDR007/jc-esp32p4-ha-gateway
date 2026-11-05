idf_component_register(SRCS "gpio_ctl.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core esp_driver_gpio esp_driver_ledc esp_driver_pcnt esp_timer json)
