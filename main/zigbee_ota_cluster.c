#include "zigbee_ota_cluster.h"

#include <string.h>

#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

#define TEST_HELLO_DELAY_MS 5000
#define TEST_HELLO_COMMAND_ID 0x01
#define ZIGBEE_OTA_PROFILE_ID 0x0104

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_test_hello_started;

static bool zigbee_ota_network_identity_valid(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static void zigbee_ota_send_test_custom_command(const char *payload)
{
    if (payload == NULL || payload[0] == '\0') return;

    const size_t payload_len = strlen(payload);
    esp_zb_zcl_custom_cluster_cmd_req_t cmd_req = {0};
    cmd_req.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd_req.zcl_basic_cmd.dst_endpoint = 1;
    cmd_req.zcl_basic_cmd.src_endpoint = ZIGBEE_OTA_ENDPOINT;
    cmd_req.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd_req.cluster_id = ZIGBEE_OTA_CLUSTER_ID;
    cmd_req.profile_id = ZIGBEE_OTA_PROFILE_ID;
    cmd_req.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd_req.custom_cmd_id = TEST_HELLO_COMMAND_ID;
    cmd_req.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    cmd_req.data.size = payload_len;
    cmd_req.data.value = (void *)payload;

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_custom_cluster_cmd_req(&cmd_req);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "TEST HELLO custom command submitted dst=0x0000/1 src_ep=%u cluster=0x%04x cmd=0x%02x bytes=%u payload=%s",
             ZIGBEE_OTA_ENDPOINT,
             ZIGBEE_OTA_CLUSTER_ID,
             TEST_HELLO_COMMAND_ID,
             (unsigned)payload_len,
             payload);
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
                 "TEST HELLO network identity valid channel=%u short=0x%04x; sending custom command H|TEST",
                 esp_zb_get_current_channel(),
                 esp_zb_get_short_address());
        zigbee_ota_send_test_custom_command("H|TEST");
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
                 "manufacturer OTA attribute registered cluster=0x%04x attr=0x%04x manuf=0x%04x; custom-command H|TEST scheduled in %u ms",
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
