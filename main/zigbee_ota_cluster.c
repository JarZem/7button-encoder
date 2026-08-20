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

#define HELLO_WAIT_ATTEMPTS 30
#define HELLO_SIGNATURE_B64_MAX 96
#define HELLO_START_DELAY_MS 5000
#define DIAG_PING "D|PING"
#define DIAG_PONG "D|PONG"
#define DIAG_LEN_PREFIX "D|LEN|"
#define DIAG_STOP "D|STOP"
#define DIAG_LEN_MIN 6
#define DIAG_LEN_MAX 100
#define DIAG_REPEAT_MS 20000

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;
static bool s_hello_sent_this_boot;
static bool s_diag_task_started;
static bool s_len_test_task_started;
static uint32_t s_hello_delay_ms;
static volatile size_t s_len_test_payload_len;

static bool zigbee_ota_network_identity_valid(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static esp_err_t base64url_encode(const uint8_t *input, size_t input_len, char *out, size_t out_size)
{
    if (input == NULL || out == NULL || out_size < 2) return ESP_ERR_INVALID_ARG;
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_size, &written, input, input_len);
    if (ret != 0 || written >= out_size) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < written; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    while (written > 0 && out[written - 1] == '=') --written;
    out[written] = '\0';
    return ESP_OK;
}

static esp_err_t zigbee_ota_send_command_payload(const char *payload)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    const size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > ZIGBEE_OTA_COMMAND_PAYLOAD_MAX || payload_len > 254) {
        ESP_LOGE(TAG, "OTA command blocked: bytes=%u max=%u", (unsigned)payload_len, ZIGBEE_OTA_COMMAND_PAYLOAD_MAX);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t wire[ZIGBEE_OTA_COMMAND_PAYLOAD_MAX + 1];
    wire[0] = (uint8_t)payload_len;
    memcpy(&wire[1], payload, payload_len);

    esp_zb_zcl_custom_cluster_cmd_req_t cmd = {0};
    cmd.zcl_basic_cmd.dst_addr_u.addr_short = 0x0000;
    cmd.zcl_basic_cmd.dst_endpoint = 1;
    cmd.zcl_basic_cmd.src_endpoint = ZIGBEE_OTA_ENDPOINT;
    cmd.address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    cmd.cluster_id = ZIGBEE_OTA_CLUSTER_ID;
    cmd.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    cmd.direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI;
    cmd.custom_cmd_id = ZIGBEE_OTA_CMD_FROM_DEVICE_ID;
    cmd.data.type = ESP_ZB_ZCL_ATTR_TYPE_SET;
    cmd.data.size = payload_len + 1;
    cmd.data.value = wire;

    esp_zb_lock_acquire(portMAX_DELAY);
    const uint8_t tsn = esp_zb_zcl_custom_cluster_cmd_req(&cmd);
    esp_zb_lock_release();

    ESP_LOGI(TAG,
             "OTA custom command tx cluster=0x%04x cmd=0x%02x bytes=%u tsn=0x%02x payload=%s",
             ZIGBEE_OTA_CLUSTER_ID, ZIGBEE_OTA_CMD_FROM_DEVICE_ID,
             (unsigned)payload_len, tsn, payload);
    return ESP_OK;
}

static void zigbee_ota_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t err = zigbee_ota_send_command_payload(DIAG_PONG);
    ESP_LOGI(TAG, "DIAG PING result custom_cmd=%s", esp_err_to_name(err));
    s_diag_task_started = false;
    vTaskDelete(NULL);
}

static void zigbee_ota_len_test_task(void *arg)
{
    (void)arg;
    unsigned iteration = 0;
    while (s_len_test_payload_len != 0) {
        const size_t len = s_len_test_payload_len;
        char test[DIAG_LEN_MAX + 1];
        int prefix = snprintf(test, sizeof(test), "D|L%03u|", (unsigned)len);
        size_t used = prefix > 0 ? (size_t)prefix : 0;
        if (used > len) used = len;
        for (size_t i = used; i < len; ++i) test[i] = (char)('A' + (i % 26));
        test[len] = '\0';
        ++iteration;
        ESP_LOGI(TAG, "DIAG LEN iteration=%u bytes=%u transport=custom_cmd", iteration, (unsigned)len);
        esp_err_t err = zigbee_ota_send_command_payload(test);
        ESP_LOGI(TAG, "DIAG LEN result iteration=%u bytes=%u custom_cmd=%s",
                 iteration, (unsigned)len, esp_err_to_name(err));
        for (unsigned elapsed = 0; elapsed < DIAG_REPEAT_MS && s_len_test_payload_len != 0; elapsed += 100)
            vTaskDelay(pdMS_TO_TICKS(100));
    }
    s_len_test_task_started = false;
    vTaskDelete(NULL);
}

