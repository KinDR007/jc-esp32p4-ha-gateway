idf_component_register(SRCS "sensors.c" "sensor_drv.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core esp_driver_i2c esp_timer json)
