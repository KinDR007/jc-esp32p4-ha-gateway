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

set(EXTRA_COMPONENT_DIRS
    ${PATCHED_DIR}/esp_ot_br_server
    $ENV{IDF_PATH}/examples/common_components/protocol_examples_common)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4gw)
