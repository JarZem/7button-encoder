/*
 * Compile ota_service.c through this wrapper so the HTTPS port is taken only
 * from the authenticated secure provisioning stored in NVS. There is no
 * compile-time/default OTA server port in the firmware.
 */
#include <stdint.h>

#include "esp_err.h"
#include "ota_secure_session.h"

static uint16_t ota_service_runtime_port(void)
{
    ota_secure_provisioning_t provisioning = {0};
    if (ota_secure_session_load_provisioning(&provisioning) != ESP_OK ||
        provisioning.ota_port == 0) {
        return 0;
    }
    const uint16_t port = provisioning.ota_port;
    ota_secure_session_clear_provisioning_copy(&provisioning);
    return port;
}

#ifdef CONFIG_APP_OTA_SERVER_PORT
#undef CONFIG_APP_OTA_SERVER_PORT
#endif
#define CONFIG_APP_OTA_SERVER_PORT ota_service_runtime_port()

#include "ota_service.c"
