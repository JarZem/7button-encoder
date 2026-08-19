#include "zigbee_ota_cluster.h"

#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

#define TEST_HELLO_DELAY_MS 5000

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_test_hello_started;

static bool zigbee_ota_network_identity_valid(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static esp_err_t zigbee_ota_report_payload(const char *payload)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    const size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > ZIGBEE_OTA_ZCL_STRING_CAPACITY) return ESP_ERR_INVALID_SIZE;

    s_ota_payload_attr[0] = (uint8_t)payload_len;
    memcpy(&s_ota_payload_attr[1], payload, payload_len);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_manufacturer_attribute_val(
        ZIGBEE_OTA_ENDPOINT,
        ZIGBEE_OTA_CLUSTER_ID,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZIGBEE_OTA_MANUFACTURER_CODE,
        ZIGBEE_OTA_CONFIG_ATTR_ID,
        s_ota_payload_attr,
        false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        esp_zb_lock_release();
        ESP_LOGW(TAG, "TEST HELLO attr set failed zcl_status=0x%x", status);
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
        .manuf_specific = 1,
        .manuf_code = ZIGBEE_OTA_MANUFACTURER_CODE,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = ZIGBEE_OTA_CONFIG_ATTR_ID,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "TEST HELLO report cluster=0x%04x attr=0x%04x manuf=0x%04x payload=%s ret=%s(0x%x)",
             ZIGBEE_OTA_CLUSTER_ID,
             ZIGBEE_OTA_CONFIG_ATTR_ID,
             ZIGBEE_OTA_MANUFACTURER_CODE,
             payload,
             esp_err_to_name(err),
             err);
    return err;
}

static void test_hello_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(TEST_HELLO_DELAY_MS));

    if (!zigbee_ota_network_identity_valid()) {
        ESP_LOGW(TAG,
                 "TEST HELLO not sent: network identity invalid factory_new=%d short=0x%04x",
                 esp_zb_bdb_is_factory_new(),
                 esp_zb_get_short_address());
    } else {
        ESP_LOGI(TAG,
                 "TEST HELLO network identity valid channel=%u short=0x%04x; sending H|TEST",
                 esp_zb_get_current_channel(),
                 esp_zb_get_short_address());
        esp_err_t err = zigbee_ota_report_payload("H|TEST");
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "TEST HELLO send failed: %s", esp_err_to_name(err));
        }
    }

    s_test_hello_started = false;
    vTaskDelete(NULL);
}

void zigbee_ota_schedule_hello(uint32_t delay_ms)
{
    (void)delay_ms;
    if (s_test_hello_started) return;
    s_test_hello_started = true;
    if (xTaskCreate(test_hello_task, "zb_hello_test", 3072, NULL, 5, NULL) != pdPASS) {
        s_test_hello_started = false;
        ESP_LOGE(TAG, "TEST HELLO task creation failed");
    }
}

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster)
{
    s_ota_payload_attr[0] = 0;
    const uint8_t access = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE |
                           ESP_ZB_ZCL_ATTR_ACCESS_REPORTING |
                           ESP_ZB_ZCL_ATTR_MANUF_SPEC;

    esp_err_t err = esp_zb_cluster_add_manufacturer_attr(
        cluster,
        ZIGBEE_OTA_CLUSTER_ID,
        ZIGBEE_OTA_CONFIG_ATTR_ID,
        ZIGBEE_OTA_MANUFACTURER_CODE,
        ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        access,
        s_ota_payload_attr);
    if (err == ESP_OK) {
        ESP_LOGI(TAG,
                 "manufacturer OTA attribute registered cluster=0x%04x attr=0x%04x manuf=0x%04x; scheduling H|TEST in %u ms",
                 ZIGBEE_OTA_CLUSTER_ID,
                 ZIGBEE_OTA_CONFIG_ATTR_ID,
                 ZIGBEE_OTA_MANUFACTURER_CODE,
                 TEST_HELLO_DELAY_MS);
        zigbee_ota_schedule_hello(TEST_HELLO_DELAY_MS);
    }
    return err;
}

bool zigbee_ota_cluster_handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS ||
        message->info.dst_endpoint != ZIGBEE_OTA_ENDPOINT ||
        message->info.cluster != ZIGBEE_OTA_CLUSTER_ID ||
        message->attribute.id != ZIGBEE_OTA_CONFIG_ATTR_ID) return false;

    const esp_zb_zcl_attribute_data_t *data = &message->attribute.data;
    if (data->value == NULL) return true;
    const uint8_t *zcl_string = (const uint8_t *)data->value;
    size_t payload_len = 0;
    size_t value_offset = 0;
    if (data->type == ESP_ZB_ZCL_ATTR_TYPE_LONG_CHAR_STRING) {
        if (data->size < 2) return true;
        payload_len = (size_t)zcl_string[0] | ((size_t)zcl_string[1] << 8);
        value_offset = 2;
    } else if (data->type == ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING) {
        if (data->size < 1) return true;
        payload_len = zcl_string[0];
        value_offset = 1;
    } else return true;

    if (payload_len == 0 || payload_len > OTA_CONFIG_MAX_PAYLOAD_LEN ||
        payload_len > ZIGBEE_OTA_ZCL_STRING_CAPACITY || payload_len + value_offset > data->size) return true;

    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &zcl_string[value_offset], payload_len);
    payload[payload_len] = '\0';
    if (!ota_service_request_payload(payload, payload_len)) {
        ESP_LOGW(TAG, "OTA ERROR: request ignored: busy or queue full");
    }
    memset(payload, 0, sizeof(payload));
    return true;
}
