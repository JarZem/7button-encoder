cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED PROJECT_VER OR PROJECT_VER STREQUAL "")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/firmware_version.txt" PROJECT_VER)
    string(STRIP "${PROJECT_VER}" PROJECT_VER)
endif()

# BEGIN JARZEM SECURE OTA - generated, do not edit
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/external/ota_server/esp_component"
    ${EXTRA_COMPONENT_DIRS}
)
include("${CMAKE_CURRENT_LIST_DIR}/external/ota_server/esp_component/jarzem_secure_ota/bootstrap.cmake")
# END JARZEM SECURE OTA - generated

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remotecontrol7andEncoder)

# BEGIN JARZEM SECURE OTA - generated, do not edit POST
include("${CMAKE_CURRENT_LIST_DIR}/external/ota_server/esp_component/jarzem_secure_ota/post_project.cmake")
# END JARZEM SECURE OTA - generated POST
