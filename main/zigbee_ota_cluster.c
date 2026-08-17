#include "zigbee_ota_cluster.h"

#include <string.h>
#include "esp_log.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster)
{
    s_ota_payload_attr[0] = 0;
    return esp_zb_custom_cluster_add_custom_attr(
        cluster,
        ZIGBEE_OTA_CONFIG_ATTR_ID,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE,
        s_ota_payload_attr);
}

bool zigbee_ota_cluster_handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL ||
        message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS ||
        message->info.dst_endpoint != ZIGBEE_OTA_ENDPOINT ||
        message->info.cluster != ZIGBEE_OTA_CLUSTER_ID ||
        message->attribute.id != ZIGBEE_OTA_CONFIG_ATTR_ID) {
        return false;
    }

    const esp_zb_zcl_attribute_data_t *data = &message->attribute.data;
    if (data->value == NULL) {
        ESP_LOGW(TAG, "OTA ERROR: invalid payload");
        return true;
    }

    const uint8_t *zcl_string = (const uint8_t *)data->value;
    size_t payload_len = 0;
    size_t value_offset = 0;
    if (data->type == ESP_ZB_ZCL_ATTR_TYPE_LONG_CHAR_STRING) {
        if (data->size < 2) {
            ESP_LOGW(TAG, "OTA ERROR: invalid payload");
            return true;
        }
        payload_len = (size_t)zcl_string[0] | ((size_t)zcl_string[1] << 8);
        value_offset = 2;
    } else if (data->type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
        if (data->size < 1) {
            ESP_LOGW(TAG, "OTA ERROR: invalid payload");
            return true;
        }
        payload_len = zcl_string[0];
        value_offset = 1;
    } else {
        ESP_LOGW(TAG, "OTA ERROR: invalid payload");
        return true;
    }

    ESP_LOGI(TAG, "OTA: Zigbee command received");
    ESP_LOGI(TAG, "OTA: payload length = %u", (unsigned)payload_len);
    if (payload_len == 0 ||
        payload_len > OTA_CONFIG_MAX_PAYLOAD_LEN ||
        payload_len > ZIGBEE_OTA_ZCL_STRING_CAPACITY ||
        payload_len + value_offset > data->size) {
        ESP_LOGW(TAG, "OTA ERROR: invalid payload");
        return true;
    }

    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &zcl_string[value_offset], payload_len);
    payload[payload_len] = '\0';

    if (!ota_service_request_payload(payload, payload_len)) {
        ESP_LOGW(TAG, "OTA ERROR: request ignored: busy or queue full");
    }
    memset(payload, 0, sizeof(payload));
    return true;
}
