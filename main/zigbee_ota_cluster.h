#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_zigbee_attribute.h"
#include "zcl/esp_zigbee_zcl_command.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ZIGBEE_OTA_CLUSTER_ID                    0xfc00
#define ZIGBEE_OTA_MANUFACTURER_CODE             0x1234
#define ZIGBEE_OTA_CONFIG_ATTR_ID                0x0001
#define ZIGBEE_OTA_CMD_PROVISION_CONFIG_ID       0x01
#define ZIGBEE_OTA_CMD_OTA_CHECK_ID              0x02
#define ZIGBEE_OTA_CMD_OTA_STATUS_ID             0x03
#define ZIGBEE_OTA_CMD_DEVICE_AUTH_CHALLENGE_ID  0x04
#define ZIGBEE_OTA_CMD_DEVICE_ENROLL_ID          0x05
#define ZIGBEE_OTA_CMD_COMMAND_ACK_ID             0x06
#define ZIGBEE_OTA_ENDPOINT                      1

/*
 * Empirical safe ceiling for esp_zb_zcl_report_attr_cmd_req() with this
 * manufacturer-specific CHAR_STRING attribute on ESP32-C6 / ESP Zigbee SDK.
 * 42+ byte reports trigger a ZBOSS assertion in zcl_general_commands.c.
 * Keep this guard below the observed crash point; longer protocol messages
 * must be fragmented before they reach the ZCL report API.
 */
#define ZIGBEE_OTA_ZCL_STRING_CAPACITY           40

/* Logical HELLO size before fragmentation. */
#define ZIGBEE_OTA_HELLO_FRAME_MAX               112

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster);
void zigbee_ota_schedule_hello(uint32_t delay_ms);
bool zigbee_ota_cluster_handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message);

#ifdef __cplusplus
}
#endif
