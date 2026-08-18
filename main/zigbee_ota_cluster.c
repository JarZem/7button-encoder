#include "zigbee_ota_cluster.h"

#include <stdio.h>
#include <string.h>

#include "device_credentials.h"
#include "device_identity.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "ota_config.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_common.h"

static const char *TAG = "zigbee_ota_cluster";

#define HELLO_PROTOCOL_VERSION 1
#define HELLO_FRAGMENT_DATA_LEN 44
#define HELLO_CANONICAL_MAX 512
#define HELLO_CERT_B64_MAX 1536
#define HELLO_PUBKEY_B64_MAX 192
#define HELLO_SIGNATURE_B64_MAX 128
#define HELLO_ECOSYSTEM "JaroslavZemanESP"
#define HELLO_DEVICE_MODEL "ESP32-C6-ENC"
#define HELLO_PRODUCT_ROLE "six-strip-cct-led-controller"
#define HELLO_HARDWARE_REVISION "ESP32-C6"
#define HELLO_CHIP_FAMILY "ESP32-C6"
#define HELLO_FLASH_SIZE "16MB"
#define HELLO_FIRMWARE_CHANNEL "stable"

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;

static bool zigbee_ota_is_joined(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static esp_err_t base64url_encode(const uint8_t *input, size_t input_len,
                                  char *out, size_t out_size)
{
    size_t written = 0;
    int ret = mbedtls_base64_encode((unsigned char *)out, out_size - 1, &written, input, input_len);
    if (ret != 0 || written >= out_size) return ESP_ERR_INVALID_SIZE;
    for (size_t i = 0; i < written; ++i) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
    while (written > 0 && out[written - 1] == '=') --written;
    out[written] = '\0';
    return ESP_OK;
}

static unsigned fragment_count(size_t len)
{
    return (unsigned)((len + HELLO_FRAGMENT_DATA_LEN - 1) / HELLO_FRAGMENT_DATA_LEN);
}

static esp_err_t zigbee_ota_report_payload(const char *payload)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    const size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > ZIGBEE_OTA_ZCL_STRING_CAPACITY) return ESP_ERR_INVALID_SIZE;

    s_ota_payload_attr[0] = (uint8_t)payload_len;
    memcpy(&s_ota_payload_attr[1], payload, payload_len);

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        ZIGBEE_OTA_ENDPOINT, ZIGBEE_OTA_CLUSTER_ID, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZIGBEE_OTA_CONFIG_ATTR_ID, s_ota_payload_attr, false);
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        esp_zb_lock_release();
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
    return err;
}

