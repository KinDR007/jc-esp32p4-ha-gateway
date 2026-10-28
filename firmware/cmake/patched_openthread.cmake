# Copies the ESP-IDF openthread component to build/patched_idf/ and applies
# firmware/patches/idf_openthread_telemetry.patch (C6 telemetry over a vendor spinel property:
# RCP side hook, P4 side getter). Used by the gateway and the ot_rcp builds; the pinned ESP-IDF
# checkout stays untouched. Sets P4GW_PATCHED_OPENTHREAD to the component directory.
get_filename_component(_p4gw_root ${CMAKE_CURRENT_LIST_DIR}/../.. ABSOLUTE)
set(_ot_patch ${_p4gw_root}/firmware/patches/idf_openthread_telemetry.patch)
set(_ot_dir ${_p4gw_root}/build/patched_idf/openthread)
file(SHA256 ${_ot_patch} _ot_hash)
if(NOT EXISTS ${_ot_dir}/.patched_${_ot_hash})
    message(STATUS "Patching a copy of the ESP-IDF openthread component")
    file(REMOVE_RECURSE ${_ot_dir})
    file(MAKE_DIRECTORY ${_p4gw_root}/build/patched_idf)
    file(COPY $ENV{IDF_PATH}/components/openthread DESTINATION ${_p4gw_root}/build/patched_idf)
    execute_process(COMMAND patch -p0 -s -i ${_ot_patch} WORKING_DIRECTORY ${_ot_dir} RESULT_VARIABLE _ot_result)
    if(NOT _ot_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${_ot_patch}")
    endif()
    file(TOUCH ${_ot_dir}/.patched_${_ot_hash})
endif()
# A changed patch re-runs the configure step, which makes a fresh copy.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${_ot_patch})
set(P4GW_PATCHED_OPENTHREAD ${_ot_dir})
