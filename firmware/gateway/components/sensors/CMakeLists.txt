idf_component_register(SRCS "sensors.c" "sensor_drv.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_driver_i2c
                       PRIV_REQUIRES gw_core esp_timer json)
