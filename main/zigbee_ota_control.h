#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_endpoint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_OTA_CONTROL_ENDPOINT           11
#define ZIGBEE_OTA_ENABLE_CLUSTER_ID          0xfc01
#define ZIGBEE_OTA_STATUS_CLUSTER_ID          0xfc02
#define ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE  0x1234
#define ZIGBEE_OTA_ENABLE_ATTR_ID             0x0000
#define ZIGBEE_OTA_STATUS_ATTR_ID             0x0000

typedef enum {
    ZIGBEE_OTA_STATUS_IDLE                    = 0,
    ZIGBEE_OTA_STATUS_PROVISIONING_STARTED    = 1,
    ZIGBEE_OTA_STATUS_PROVISIONING_COMPLETE   = 2,
    ZIGBEE_OTA_STATUS_FW_UPDATE_STARTED       = 16,
    ZIGBEE_OTA_STATUS_FW_UPDATE_COMPLETE      = 17,
    ZIGBEE_OTA_STATUS_PROVISIONING_ERROR      = 32,
    ZIGBEE_OTA_STATUS_PROVISIONING_TIMEOUT    = 33,
    ZIGBEE_OTA_STATUS_FW_UPDATE_ERROR         = 48,
    ZIGBEE_OTA_STATUS_FW_VERIFY_ERROR         = 49,
    ZIGBEE_OTA_STATUS_FW_SKIPPED              = 64,
} zigbee_ota_status_t;

esp_err_t zigbee_ota_control_add_endpoint(esp_zb_ep_list_t *ep_list);
bool zigbee_ota_control_handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message);
bool zigbee_ota_control_is_enabled(void);
void zigbee_ota_control_set_status(zigbee_ota_status_t status);
uint8_t zigbee_ota_control_get_status(void);

#ifdef __cplusplus
}
#endif
