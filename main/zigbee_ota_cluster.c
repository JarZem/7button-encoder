#include "zigbee_ota_cluster.h"

#include <stdio.h>
#include <string.h>

#include "device_identity.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;

static bool zigbee_ota_is_joined(void)
{
    if (esp_zb_bdb_is_factory_new()) {
        return false;
    }

    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static esp_err_t zigbee_ota_report_payload(const char *payload)
{
    if (payload == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    const size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > ZIGBEE_OTA_ZCL_STRING_CAPACITY) {
        return ESP_ERR_INVALID_SIZE;
    }

    s_ota_payload_attr[0] = (uint8_t)payload_len;
    memcpy(&s_ota_payload_attr[1], payload, payload_len);

    esp_zb_lock_acquire(portMAX_DELAY);

    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        ZIGBEE_OTA_ENDPOINT,
        ZIGBEE_OTA_CLUSTER_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZIGBEE_OTA_CONFIG_ATTR_ID,
        s_ota_payload_attr,
        false);

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        esp_zb_lock_release();
        ESP_LOGW(TAG, "HELLO set attr failed status=0x%x", status);
        return ESP_FAIL;
    }

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,
            .dst_endpoint = 1,
            .src_endpoint = ZIGBEE_OTA_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ZIGBEE_OTA_CLUSTER_ID,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = ZIGBEE_OTA_CONFIG_ATTR_ID,
    };

    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "HELLO report cluster=0x%04x attr=0x%04x payload=%s ret=%s(0x%x)",
             ZIGBEE_OTA_CLUSTER_ID,
             ZIGBEE_OTA_CONFIG_ATTR_ID,
             payload,
             esp_err_to_name(err),
             err);
    return err;
}

static void zigbee_ota_hello_task(void *arg)
{
    (void)arg;

    for (unsigned attempt = 1; attempt <= 120; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        if (!zigbee_ota_is_joined()) {
            continue;
        }

        /* Let the stack settle after reconnect/join before the first custom report. */
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!zigbee_ota_is_joined()) {
            continue;
        }

        char device_id[DEVICE_ID_MAX_LEN] = {0};
        esp_err_t id_err = device_identity_get_device_id(device_id);
        if (id_err != ESP_OK || device_id[0] == '\0' || strcmp(device_id, "00:00:00:00:00:00:00:00") == 0) {
            ESP_LOGW(TAG, "HELLO device id not ready: %s value=%s",
                     esp_err_to_name(id_err),
                     device_id);
            continue;
        }

        char payload[DEVICE_ID_MAX_LEN + 3];
        const int written = snprintf(payload, sizeof(payload), "H|%s", device_id);
        if (written <= 0 || (size_t)written >= sizeof(payload)) {
            ESP_LOGE(TAG, "HELLO payload build failed");
            break;
        }

        const uint16_t short_addr = esp_zb_get_short_address();
        esp_err_t err = zigbee_ota_report_payload(payload);
        if (err == ESP_OK) {
            ESP_LOGI(TAG, "HELLO sent after Zigbee join short=0x%04x", short_addr);
            break;
        }

        ESP_LOGW(TAG, "HELLO send failed: %s; retrying", esp_err_to_name(err));
    }

    s_hello_task_started = false;
    vTaskDelete(NULL);
}

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster)
{
    s_ota_payload_attr[0] = 0;
    esp_err_t err = esp_zb_custom_cluster_add_custom_attr(
        cluster,
        ZIGBEE_OTA_CONFIG_ATTR_ID,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        s_ota_payload_attr);
    if (err != ESP_OK) {
        return err;
    }

    if (!s_hello_task_started) {
        s_hello_task_started = true;
        BaseType_t created = xTaskCreate(
            zigbee_ota_hello_task,
            "zb_ota_hello",
            3072,
            NULL,
            5,
            NULL);
        if (created != pdPASS) {
            s_hello_task_started = false;
            ESP_LOGE(TAG, "HELLO task creation failed");
            return ESP_FAIL;
        }
    }

    return ESP_OK;
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
