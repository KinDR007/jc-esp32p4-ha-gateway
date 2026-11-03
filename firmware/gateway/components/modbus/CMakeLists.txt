idf_component_register(SRCS "modbus.c" "mb_bus.c" "mb_proto.c" "mb_template.c"
                       INCLUDE_DIRS "include"
                       PRIV_REQUIRES gw_core esp_driver_uart esp_timer json lwip)

# The built-in device templates (templates/*.json, one file per device) as one JSON array in the firmware.
file(GLOB templates CONFIGURE_DEPENDS ${CMAKE_CURRENT_LIST_DIR}/templates/*.json)
idf_build_get_property(python PYTHON)
set(bundle ${CMAKE_CURRENT_BINARY_DIR}/templates.json)
add_custom_command(OUTPUT ${bundle}
                   COMMAND ${python} -c "import json,sys;json.dump([json.load(open(f,encoding='utf-8')) for f in sorted(sys.argv[2:])],open(sys.argv[1],'w',encoding='utf-8'),separators=(',',':'),ensure_ascii=False)" ${bundle} ${templates}
                   DEPENDS ${templates}
                   VERBATIM)
add_custom_target(modbus_templates DEPENDS ${bundle})
target_add_binary_data(${COMPONENT_LIB} ${bundle} TEXT DEPENDS modbus_templates)
