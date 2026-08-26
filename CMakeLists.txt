cmake_minimum_required(VERSION 3.16)

set(FIRMWARE_VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/firmware_version.txt")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${FIRMWARE_VERSION_FILE}")
file(READ "${FIRMWARE_VERSION_FILE}" PROJECT_VER_FILE)
string(STRIP "${PROJECT_VER_FILE}" PROJECT_VER_FILE)
set(PROJECT_VER "${PROJECT_VER_FILE}" CACHE STRING "Firmware version" FORCE)

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
