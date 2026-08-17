set(OTA_CERT_DIR "${PROJECT_DIR}/cert")
set(OTA_CA_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/ota_server_ca_bundle.pem")
file(WRITE "${OTA_CA_BUNDLE}" "")
set(OTA_CA_BUNDLE_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/make_ota_ca_bundle.cmake")
file(WRITE "${OTA_CA_BUNDLE_SCRIPT}" [=[
file(GLOB CERT_INPUTS
    "${CERT_DIR}/*.pem"
    "${CERT_DIR}/*.crt"
    "${CERT_DIR}/*.cer"
)
if(NOT CERT_INPUTS)
    message(FATAL_ERROR
        "OTA HTTPS certificate not found. Put one or more .pem/.crt/.cer files into ${CERT_DIR}"
    )
endif()

file(WRITE "${OUTPUT}" "")
foreach(CERT_FILE IN LISTS CERT_INPUTS)
    file(READ "${CERT_FILE}" CERT_CONTENT)
    if(CERT_CONTENT MATCHES "PRIVATE KEY")
        message(FATAL_ERROR
            "Private key found in ${CERT_FILE}. Put only public OTA server CA/server certificates into ${CERT_DIR}"
        )
    endif()
    if(NOT CERT_CONTENT MATCHES "-----BEGIN CERTIFICATE-----")
        message(FATAL_ERROR
            "Certificate file ${CERT_FILE} is not PEM text with BEGIN CERTIFICATE"
        )
    endif()
    file(APPEND "${OUTPUT}" "${CERT_CONTENT}\n")
    message(STATUS "Embedded OTA certificate: ${CERT_FILE}")
endforeach()
]=])

if(CONFIG_APP_ZIGBEE_MINIMAL_TEST)
    idf_component_register(
        SRCS
            "app_main.c"
            "device_auth.c"
            "device_enrollment.c"
            "device_identity.c"
            "event_bus.c"
            "fatal_error.c"
            "input.c"
            "minimal_app_state.c"
            "ota_config.c"
            "ota_service.c"
            "ota_wifi.c"
            "status_led.c"
            "storage.c"
            "zigbee_minimal.c"
            "zigbee_ota_cluster.c"
        INCLUDE_DIRS "."
        REQUIRES
            esp_driver_gpio
            esp_timer
            esp_http_client
            esp_wifi
            esp_phy
            esp_netif
            esp_event
            esp_coex
            esp_hw_support
            ieee802154
            mbedtls
            nvs_flash
            app_update
            espressif__led_strip
            espressif__esp-zigbee-lib
        EMBED_TXTFILES
            "${OTA_CA_BUNDLE}"
    )
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        add_custom_target(ota_ca_bundle
            ALL
            COMMAND "${CMAKE_COMMAND}" "-DCERT_DIR=${OTA_CERT_DIR}" "-DOUTPUT=${OTA_CA_BUNDLE}" -P "${OTA_CA_BUNDLE_SCRIPT}"
            BYPRODUCTS "${OTA_CA_BUNDLE}"
            VERBATIM
        )
        add_dependencies(${COMPONENT_LIB} ota_ca_bundle)
    endif()
else()
    idf_component_register(
        SRCS
            "app_main.c"
            "device_auth.c"
            "device_enrollment.c"
            "device_identity.c"
            "input.c"
            "event_bus.c"
            "serial_protocol.c"
            "uart_transport.c"
            "state.c"
            "storage.c"
            "zigbee.c"
            "zigbee_ota_cluster.c"
            "ota_config.c"
            "debug_console.c"
            "ota_service.c"
            "ota_wifi.c"
            "ota_diagnostic.c"
            "power_manager.c"
            "status_led.c"
            "fatal_error.c"
        INCLUDE_DIRS "."
        REQUIRES
            esp_driver_gpio
            esp_driver_uart
            esp_driver_usb_serial_jtag
            nvs_flash
            app_update
            esp_timer
            esp_http_client
            esp_wifi
            esp_phy
            esp_netif
            esp_event
            esp_coex
            esp_hw_support
            ieee802154
            esp_pm
            mbedtls
            espressif__led_strip
            espressif__esp-zigbee-lib
        EMBED_TXTFILES
            "${OTA_CA_BUNDLE}"
    )
    if(NOT CMAKE_SCRIPT_MODE_FILE)
        add_custom_target(ota_ca_bundle
            ALL
            COMMAND "${CMAKE_COMMAND}" "-DCERT_DIR=${OTA_CERT_DIR}" "-DOUTPUT=${OTA_CA_BUNDLE}" -P "${OTA_CA_BUNDLE_SCRIPT}"
            BYPRODUCTS "${OTA_CA_BUNDLE}"
            VERBATIM
        )
        add_dependencies(${COMPONENT_LIB} ota_ca_bundle)
    endif()
endif()
