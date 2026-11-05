idf_component_register(SRCS "gpio_ctl.c" "gpio_uart.c" "gpio_ws2812.c" "gpio_onewire.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core sensors esp_driver_gpio esp_driver_i2c esp_driver_ledc esp_driver_pcnt esp_driver_rmt esp_driver_uart esp_timer json lwip)
