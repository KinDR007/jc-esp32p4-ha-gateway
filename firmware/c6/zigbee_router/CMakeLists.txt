cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS $ENV{ROOT_DIR}/firmware/c6/common)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(p4gw_zigbee_router)
