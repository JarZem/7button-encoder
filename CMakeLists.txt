cmake_minimum_required(VERSION 3.16)

# Keep project configuration isolated in the build directory. This prevents an
# old local project-root sdkconfig (for example UART console settings) from
# overriding the tracked sdkconfig.defaults. Normal `idf.py flash monitor` will
# therefore always configure the ESP32-C6 console from the repository defaults.
set(SDKCONFIG "${CMAKE_BINARY_DIR}/sdkconfig")
set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")

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
