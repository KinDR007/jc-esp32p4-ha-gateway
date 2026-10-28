idf_component_register(SRCS "gw_api.c" "gw_boot.c" "gw_config.c" "gw_eth.c" "gw_http.c" "gw_log.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_netif json
                       PRIV_REQUIRES app_update c6_flasher esp_driver_tsens esp_eth esp_partition
                                     espcoredump mbedtls nvs_flash spi_flash esp_timer lwip)

# The web UI is a single HTML file, served gzipped from flash.
idf_build_get_property(python PYTHON)
set(web_src ${CMAKE_CURRENT_LIST_DIR}/../../web/index.html)
set(web_gz ${CMAKE_CURRENT_BINARY_DIR}/index.html.gz)
add_custom_command(OUTPUT ${web_gz}
                   COMMAND ${python} -c "import gzip,sys;open(sys.argv[2],'wb').write(gzip.compress(open(sys.argv[1],'rb').read(),9,mtime=0))" ${web_src} ${web_gz}
                   DEPENDS ${web_src}
                   VERBATIM)
add_custom_target(gw_web DEPENDS ${web_gz})
target_add_binary_data(${COMPONENT_LIB} ${web_gz} BINARY DEPENDS gw_web)