static bool zigbee_ota_process_payload(const char *payload, size_t payload_len)
{
    if (payload == NULL || payload_len == 0) return true;

    if (strcmp(payload, DIAG_PING) == 0) {
        if (!s_diag_task_started) {
            s_diag_task_started = true;
            if (xTaskCreate(zigbee_ota_diag_task, "zb_ota_diag", 3072, NULL, 5, NULL) != pdPASS)
                s_diag_task_started = false;
        }
        return true;
    }

    if (strcmp(payload, DIAG_STOP) == 0) {
        s_len_test_payload_len = 0;
        return true;
    }

    if (strncmp(payload, DIAG_LEN_PREFIX, strlen(DIAG_LEN_PREFIX)) == 0) {
        char *end = NULL;
        unsigned long requested = strtoul(payload + strlen(DIAG_LEN_PREFIX), &end, 10);
        if (end == payload + strlen(DIAG_LEN_PREFIX) || *end != '\0' || requested < DIAG_LEN_MIN || requested > DIAG_LEN_MAX) {
            ESP_LOGE(TAG, "DIAG LEN rejected payload=%s allowed=%u..%u", payload, DIAG_LEN_MIN, DIAG_LEN_MAX);
            return true;
        }
        s_len_test_payload_len = (size_t)requested;
        if (!s_len_test_task_started) {
            s_len_test_task_started = true;
            if (xTaskCreate(zigbee_ota_len_test_task, "zb_ota_len", 3072, NULL, 5, NULL) != pdPASS) {
                s_len_test_task_started = false;
                s_len_test_payload_len = 0;
            }
        }
        ESP_LOGW(TAG, "DIAG LEN custom-command test bytes=%lu interval_ms=%u", requested, DIAG_REPEAT_MS);
        return true;
    }

    if (!ota_service_request_payload(payload, payload_len))
        ESP_LOGW(TAG, "OTA ERROR: request ignored: busy or queue full");
    return true;
}

static esp_err_t send_secure_hello(void)
{
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "device credentials unavailable");
    char device_id[DEVICE_ID_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(device_identity_get_device_id(device_id), TAG, "device identity unavailable");
    if (device_id[0] == '\0' || strcmp(device_id, "00:00:00:00:00:00:00:00") == 0) return ESP_ERR_INVALID_STATE;

    uint64_t counter = 0;
    ESP_RETURN_ON_ERROR(device_identity_next_enrollment_counter(&counter), TAG, "HELLO counter update failed");
    char canonical[96];
    int canonical_len = snprintf(canonical, sizeof(canonical), "H|%s|%" PRIu64, device_id, counter);
    if (canonical_len <= 0 || (size_t)canonical_len >= sizeof(canonical)) return ESP_ERR_INVALID_SIZE;

    uint8_t signature_raw[DEVICE_CREDENTIAL_SIGNATURE_RAW_LEN];
    ESP_RETURN_ON_ERROR(device_credentials_sign_raw64((const uint8_t *)canonical, (size_t)canonical_len, signature_raw), TAG, "HELLO signing failed");
    char signature_b64[HELLO_SIGNATURE_B64_MAX];
    ESP_RETURN_ON_ERROR(base64url_encode(signature_raw, sizeof(signature_raw), signature_b64, sizeof(signature_b64)), TAG, "HELLO signature encoding failed");

    char payload[ZIGBEE_OTA_HELLO_FRAME_MAX + 1];
    int payload_len = snprintf(payload, sizeof(payload), "H|%" PRIu64 "|%s", counter, signature_b64);
    if (payload_len <= 0 || payload_len > ZIGBEE_OTA_HELLO_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG, "HELLO sending signed frame counter=%" PRIu64 " bytes=%d transport=custom_zcl one-shot", counter, payload_len);
    return zigbee_ota_send_command_payload(payload);
}

