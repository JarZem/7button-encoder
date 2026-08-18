set(DEVICE_CREDENTIAL_DIR "${PROJECT_DIR}/device_credentials")
set(DEVICE_CERT_SOURCE "${DEVICE_CREDENTIAL_DIR}/device_cert.pem")
set(DEVICE_KEY_SOURCE "${DEVICE_CREDENTIAL_DIR}/device_private.pem")
set(ROOT_CA_SOURCE "${DEVICE_CREDENTIAL_DIR}/root_ca_cert.pem")
set(OTA_SERVER_CERT_SOURCE "${DEVICE_CREDENTIAL_DIR}/ota_server_cert.pem")

set(DEVICE_CERT_EMBED "${CMAKE_CURRENT_BINARY_DIR}/device_cert.pem")
set(DEVICE_KEY_EMBED "${CMAKE_CURRENT_BINARY_DIR}/device_private.pem")
set(ROOT_CA_EMBED "${CMAKE_CURRENT_BINARY_DIR}/root_ca_cert.pem")
set(OTA_SERVER_CERT_EMBED "${CMAKE_CURRENT_BINARY_DIR}/ota_server_cert.pem")
set(OTA_CA_BUNDLE "${CMAKE_CURRENT_BINARY_DIR}/ota_server_ca_bundle.pem")

foreach(REQUIRED_FILE IN ITEMS
        "${DEVICE_CERT_SOURCE}"
        "${DEVICE_KEY_SOURCE}"
        "${ROOT_CA_SOURCE}"
        "${OTA_SERVER_CERT_SOURCE}")
    if(NOT EXISTS "${REQUIRED_FILE}")
        message(FATAL_ERROR
            "Missing ${REQUIRED_FILE}. Run tools/create_device_credentials.py before building this device."
        )
    endif()
endforeach()

configure_file("${DEVICE_CERT_SOURCE}" "${DEVICE_CERT_EMBED}" COPYONLY)
configure_file("${DEVICE_KEY_SOURCE}" "${DEVICE_KEY_EMBED}" COPYONLY)
configure_file("${ROOT_CA_SOURCE}" "${ROOT_CA_EMBED}" COPYONLY)
configure_file("${OTA_SERVER_CERT_SOURCE}" "${OTA_SERVER_CERT_EMBED}" COPYONLY)

file(READ "${ROOT_CA_SOURCE}" ROOT_CA_CONTENT)
file(READ "${OTA_SERVER_CERT_SOURCE}" OTA_SERVER_CERT_CONTENT)
if(ROOT_CA_CONTENT MATCHES "PRIVATE KEY" OR OTA_SERVER_CERT_CONTENT MATCHES "PRIVATE KEY")
    message(FATAL_ERROR "Public trust input unexpectedly contains a private key")
endif()
file(WRITE "${OTA_CA_BUNDLE}" "${ROOT_CA_CONTENT}\n${OTA_SERVER_CERT_CONTENT}\n")

set(COMMON_EMBED_TXTFILES
    "${OTA_CA_BUNDLE}"
    "${DEVICE_CERT_EMBED}"
    "${DEVICE_KEY_EMBED}"
    "${ROOT_CA_EMBED}"
    "${OTA_SERVER_CERT_EMBED}"
)

if(CONFIG_APP_ZIGBEE_MINIMAL_TEST)
    idf_component_register(
        SRCS
            "app_main.c"
            "device_auth.c"
            "device_credentials.c"
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
            ${COMMON_EMBED_TXTFILES}
    )
else()
    idf_component_register(
        SRCS
            "app_main.c"
            "device_auth.c"
            "device_credentials.c"
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
            ${COMMON_EMBED_TXTFILES}
    )
endif()
