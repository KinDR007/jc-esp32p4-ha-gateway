idf_component_register(SRCS "gw_api.c" "gw_auth.c" "gw_boot.c" "gw_config.c" "gw_eth.c" "gw_http.c" "gw_i2c.c" "gw_inet.c" "gw_log.c" "gw_mqtt.c" "gw_notify.c" "gw_report.c" "gw_telegram.c" "gw_time.c" "gw_update.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_netif esp_partition json esp_driver_i2c
                       PRIV_REQUIRES app_update c6_flasher esp_driver_tsens esp_eth esp_psram
                                     espcoredump esp_http_client esp_https_ota mbedtls mqtt nvs_flash spi_flash esp_timer lwip espressif__qrcode)

# The web UI is a single HTML file and the API description (docs/api.md, the UI's API page) a JSON
# file, both served gzipped from flash.
idf_build_get_property(python PYTHON)
foreach(name index.html api.json)
    set(src ${CMAKE_CURRENT_LIST_DIR}/../../web/${name})
    set(gz ${CMAKE_CURRENT_BINARY_DIR}/${name}.gz)
    add_custom_command(OUTPUT ${gz}
                       COMMAND ${python} -c "import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))" ${src} ${gz}
                       DEPENDS ${src}
                       VERBATIM)
    string(REPLACE "." "_" target ${name})
    add_custom_target(gw_web_${target} DEPENDS ${gz})
    target_add_binary_data(${COMPONENT_LIB} ${gz} BINARY DEPENDS gw_web_${target})
endforeach()
target_add_binary_data(${COMPONENT_LIB} ${CMAKE_CURRENT_LIST_DIR}/../../web/p4gw_favicon.ico BINARY)
