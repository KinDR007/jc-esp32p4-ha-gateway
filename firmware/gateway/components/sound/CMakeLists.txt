idf_component_register(SRCS "sound.c" "sound_mel.c" "sound_model.cpp"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core esp_driver_i2s esp_driver_i2c esp_partition esp_timer
                       EMBED_TXTFILES "yamnet_class_map.csv")

# Build only the ops and int8 kernels YAMNet needs (esp-dl compile-time stripping).
idf_component_get_property(espdl_dir espressif__esp-dl COMPONENT_DIR)
include(${espdl_dir}/cmake/compile_register.cmake)
esp_dl_compile_add_yml(${CMAKE_CURRENT_LIST_DIR}/yamnet.compile_req.yml)
