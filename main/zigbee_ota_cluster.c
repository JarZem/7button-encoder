#include "zigbee_ota_cluster.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_credentials.h"
#include "device_identity.h"
#include "device_enrollment.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "nvs.h"
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
#define OTA_MESSAGE_ID_HEX_LEN 16
#define OTA_CHALLENGE_B64URL_LEN 43
#define OTA_CHALLENGE_RESPONSE_DOMAIN "JaroslavZemanESP-DEVICE-CHALLENGE-RESPONSE-v1"
#define OTA_SEC_NAMESPACE "ota_sec"
#define OTA_NVS_CHALLENGE_MESSAGE_ID "auth_mid"
#define OTA_NVS_CHALLENGE_VALUE "auth_chal"
#define OTA_NVS_CHALLENGE_RESPONSE "auth_resp"
#define OTA_NVS_CHALLENGE_PENDING "auth_pending"
#define OTA_CHALLENGE_RESPONSE_BINARY_LEN (8 + DEVICE_CREDENTIAL_SIGNATURE_RAW_LEN)
#define OTA_CHALLENGE_RESPONSE_B64_MAX 100
#define OTA_CHALLENGE_RESPONSE_FRAME_MAX 104

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;
static bool s_hello_sent_this_boot;
static bool s_diag_task_started;
static bool s_len_test_task_started;
static uint32_t s_hello_delay_ms;
static volatile size_t s_len_test_payload_len;
static char s_pending_message_id[OTA_MESSAGE_ID_HEX_LEN + 1];
static uint8_t s_pending_challenge[DEVICE_AUTH_CHALLENGE_LEN];
static char s_pending_challenge_response[OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1];
static bool s_pending_challenge_valid;