static void zigbee_ota_hello_task(void *arg)
{
    (void)arg;
    if (s_hello_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(s_hello_delay_ms));
    for (unsigned attempt = 1; attempt <= HELLO_WAIT_ATTEMPTS; ++attempt) {
        if (!zigbee_ota_network_identity_valid()) {
            ESP_LOGI(TAG, "HELLO waiting for joined Zigbee network attempt=%u/%u", attempt, HELLO_WAIT_ATTEMPTS);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        esp_err_t err = send_secure_hello();
        if (err == ESP_OK) s_hello_sent_this_boot = true;
        ESP_LOGI(TAG, "HELLO one-shot custom ZCL request result=%s; no automatic retry", esp_err_to_name(err));
        break;
    }
    if (!s_hello_sent_this_boot) {
        ESP_LOGE(TAG, "HELLO not sent: Zigbee network did not become usable within wait window");
    }
    s_hello_task_started = false;
    vTaskDelete(NULL);
}

void zigbee_ota_schedule_hello(uint32_t delay_ms)
{
    if (s_hello_sent_this_boot || s_hello_task_started) return;
    s_hello_delay_ms = delay_ms;
    s_hello_task_started = true;
    ESP_LOGI(TAG, "HELLO scheduled delay_ms=%lu transport=custom_zcl one-shot", (unsigned long)delay_ms);
    if (xTaskCreate(zigbee_ota_hello_task, "zb_ota_hello", 4096, NULL, 5, NULL) != pdPASS) {
        s_hello_task_started = false;
        ESP_LOGE(TAG, "HELLO task create failed");
    }
}

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster)
{
    s_ota_payload_attr[0] = 0;
    const uint8_t access = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE |
                           ESP_ZB_ZCL_ATTR_ACCESS_REPORTING |
                           ESP_ZB_ZCL_ATTR_MANUF_SPEC;
    esp_err_t err = esp_zb_cluster_add_manufacturer_attr(
        cluster, ZIGBEE_OTA_CLUSTER_ID, ZIGBEE_OTA_CONFIG_ATTR_ID,
        ZIGBEE_OTA_MANUFACTURER_CODE, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        access, s_ota_payload_attr);
    if (err == ESP_OK) {
        ESP_LOGW(TAG, "OTA cluster registered endpoint=%u cluster=0x%04x transport=custom ZCL command; HELLO one-shot; legacy attr retained",
                 ZIGBEE_OTA_ENDPOINT, ZIGBEE_OTA_CLUSTER_ID);
        zigbee_ota_schedule_hello(HELLO_START_DELAY_MS);
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
    if (data->value == NULL || data->type != ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING || data->size < 1) return true;
    const uint8_t *zcl = (const uint8_t *)data->value;
    size_t len = zcl[0];
    if (len == 0 || len > OTA_CONFIG_MAX_PAYLOAD_LEN || len + 1 > data->size) return true;
    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &zcl[1], len);
    payload[len] = '\0';
    return zigbee_ota_process_payload(payload, len);
}

bool zigbee_ota_cluster_handle_custom_cmd(const esp_zb_zcl_custom_cluster_command_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS ||
        message->info.dst_endpoint != ZIGBEE_OTA_ENDPOINT ||
        message->info.cluster != ZIGBEE_OTA_CLUSTER_ID ||
        message->info.command.id != ZIGBEE_OTA_CMD_TO_DEVICE_ID) return false;

    if (message->data.value == NULL || message->data.size < 2) return true;
    const uint8_t *wire = (const uint8_t *)message->data.value;
    const size_t len = wire[0];
    if (len == 0 || len > OTA_CONFIG_MAX_PAYLOAD_LEN || len + 1 > message->data.size) {
        ESP_LOGW(TAG, "OTA custom command rx invalid wire_size=%u declared=%u", message->data.size, wire[0]);
        return true;
    }

    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &wire[1], len);
    payload[len] = '\0';
    ESP_LOGI(TAG, "OTA custom command rx cmd=0x%02x bytes=%u payload=%s",
             message->info.command.id, (unsigned)len, payload);
    return zigbee_ota_process_payload(payload, len);
}
