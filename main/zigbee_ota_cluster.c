#include "zigbee_ota_cluster.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_credentials.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

#define HELLO_STARTUP_DELAY_MS 4500
#define HELLO_WAIT_ATTEMPTS 30
#define HELLO_SIGNATURE_B64_MAX 96
#define DIAG_PING "D|PING"
#define DIAG_PONG "D|PONG"
#define DIAG_LEN_PREFIX "D|LEN|"
#define DIAG_LEN_MIN 6
#define DIAG_LEN_MAX 100

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;
static bool s_hello_sent_this_boot;
static bool s_diag_task_started;
static uint32_t s_hello_delay_ms;
static size_t s_diag_payload_len;

static bool zigbee_ota_network_identity_valid(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static esp_err_t base64url_encode(const uint8_t *input, size_t input_len,
                                  char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size < 2) return ESP_ERR_INVALID_ARG;

    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_size, &written, input, input_len);
    if (ret != 0 || written >= out_size) {
        ESP_LOGE(TAG,
                 "base64 encode failed ret=-0x%04x input_len=%u out_size=%u written=%u",
                 (unsigned)(ret < 0 ? -ret : ret),
                 (unsigned)input_len,
                 (unsigned)out_size,
                 (unsigned)written);
        return ESP_ERR_INVALID_SIZE;
    }

    for (size_t i = 0; i < written; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }

    while (written > 0 && out[written - 1] == '=') --written;
    out[written] = '\0';
    return ESP_OK;
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
        ESP_LOGW(TAG,
                 "OTA attr set failed cluster=0x%04x attr=0x%04x manuf=0x%04x zcl_status=0x%x",
                 ZIGBEE_OTA_CLUSTER_ID,
                 ZIGBEE_OTA_CONFIG_ATTR_ID,
                 ZIGBEE_OTA_MANUFACTURER_CODE,
                 status);
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
             "OTA custom report cluster=0x%04x attr=0x%04x manuf=0x%04x bytes=%u ret=%s(0x%x) payload=%s",
             ZIGBEE_OTA_CLUSTER_ID,
             ZIGBEE_OTA_CONFIG_ATTR_ID,
             ZIGBEE_OTA_MANUFACTURER_CODE,
             (unsigned)payload_len,
             esp_err_to_name(err),
             err,
             payload);
    return err;
}

static void zigbee_ota_make_diag_payload(char *payload, size_t payload_size, size_t payload_len)
{
    if (payload == NULL || payload_size == 0) return;
    if (payload_len >= payload_size) payload_len = payload_size - 1;

    int prefix_len = snprintf(payload, payload_size, "D|L%03u|", (unsigned)payload_len);
    if (prefix_len < 0) {
        payload[0] = '\0';
        return;
    }
    size_t used = (size_t)prefix_len;
    if (used > payload_len) used = payload_len;
    for (size_t i = used; i < payload_len; ++i) payload[i] = (char)('A' + (i % 26));
    payload[payload_len] = '\0';
}

static void zigbee_ota_diag_task(void *arg)
{
    (void)arg;
    const size_t payload_len = s_diag_payload_len;
    vTaskDelay(pdMS_TO_TICKS(100));

    if (!zigbee_ota_network_identity_valid()) {
        ESP_LOGW(TAG, "DIAG ignored: Zigbee network identity is not usable");
        s_diag_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    char payload[DIAG_LEN_MAX + 1];
    if (payload_len == 0) {
        strcpy(payload, DIAG_PONG);
    } else {
        zigbee_ota_make_diag_payload(payload, sizeof(payload), payload_len);
    }

    ESP_LOGI(TAG, "DIAG custom payload test bytes=%u", (unsigned)strlen(payload));
    esp_err_t custom_err = zigbee_ota_report_payload(payload);
    ESP_LOGI(TAG, "DIAG result bytes=%u custom=%s", (unsigned)strlen(payload), esp_err_to_name(custom_err));

    s_diag_task_started = false;
    vTaskDelete(NULL);
}

static void zigbee_ota_schedule_diag(size_t payload_len)
{
    if (s_diag_task_started) {
        ESP_LOGW(TAG, "DIAG ignored: diagnostic task already active");
        return;
    }
    s_diag_payload_len = payload_len;
    s_diag_task_started = true;
    if (xTaskCreate(zigbee_ota_diag_task, "zb_ota_diag", 3072, NULL, 5, NULL) != pdPASS) {
        s_diag_task_started = false;
        ESP_LOGE(TAG, "DIAG task creation failed");
    }
}

static esp_err_t send_secure_hello(void)
{
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "device credentials unavailable");

    char device_id[DEVICE_ID_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(device_identity_get_device_id(device_id), TAG, "device identity unavailable");
    if (device_id[0] == '\0' || strcmp(device_id, "00:00:00:00:00:00:00:00") == 0) {
        return ESP_ERR_INVALID_STATE;
    }

    uint64_t counter = 0;
    ESP_RETURN_ON_ERROR(device_identity_next_enrollment_counter(&counter), TAG, "HELLO counter update failed");

    char canonical[96];
    int canonical_len = snprintf(canonical, sizeof(canonical), "H|%s|%" PRIu64, device_id, counter);
    if (canonical_len <= 0 || (size_t)canonical_len >= sizeof(canonical)) return ESP_ERR_INVALID_SIZE;

    uint8_t signature_raw[DEVICE_CREDENTIAL_SIGNATURE_RAW_LEN];
    ESP_RETURN_ON_ERROR(
        device_credentials_sign_raw64((const uint8_t *)canonical, (size_t)canonical_len, signature_raw),
        TAG,
        "HELLO signing failed");

    char signature_b64[HELLO_SIGNATURE_B64_MAX];
    ESP_RETURN_ON_ERROR(
        base64url_encode(signature_raw, sizeof(signature_raw), signature_b64, sizeof(signature_b64)),
        TAG,
        "HELLO signature encoding failed");

    char payload[ZIGBEE_OTA_HELLO_FRAME_MAX + 1];
    int payload_len = snprintf(payload, sizeof(payload), "H|%" PRIu64 "|%s", counter, signature_b64);
    if (payload_len <= 0 || payload_len > ZIGBEE_OTA_HELLO_FRAME_MAX) return ESP_ERR_INVALID_SIZE;

    ESP_LOGI(TAG,
             "HELLO signed single-frame device_id=%s counter=%" PRIu64 " signed_bytes=%u signature_b64_len=%u frame_bytes=%u",
             device_id,
             counter,
             (unsigned)canonical_len,
             (unsigned)strlen(signature_b64),
             (unsigned)payload_len);

    esp_err_t err = zigbee_ota_report_payload(payload);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "HELLO send submitted device_id=%s counter=%" PRIu64, device_id, counter);
    }
    return err;
}