static bool zigbee_ota_network_identity_valid(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static bool is_hex_string(const char *value, size_t len)
{
    if (value == NULL) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = value[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static esp_err_t hex_decode_fixed(const char *hex, size_t hex_len, uint8_t *out, size_t out_len)
{
    if (hex == NULL || out == NULL || hex_len != out_len * 2) return ESP_ERR_INVALID_ARG;
    for (size_t i = 0; i < out_len; ++i) {
        const int hi = hex_nibble(hex[i * 2]);
        const int lo = hex_nibble(hex[i * 2 + 1]);
        if (hi < 0 || lo < 0) return ESP_ERR_INVALID_ARG;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return ESP_OK;
}

static bool bytes_all_zero(const uint8_t *data, size_t len)
{
    uint8_t any = 0;
    for (size_t i = 0; i < len; ++i) any |= data[i];
    return any == 0;
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

static esp_err_t base64url_decode_challenge(const char *input,
                                            uint8_t out[DEVICE_AUTH_CHALLENGE_LEN])
{
    if (input == NULL || out == NULL || strlen(input) != OTA_CHALLENGE_B64URL_LEN) {
        return ESP_ERR_INVALID_ARG;
    }

    char padded[48];
    const size_t input_len = strlen(input);
    memcpy(padded, input, input_len);
    size_t padded_len = input_len;
    for (size_t i = 0; i < input_len; ++i) {
        if (padded[i] == '-') padded[i] = '+';
        else if (padded[i] == '_') padded[i] = '/';
    }
    while ((padded_len % 4) != 0) padded[padded_len++] = '=';
    padded[padded_len] = '\0';

    size_t written = 0;
    const int ret = mbedtls_base64_decode(out,
                                          DEVICE_AUTH_CHALLENGE_LEN,
                                          &written,
                                          (const unsigned char *)padded,
                                          padded_len);
    memset(padded, 0, sizeof(padded));
    if (ret != 0 || written != DEVICE_AUTH_CHALLENGE_LEN) {
        memset(out, 0, DEVICE_AUTH_CHALLENGE_LEN);
        return ESP_ERR_INVALID_SIZE;
    }
    return ESP_OK;
}

static esp_err_t challenge_state_load(char message_id[OTA_MESSAGE_ID_HEX_LEN + 1],
                                      uint8_t challenge[DEVICE_AUTH_CHALLENGE_LEN],
                                      char response[OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1],
                                      bool *pending)
{
    if (message_id == NULL || challenge == NULL || response == NULL || pending == NULL) return ESP_ERR_INVALID_ARG;
    message_id[0] = '\0';
    response[0] = '\0';
    memset(challenge, 0, DEVICE_AUTH_CHALLENGE_LEN);
    *pending = false;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(OTA_SEC_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (err != ESP_OK) return err;

    uint8_t pending_u8 = 0;
    size_t mid_len = OTA_MESSAGE_ID_HEX_LEN + 1;
    size_t challenge_len = DEVICE_AUTH_CHALLENGE_LEN;
    size_t response_len = OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1;
    esp_err_t mid_err = nvs_get_str(handle, OTA_NVS_CHALLENGE_MESSAGE_ID, message_id, &mid_len);
    esp_err_t challenge_err = nvs_get_blob(handle, OTA_NVS_CHALLENGE_VALUE, challenge, &challenge_len);
    esp_err_t response_err = nvs_get_str(handle, OTA_NVS_CHALLENGE_RESPONSE, response, &response_len);
    esp_err_t pending_err = nvs_get_u8(handle, OTA_NVS_CHALLENGE_PENDING, &pending_u8);
    nvs_close(handle);

    if (mid_err == ESP_ERR_NVS_NOT_FOUND && challenge_err == ESP_ERR_NVS_NOT_FOUND &&
        response_err == ESP_ERR_NVS_NOT_FOUND && pending_err == ESP_ERR_NVS_NOT_FOUND) return ESP_OK;
    if (mid_err != ESP_OK || challenge_err != ESP_OK || response_err != ESP_OK || pending_err != ESP_OK ||
        mid_len != OTA_MESSAGE_ID_HEX_LEN + 1 || challenge_len != DEVICE_AUTH_CHALLENGE_LEN ||
        response_len < 2 || !is_hex_string(message_id, OTA_MESSAGE_ID_HEX_LEN)) {
        memset(message_id, 0, OTA_MESSAGE_ID_HEX_LEN + 1);
        memset(challenge, 0, DEVICE_AUTH_CHALLENGE_LEN);
        memset(response, 0, OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1);
        return ESP_ERR_INVALID_STATE;
    }
    *pending = pending_u8 != 0;
    return ESP_OK;
}

static esp_err_t challenge_state_save(const char *message_id,
                                      const uint8_t challenge[DEVICE_AUTH_CHALLENGE_LEN],
                                      const char *response)
{
    if (message_id == NULL || challenge == NULL || response == NULL ||
        strlen(message_id) != OTA_MESSAGE_ID_HEX_LEN || !is_hex_string(message_id, OTA_MESSAGE_ID_HEX_LEN) ||
        strlen(response) == 0 || strlen(response) > OTA_CHALLENGE_RESPONSE_FRAME_MAX) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    ESP_RETURN_ON_ERROR(nvs_open(OTA_SEC_NAMESPACE, NVS_READWRITE, &handle), TAG, "challenge NVS open failed");
    esp_err_t err = nvs_set_str(handle, OTA_NVS_CHALLENGE_MESSAGE_ID, message_id);
    if (err == ESP_OK) err = nvs_set_blob(handle, OTA_NVS_CHALLENGE_VALUE, challenge, DEVICE_AUTH_CHALLENGE_LEN);
    if (err == ESP_OK) err = nvs_set_str(handle, OTA_NVS_CHALLENGE_RESPONSE, response);
    if (err == ESP_OK) err = nvs_set_u8(handle, OTA_NVS_CHALLENGE_PENDING, 1);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

static esp_err_t build_challenge_response(const char *message_id,
                                          const uint8_t challenge[DEVICE_AUTH_CHALLENGE_LEN],
                                          char out[OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1])
{
    if (message_id == NULL || challenge == NULL || out == NULL ||
        strlen(message_id) != OTA_MESSAGE_ID_HEX_LEN || !is_hex_string(message_id, OTA_MESSAGE_ID_HEX_LEN)) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(device_credentials_init(), TAG, "device credentials unavailable for challenge response");
    char device_id[DEVICE_ID_MAX_LEN] = {0};
    ESP_RETURN_ON_ERROR(device_identity_get_device_id(device_id), TAG, "device identity unavailable for challenge response");

    uint8_t canonical[160];
    const int prefix_len = snprintf((char *)canonical, sizeof(canonical), "%s|%s|%s|",
                                    OTA_CHALLENGE_RESPONSE_DOMAIN, device_id, message_id);
    if (prefix_len <= 0 || (size_t)prefix_len + DEVICE_AUTH_CHALLENGE_LEN > sizeof(canonical)) return ESP_ERR_INVALID_SIZE;
    memcpy(canonical + prefix_len, challenge, DEVICE_AUTH_CHALLENGE_LEN);
    const size_t canonical_len = (size_t)prefix_len + DEVICE_AUTH_CHALLENGE_LEN;

    uint8_t signature[DEVICE_CREDENTIAL_SIGNATURE_RAW_LEN];
    esp_err_t err = device_credentials_sign_raw64(canonical, canonical_len, signature);
    memset(canonical, 0, sizeof(canonical));
    if (err != ESP_OK) return err;

    uint8_t message_id_raw[8];
    err = hex_decode_fixed(message_id, OTA_MESSAGE_ID_HEX_LEN, message_id_raw, sizeof(message_id_raw));
    if (err != ESP_OK) {
        memset(signature, 0, sizeof(signature));
        return err;
    }

    uint8_t packed[OTA_CHALLENGE_RESPONSE_BINARY_LEN];
    memcpy(packed, message_id_raw, sizeof(message_id_raw));
    memcpy(packed + sizeof(message_id_raw), signature, sizeof(signature));
    memset(signature, 0, sizeof(signature));

    char encoded[OTA_CHALLENGE_RESPONSE_B64_MAX];
    err = base64url_encode(packed, sizeof(packed), encoded, sizeof(encoded));
    memset(packed, 0, sizeof(packed));
    if (err != ESP_OK) return err;

    const int frame_len = snprintf(out, OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1, "E|%s", encoded);
    memset(encoded, 0, sizeof(encoded));
    if (frame_len <= 0 || frame_len > OTA_CHALLENGE_RESPONSE_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
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

static void zigbee_ota_challenge_response_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(50));

    char ack[32];
    snprintf(ack, sizeof(ack), "R|%s|OK", s_pending_message_id);
    esp_err_t err = zigbee_ota_send_command_payload(ack);
    ESP_LOGI(TAG, "DEVICE_AUTH_CHALLENGE application ACK message_id=%s result=%s",
             s_pending_message_id, esp_err_to_name(err));

    vTaskDelay(pdMS_TO_TICKS(100));
    err = zigbee_ota_send_command_payload(s_pending_challenge_response);
    ESP_LOGI(TAG, "DEVICE_AUTH_CHALLENGE signed response TX message_id=%s bytes=%u result=%s payload=%s",
             s_pending_message_id,
             (unsigned)strlen(s_pending_challenge_response),
             esp_err_to_name(err),
             s_pending_challenge_response);
    vTaskDelete(NULL);
}

static void schedule_challenge_response(void)
{
    if (xTaskCreate(zigbee_ota_challenge_response_task, "zb_auth_resp", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE response task create failed");
    }
}

static bool process_auth_challenge(const char *payload)
{
    if (payload == NULL || strncmp(payload, "A|", 2) != 0) return false;

    const char *message_id = payload + 2;
    const char *separator = strchr(message_id, '|');
    if (separator == NULL || (size_t)(separator - message_id) != OTA_MESSAGE_ID_HEX_LEN ||
        !is_hex_string(message_id, OTA_MESSAGE_ID_HEX_LEN)) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: invalid message_id");
        return true;
    }

    const char *challenge_b64 = separator + 1;
    if (strchr(challenge_b64, '|') != NULL || strlen(challenge_b64) != OTA_CHALLENGE_B64URL_LEN) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: invalid compact challenge length=%u expected=%u",
                 (unsigned)strlen(challenge_b64), OTA_CHALLENGE_B64URL_LEN);
        return true;
    }

    uint8_t challenge[DEVICE_AUTH_CHALLENGE_LEN];
    const esp_err_t decode_err = base64url_decode_challenge(challenge_b64, challenge);
    if (decode_err != ESP_OK || bytes_all_zero(challenge, sizeof(challenge))) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: invalid challenge data decode=%s all_zero=%s",
                 esp_err_to_name(decode_err), bytes_all_zero(challenge, sizeof(challenge)) ? "true" : "false");
        memset(challenge, 0, sizeof(challenge));
        return true;
    }

    char incoming_message_id[OTA_MESSAGE_ID_HEX_LEN + 1];
    memcpy(incoming_message_id, message_id, OTA_MESSAGE_ID_HEX_LEN);
    incoming_message_id[OTA_MESSAGE_ID_HEX_LEN] = '\0';

    char stored_mid[OTA_MESSAGE_ID_HEX_LEN + 1];
    uint8_t stored_challenge[DEVICE_AUTH_CHALLENGE_LEN];
    char stored_response[OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1];
    bool stored_pending = false;
    const esp_err_t load_err = challenge_state_load(stored_mid, stored_challenge, stored_response, &stored_pending);
    if (load_err == ESP_OK && stored_pending && strcmp(stored_mid, incoming_message_id) == 0) {
        if (memcmp(stored_challenge, challenge, DEVICE_AUTH_CHALLENGE_LEN) != 0) {
            ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: message_id reuse with different challenge message_id=%s",
                     incoming_message_id);
            memset(challenge, 0, sizeof(challenge));
            memset(stored_challenge, 0, sizeof(stored_challenge));
            return true;
        }
        memcpy(s_pending_message_id, stored_mid, sizeof(s_pending_message_id));
        memcpy(s_pending_challenge, stored_challenge, sizeof(s_pending_challenge));
        strncpy(s_pending_challenge_response, stored_response, sizeof(s_pending_challenge_response) - 1);
        s_pending_challenge_response[sizeof(s_pending_challenge_response) - 1] = '\0';
        s_pending_challenge_valid = true;
        ESP_LOGW(TAG, "DEVICE_AUTH_CHALLENGE duplicate accepted idempotently message_id=%s; using NVS response=%s",
                 s_pending_message_id, s_pending_challenge_response);
        memset(challenge, 0, sizeof(challenge));
        memset(stored_challenge, 0, sizeof(stored_challenge));
        schedule_challenge_response();
        return true;
    }
    memset(stored_challenge, 0, sizeof(stored_challenge));

    char response[OTA_CHALLENGE_RESPONSE_FRAME_MAX + 1];
    const esp_err_t response_err = build_challenge_response(incoming_message_id, challenge, response);
    if (response_err != ESP_OK) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: response signing failed=%s", esp_err_to_name(response_err));
        memset(challenge, 0, sizeof(challenge));
        return true;
    }

    const esp_err_t save_err = challenge_state_save(incoming_message_id, challenge, response);
    if (save_err != ESP_OK) {
        ESP_LOGE(TAG, "DEVICE_AUTH_CHALLENGE rejected: NVS persist failed=%s", esp_err_to_name(save_err));
        memset(challenge, 0, sizeof(challenge));
        memset(response, 0, sizeof(response));
        return true;
    }

    memcpy(s_pending_message_id, incoming_message_id, sizeof(s_pending_message_id));
    memcpy(s_pending_challenge, challenge, sizeof(s_pending_challenge));
    strncpy(s_pending_challenge_response, response, sizeof(s_pending_challenge_response) - 1);
    s_pending_challenge_response[sizeof(s_pending_challenge_response) - 1] = '\0';
    s_pending_challenge_valid = true;

    ESP_LOGI(TAG,
             "DEVICE_AUTH_CHALLENGE verified+stored endpoint=%u message_id=%s wire_bytes=%u challenge_bytes=%u nvs=ota_sec/%s response_bytes=%u",
             ZIGBEE_OTA_ENDPOINT,
             s_pending_message_id,
             (unsigned)strlen(payload),
             DEVICE_AUTH_CHALLENGE_LEN,
             OTA_NVS_CHALLENGE_VALUE,
             (unsigned)strlen(s_pending_challenge_response));
    ESP_LOGI(TAG,
             "DEVICE_AUTH_CHALLENGE response canonical=%s|<device_id>|%s|<32 raw challenge bytes>",
             OTA_CHALLENGE_RESPONSE_DOMAIN, s_pending_message_id);
    ESP_LOGI(TAG, "DEVICE_AUTH_CHALLENGE response payload=%s", s_pending_challenge_response);

    memset(challenge, 0, sizeof(challenge));
    memset(response, 0, sizeof(response));
    schedule_challenge_response();
    return true;
}

static void zigbee_ota_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t err = zigbee_ota_send_command_payload(DIAG_PONG);
    ESP_LOGI(TAG, "MQTT/ZIGBEE TEST TX: D|PONG result=%s", esp_err_to_name(err));
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

    ESP_LOGI(TAG, "MQTT/ZIGBEE TEST RX: endpoint=%u bytes=%u payload=%.*s",
             ZIGBEE_OTA_ENDPOINT, (unsigned)payload_len, (int)payload_len, payload);

    if (process_auth_challenge(payload)) return true;

    if (strcmp(payload, DIAG_PING) == 0) {
        ESP_LOGI(TAG, "MQTT/ZIGBEE TEST RX OK: D|PING received on endpoint=%u", ZIGBEE_OTA_ENDPOINT);
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

    ESP_LOGI(TAG, "MQTT/ZIGBEE SET_ATTR callback endpoint=%u cluster=0x%04x attr=0x%04x type=0x%02x size=%u",
             message->info.dst_endpoint,
             message->info.cluster,
             message->attribute.id,
             message->attribute.data.type,
             message->attribute.data.size);

    const esp_zb_zcl_attribute_data_t *data = &message->attribute.data;
    if (data->value == NULL || data->type != ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING || data->size < 1) return true;
    const uint8_t *zcl = (const uint8_t *)data->value;
    size_t len = zcl[0];
    if (len == 0 || len > OTA_CONFIG_MAX_PAYLOAD_LEN || len + 1 > data->size) {
        ESP_LOGE(TAG, "MQTT/ZIGBEE SET_ATTR invalid CHAR_STRING declared=%u size=%u", (unsigned)len, data->size);
        return true;
    }
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
