cmake_minimum_required(VERSION 3.16)

set(ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(BR_DIR ${ROOT_DIR}/third_party/esp-thread-br)
if(NOT EXISTS ${BR_DIR}/components/esp_ot_br_server)
    message(FATAL_ERROR "esp-thread-br missing, run scripts/fetch_deps.sh")
endif()

# esp_ot_br_server is used patched (REST on its own port, explicit start), so build it from
# a patched copy and keep the pinned checkout pristine.
set(PATCHED_DIR ${CMAKE_BINARY_DIR}/patched_components)
set(BR_SERVER_PATCH ${CMAKE_CURRENT_LIST_DIR}/patches/esp_ot_br_server.patch)
file(SHA256 ${BR_SERVER_PATCH} patch_hash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${BR_SERVER_PATCH})
if(NOT EXISTS ${PATCHED_DIR}/esp_ot_br_server/.patched_${patch_hash})
    file(REMOVE_RECURSE ${PATCHED_DIR}/esp_ot_br_server)
    file(COPY ${BR_DIR}/components/esp_ot_br_server DESTINATION ${PATCHED_DIR})
    execute_process(COMMAND patch -p0 -s -i ${BR_SERVER_PATCH}
                    WORKING_DIRECTORY ${PATCHED_DIR}/esp_ot_br_server
                    RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${BR_SERVER_PATCH}")
    endif()
    file(TOUCH ${PATCHED_DIR}/esp_ot_br_server/.patched_${patch_hash})
endif()

# ESP-Hosted host (role_wifi) from the pinned checkout the co-processor is built from, patched so one
# image serves the co-processor on either radio (ESP32-C6 or ESP32-C5); role_wifi's manifest points
# the dependency at this copy (override_path, relative to the build directory build/gateway).
set(EH_DIR ${ROOT_DIR}/third_party/esp-hosted-mcu)
set(EH_PATCH ${CMAKE_CURRENT_LIST_DIR}/patches/esp_hosted_any_cp.patch)
set(EH_COPY ${PATCHED_DIR}/espressif__esp_hosted)
file(SHA256 ${EH_PATCH} eh_hash)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS ${EH_PATCH})
if(NOT EXISTS ${EH_COPY}/.patched_${eh_hash})
    if(NOT EXISTS ${EH_DIR}/host)
        message(FATAL_ERROR "esp-hosted-mcu missing, run scripts/fetch_deps.sh")
    endif()
    file(REMOVE_RECURSE ${EH_COPY})
    file(COPY ${EH_DIR}/ DESTINATION ${EH_COPY}
         PATTERN .git EXCLUDE PATTERN .github EXCLUDE PATTERN examples EXCLUDE PATTERN docs EXCLUDE)
    execute_process(COMMAND patch -p0 -s -i ${EH_PATCH} WORKING_DIRECTORY ${EH_COPY} RESULT_VARIABLE patch_result)
    if(NOT patch_result EQUAL 0)
        message(FATAL_ERROR "Failed to apply ${EH_PATCH}")
    endif()
    file(TOUCH ${EH_COPY}/.patched_${eh_hash})
endif()

include(${ROOT_DIR}/firmware/cmake/patched_openthread.cmake)

set(EXTRA_COMPONENT_DIRS
    ${PATCHED_DIR}/esp_ot_br_server
    ${P4GW_PATCHED_OPENTHREAD}
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4gw)