static void zigbee_ota_hello_task(void *arg)
{
    (void)arg;
    const uint32_t delay_ms = s_hello_delay_ms;
    if (delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(delay_ms));

    for (unsigned attempt = 1; attempt <= HELLO_WAIT_ATTEMPTS; ++attempt) {
        if (!zigbee_ota_network_identity_valid()) {
            ESP_LOGI(TAG,
                     "HELLO waiting for usable network attempt=%u/%u factory_new=%d short=0x%04x",
                     attempt,
                     HELLO_WAIT_ATTEMPTS,
                     esp_zb_bdb_is_factory_new(),
                     esp_zb_get_short_address());
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        ESP_LOGI(TAG, "HELLO network ready channel=%u short=0x%04x",
                 esp_zb_get_current_channel(), esp_zb_get_short_address());
        esp_err_t err = send_secure_hello();
        if (err == ESP_OK) s_hello_sent_this_boot = true;
        else ESP_LOGW(TAG, "HELLO send failed: %s", esp_err_to_name(err));
        break;
    }

    if (!s_hello_sent_this_boot) ESP_LOGW(TAG, "HELLO not submitted during startup window");
    s_hello_task_started = false;
    vTaskDelete(NULL);
}

void zigbee_ota_schedule_hello(uint32_t delay_ms)
{
    if (s_hello_sent_this_boot) {
        ESP_LOGI(TAG, "HELLO schedule ignored: already submitted this boot");
        return;
    }
    if (s_hello_task_started) {
        ESP_LOGI(TAG, "HELLO schedule ignored: task already active");
        return;
    }

    s_hello_delay_ms = delay_ms;
    s_hello_task_started = true;
    if (xTaskCreate(zigbee_ota_hello_task, "zb_ota_hello", 4096, NULL, 5, NULL) != pdPASS) {
        s_hello_task_started = false;
        ESP_LOGE(TAG, "HELLO task creation failed");
        return;
    }
    ESP_LOGI(TAG, "HELLO scheduled delay_ms=%lu", (unsigned long)delay_ms);
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
        ESP_LOGW(TAG,
                 "manufacturer OTA attribute registered cluster=0x%04x attr=0x%04x manuf=0x%04x; automatic HELLO disabled for Zigbee stabilization",
                 ZIGBEE_OTA_CLUSTER_ID,
                 ZIGBEE_OTA_CONFIG_ATTR_ID,
                 ZIGBEE_OTA_MANUFACTURER_CODE);
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

    if (strcmp(payload, DIAG_PING) == 0) {
        ESP_LOGI(TAG, "DIAG command received payload=%s", payload);
        zigbee_ota_schedule_diag(0);
        memset(payload, 0, sizeof(payload));
        return true;
    }

    if (strncmp(payload, DIAG_LEN_PREFIX, strlen(DIAG_LEN_PREFIX)) == 0) {
        char *end = NULL;
        unsigned long requested = strtoul(payload + strlen(DIAG_LEN_PREFIX), &end, 10);
        if (end == payload + strlen(DIAG_LEN_PREFIX) || *end != '\0' ||
            requested < DIAG_LEN_MIN || requested > DIAG_LEN_MAX) {
            ESP_LOGW(TAG, "DIAG LEN invalid payload=%s allowed=%u..%u",
                     payload, DIAG_LEN_MIN, DIAG_LEN_MAX);
        } else {
            ESP_LOGI(TAG, "DIAG LEN command received requested_bytes=%lu", requested);
            zigbee_ota_schedule_diag((size_t)requested);
        }
        memset(payload, 0, sizeof(payload));
        return true;
    }

    if (!ota_service_request_payload(payload, payload_len)) {
        ESP_LOGW(TAG, "OTA ERROR: request ignored: busy or queue full");
    }
    memset(payload, 0, sizeof(payload));
    return true;
}