static esp_err_t send_fragment_series(const char *type, const char *tx,
                                      const char *data, unsigned total)
{
    const size_t len = strlen(data);
    for (unsigned index = 0; index < total; ++index) {
        const size_t offset = (size_t)index * HELLO_FRAGMENT_DATA_LEN;
        const size_t remain = len - offset;
        const size_t chunk_len = remain < HELLO_FRAGMENT_DATA_LEN ? remain : HELLO_FRAGMENT_DATA_LEN;
        char chunk[HELLO_FRAGMENT_DATA_LEN + 1];
        memcpy(chunk, data + offset, chunk_len);
        chunk[chunk_len] = '\0';

        char payload[ZIGBEE_OTA_HELLO_FRAME_MAX + 1];
        int n = snprintf(payload, sizeof(payload), "%s|%s|%u|%u|%s",
                         type, tx, index, total, chunk);
        if (n <= 0 || n > ZIGBEE_OTA_HELLO_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
        esp_err_t err = zigbee_ota_report_payload(payload);
        if (err != ESP_OK) return err;
        vTaskDelay(pdMS_TO_TICKS(80));
    }
    return ESP_OK;
}

static esp_err_t send_secure_hello(const char *device_id)
{
    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "device credentials unavailable");

    uint8_t public_key_der[DEVICE_CREDENTIAL_PUBLIC_KEY_MAX_DER];
    size_t public_key_der_len = 0;
    ESP_RETURN_ON_ERROR(device_credentials_get_public_key_der(public_key_der, sizeof(public_key_der),
                                                               &public_key_der_len),
                        TAG, "public key export failed");
    char public_key_b64[HELLO_PUBKEY_B64_MAX];
    ESP_RETURN_ON_ERROR(base64url_encode(public_key_der, public_key_der_len,
                                         public_key_b64, sizeof(public_key_b64)),
                        TAG, "public key encoding failed");

    const esp_app_desc_t *app = esp_app_get_description();
    char canonical[HELLO_CANONICAL_MAX];
    int canonical_len = snprintf(
        canonical, sizeof(canonical),
        "%u|%s|%s|%s|%s|%s|%s|%s|%s|%s|%u|%s",
        HELLO_PROTOCOL_VERSION,
        device_id,
        HELLO_ECOSYSTEM,
        HELLO_DEVICE_MODEL,
        HELLO_PRODUCT_ROLE,
        HELLO_HARDWARE_REVISION,
        HELLO_CHIP_FAMILY,
        HELLO_FLASH_SIZE,
        app != NULL ? app->version : "unknown",
        HELLO_FIRMWARE_CHANNEL,
        (unsigned)device_identity_get_key_id(),
        public_key_b64);
    if (canonical_len <= 0 || (size_t)canonical_len >= sizeof(canonical)) return ESP_ERR_INVALID_SIZE;

    const uint8_t *cert_der = NULL;
    size_t cert_der_len = 0;
    ESP_RETURN_ON_ERROR(device_credentials_get_certificate_der(&cert_der, &cert_der_len),
                        TAG, "certificate unavailable");
    char cert_b64[HELLO_CERT_B64_MAX];
    ESP_RETURN_ON_ERROR(base64url_encode(cert_der, cert_der_len, cert_b64, sizeof(cert_b64)),
                        TAG, "certificate encoding failed");

    uint8_t signature_der[DEVICE_CREDENTIAL_SIGNATURE_MAX_DER];
    size_t signature_der_len = 0;
    ESP_RETURN_ON_ERROR(device_credentials_sign((const uint8_t *)canonical, (size_t)canonical_len,
                                                 signature_der, sizeof(signature_der),
                                                 &signature_der_len),
                        TAG, "HELLO signing failed");
    char signature_b64[HELLO_SIGNATURE_B64_MAX];
    ESP_RETURN_ON_ERROR(base64url_encode(signature_der, signature_der_len,
                                         signature_b64, sizeof(signature_b64)),
                        TAG, "signature encoding failed");

    const unsigned data_parts = fragment_count((size_t)canonical_len);
    const unsigned cert_parts = fragment_count(strlen(cert_b64));
    const unsigned sig_parts = fragment_count(strlen(signature_b64));
    char tx[9];
    snprintf(tx, sizeof(tx), "%08lx", (unsigned long)esp_random());

    char start[ZIGBEE_OTA_HELLO_FRAME_MAX + 1];
    int start_len = snprintf(start, sizeof(start), "H0|%u|%s|%u|%u|%u",
                             HELLO_PROTOCOL_VERSION, tx, data_parts, cert_parts, sig_parts);
    if (start_len <= 0 || start_len > ZIGBEE_OTA_HELLO_FRAME_MAX) return ESP_ERR_INVALID_SIZE;

    ESP_LOGI(TAG,
             "HELLO secure start device_id=%s tx=%s canonical=%u cert_der=%u pub_der=%u signature_der=%u parts=%u/%u/%u",
             device_id, tx, (unsigned)canonical_len, (unsigned)cert_der_len,
             (unsigned)public_key_der_len, (unsigned)signature_der_len,
             data_parts, cert_parts, sig_parts);

    ESP_RETURN_ON_ERROR(zigbee_ota_report_payload(start), TAG, "HELLO start send failed");
    vTaskDelay(pdMS_TO_TICKS(80));
    ESP_RETURN_ON_ERROR(send_fragment_series("HD", tx, canonical, data_parts), TAG, "HELLO data send failed");
    ESP_RETURN_ON_ERROR(send_fragment_series("HC", tx, cert_b64, cert_parts), TAG, "HELLO cert send failed");
    ESP_RETURN_ON_ERROR(send_fragment_series("HS", tx, signature_b64, sig_parts), TAG, "HELLO signature send failed");

    ESP_LOGI(TAG, "HELLO secure complete device_id=%s tx=%s", device_id, tx);
    return ESP_OK;
}

static void zigbee_ota_hello_task(void *arg)
{
    (void)arg;
    for (unsigned attempt = 1; attempt <= 120; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (!zigbee_ota_is_joined()) continue;
        vTaskDelay(pdMS_TO_TICKS(1500));
        if (!zigbee_ota_is_joined()) continue;

        char device_id[DEVICE_ID_MAX_LEN] = {0};
        esp_err_t id_err = device_identity_get_device_id(device_id);
        if (id_err != ESP_OK || device_id[0] == '\0' ||
            strcmp(device_id, "00:00:00:00:00:00:00:00") == 0) continue;

        esp_err_t err = send_secure_hello(device_id);
        if (err == ESP_OK) break;
        ESP_LOGW(TAG, "HELLO secure send failed attempt=%u: %s", attempt, esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    s_hello_task_started = false;
    vTaskDelete(NULL);
}

esp_err_t zigbee_ota_cluster_add_attrs(esp_zb_attribute_list_t *cluster)
{
    s_ota_payload_attr[0] = 0;
    esp_err_t err = esp_zb_custom_cluster_add_custom_attr(
        cluster, ZIGBEE_OTA_CONFIG_ATTR_ID, ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        s_ota_payload_attr);
    if (err != ESP_OK) return err;

    if (!s_hello_task_started) {
        s_hello_task_started = true;
        if (xTaskCreate(zigbee_ota_hello_task, "zb_ota_hello", 6144, NULL, 5, NULL) != pdPASS) {
            s_hello_task_started = false;
            return ESP_FAIL;
        }
    }
    return ESP_OK;
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
