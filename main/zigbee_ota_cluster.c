#include "zigbee_ota_cluster.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aps/esp_zigbee_aps.h"
#include "device_credentials.h"
#include "device_identity.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "ota_config.h"
#include "ota_service.h"

static const char *TAG = "zigbee_ota_cluster";

#define HELLO_WAIT_ATTEMPTS 30
#define HELLO_SIGNATURE_B64_MAX 96
#define HELLO_START_DELAY_MS 5000
#define APS_CONFIRM_STUCK_MS 30000
#define APS_COORDINATOR_SHORT_ADDR 0x0000
#define APS_COORDINATOR_ENDPOINT 1
#define APS_RADIUS 2
#define DIAG_PING "D|PING"
#define DIAG_PONG "D|PONG"
#define DIAG_LEN_PREFIX "D|LEN|"
#define DIAG_STOP "D|STOP"
#define DIAG_LEN_MIN 6
#define DIAG_LEN_MAX 100
#define DIAG_REPEAT_MS 20000
#define APP_ACK_MAX_ATTEMPTS 40
#define APP_ACK_RETRY_MS 250

static uint8_t s_ota_payload_attr[ZIGBEE_OTA_ZCL_STRING_CAPACITY + 1];
static bool s_hello_task_started;
static volatile bool s_auth_challenge_received;
static bool s_diag_task_started;
static bool s_len_test_task_started;
static uint32_t s_hello_delay_ms;
static volatile size_t s_len_test_payload_len;

/* Exactly one APS request may be outstanding. Keep the buffer alive until confirm. */
static volatile bool s_aps_tx_pending;
static int64_t s_aps_tx_started_ms;
static uint8_t s_aps_tx_payload[ZIGBEE_OTA_COMMAND_PAYLOAD_MAX + 1];
static size_t s_aps_tx_payload_len;
static uint32_t s_aps_tx_ok_count;
static uint32_t s_aps_tx_fail_count;

static bool s_app_ack_task_started;
static char s_app_ack_payload[32];

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

static void zigbee_ota_aps_confirm_handler(esp_zb_apsde_data_confirm_t confirm)
{
    /* esp-zigbee 1.6.x confirm does not carry cluster_id; this module allows only one APS TX. */
    const bool ours = confirm.src_endpoint == ZIGBEE_OTA_ENDPOINT &&
                      confirm.dst_endpoint == APS_COORDINATOR_ENDPOINT &&
                      confirm.dst_addr.addr_short == APS_COORDINATOR_SHORT_ADDR;
    if (!ours) return;

    const uint8_t status = confirm.status;
    if (status == 0x00) {
        ++s_aps_tx_ok_count;
        ESP_LOGI(TAG,
                 "APS CONFIRM OK dst=0x%04x/%u bytes=%u ok=%lu fail=%lu",
                 confirm.dst_addr.addr_short,
                 confirm.dst_endpoint,
                 (unsigned)s_aps_tx_payload_len,
                 (unsigned long)s_aps_tx_ok_count,
                 (unsigned long)s_aps_tx_fail_count);
    } else {
        ++s_aps_tx_fail_count;
        ESP_LOGE(TAG,
                 "APS CONFIRM FAIL status=0x%02x dst=0x%04x/%u bytes=%u ok=%lu fail=%lu; no automatic retry",
                 status,
                 confirm.dst_addr.addr_short,
                 confirm.dst_endpoint,
                 (unsigned)s_aps_tx_payload_len,
                 (unsigned long)s_aps_tx_ok_count,
                 (unsigned long)s_aps_tx_fail_count);
    }

    s_aps_tx_pending = false;
    s_aps_tx_payload_len = 0;
    s_aps_tx_started_ms = 0;
}

static esp_err_t zigbee_ota_send_aps_payload(const char *payload)
{
    if (payload == NULL) return ESP_ERR_INVALID_ARG;
    const size_t payload_len = strlen(payload);
    if (payload_len == 0 || payload_len > ZIGBEE_OTA_COMMAND_PAYLOAD_MAX) {
        ESP_LOGE(TAG, "APS TX blocked: bytes=%u max=%u",
                 (unsigned)payload_len, ZIGBEE_OTA_COMMAND_PAYLOAD_MAX);
        return ESP_ERR_INVALID_SIZE;
    }
    if (!zigbee_ota_network_identity_valid()) return ESP_ERR_INVALID_STATE;

    if (s_aps_tx_pending) {
        const int64_t age_ms = s_aps_tx_started_ms > 0
            ? (esp_timer_get_time() / 1000) - s_aps_tx_started_ms
            : 0;
        ESP_LOGW(TAG,
                 "APS TX backpressure: previous request still pending age_ms=%lld bytes=%u; new payload not queued",
                 (long long)age_ms,
                 (unsigned)payload_len);
        if (age_ms >= APS_CONFIRM_STUCK_MS) {
            ESP_LOGE(TAG,
                     "APS TX transport stuck: no APSDE-DATA.confirm for >=%u ms; transport remains blocked to protect Zigbee queues",
                     APS_CONFIRM_STUCK_MS);
        }
        return ESP_ERR_INVALID_STATE;
    }

    memcpy(s_aps_tx_payload, payload, payload_len);
    s_aps_tx_payload[payload_len] = '\0';
    s_aps_tx_payload_len = payload_len;

    esp_zb_apsde_data_req_t req = {0};
    req.dst_addr_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT;
    req.dst_addr.addr_short = APS_COORDINATOR_SHORT_ADDR;
    req.dst_endpoint = APS_COORDINATOR_ENDPOINT;
    req.src_endpoint = ZIGBEE_OTA_ENDPOINT;
    req.profile_id = ESP_ZB_AF_HA_PROFILE_ID;
    req.cluster_id = ZIGBEE_OTA_CLUSTER_ID;
    req.asdu_length = payload_len;
    req.asdu = s_aps_tx_payload;
    req.radius = APS_RADIUS;
    /* 92/100-byte transport test: request APS ACK only. Do not request fragmentation. */
    req.tx_options = ESP_ZB_APSDE_TX_OPT_ACK_TX;
    req.use_alias = false;

    s_aps_tx_pending = true;
    s_aps_tx_started_ms = esp_timer_get_time() / 1000;

    esp_zb_lock_acquire(portMAX_DELAY);
    const esp_err_t err = esp_zb_aps_data_request(&req);
    esp_zb_lock_release();

    if (err != ESP_OK) {
        s_aps_tx_pending = false;
        s_aps_tx_payload_len = 0;
        s_aps_tx_started_ms = 0;
        ++s_aps_tx_fail_count;
        ESP_LOGE(TAG,
                 "APS REQUEST rejected immediately err=%s(0x%x) bytes=%u ok=%lu fail=%lu",
                 esp_err_to_name(err), err, (unsigned)payload_len,
                 (unsigned long)s_aps_tx_ok_count,
                 (unsigned long)s_aps_tx_fail_count);
        return err;
    }

    ESP_LOGI(TAG,
             "APS REQUEST queued dst=0x%04x/%u cluster=0x%04x bytes=%u options=ACK_TX payload=%s",
             APS_COORDINATOR_SHORT_ADDR,
             APS_COORDINATOR_ENDPOINT,
             ZIGBEE_OTA_CLUSTER_ID,
             (unsigned)payload_len,
             payload);
    return ESP_OK;
}

static void zigbee_ota_app_ack_task(void *arg)
{
    (void)arg;
    for (unsigned attempt = 1; attempt <= APP_ACK_MAX_ATTEMPTS; ++attempt) {
        if (!s_aps_tx_pending) {
            const esp_err_t err = zigbee_ota_send_aps_payload(s_app_ack_payload);
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "APP ACK queued attempt=%u payload=%s", attempt, s_app_ack_payload);
                s_app_ack_task_started = false;
                vTaskDelete(NULL);
                return;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(APP_ACK_RETRY_MS));
    }
    ESP_LOGE(TAG, "APP ACK not queued after %u attempts payload=%s",
             APP_ACK_MAX_ATTEMPTS, s_app_ack_payload);
    s_app_ack_task_started = false;
    vTaskDelete(NULL);
}

static void zigbee_ota_schedule_app_ack(const char *message_id, size_t message_id_len)
{
    if (message_id == NULL || message_id_len != 16) {
        ESP_LOGW(TAG, "APP ACK not scheduled: invalid message_id length=%u", (unsigned)message_id_len);
        return;
    }
    if (s_app_ack_task_started) {
        ESP_LOGW(TAG, "APP ACK already pending; duplicate challenge will not create another task");
        return;
    }
    snprintf(s_app_ack_payload, sizeof(s_app_ack_payload), "R|%.*s|OK", 16, message_id);
    s_app_ack_task_started = true;
    if (xTaskCreate(zigbee_ota_app_ack_task, "zb_ota_ack", 3072, NULL, 5, NULL) != pdPASS) {
        s_app_ack_task_started = false;
        ESP_LOGE(TAG, "APP ACK task create failed payload=%s", s_app_ack_payload);
    }
}

static void zigbee_ota_diag_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_err_t err = zigbee_ota_send_aps_payload(DIAG_PONG);
    ESP_LOGI(TAG, "DIAG PING result aps_request=%s", esp_err_to_name(err));
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
        ESP_LOGI(TAG, "DIAG LEN iteration=%u bytes=%u transport=aps_ack",
                 iteration, (unsigned)len);
        esp_err_t err = zigbee_ota_send_aps_payload(test);
        ESP_LOGI(TAG, "DIAG LEN request iteration=%u bytes=%u result=%s pending=%s",
                 iteration, (unsigned)len, esp_err_to_name(err),
                 s_aps_tx_pending ? "true" : "false");
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
        if (end == payload + strlen(DIAG_LEN_PREFIX) || *end != '\0' ||
            requested < DIAG_LEN_MIN || requested > DIAG_LEN_MAX) {
            ESP_LOGE(TAG, "DIAG LEN rejected payload=%s allowed=%u..%u",
                     payload, DIAG_LEN_MIN, DIAG_LEN_MAX);
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
        ESP_LOGW(TAG, "DIAG LEN APS test bytes=%lu interval_ms=%u", requested, DIAG_REPEAT_MS);
        return true;
    }

    if (payload_len >= 3 && payload[0] == 'A' && payload[1] == '|') {
        const char *message_id = payload + 2;
        const char *separator = strchr(message_id, '|');
        const size_t message_id_len = separator ? (size_t)(separator - message_id) : 0;
        s_auth_challenge_received = true;
        ESP_LOGI(TAG, "DEVICE_AUTH_CHALLENGE received bytes=%u message_id_len=%u payload=%s",
                 (unsigned)payload_len, (unsigned)message_id_len, payload);
        zigbee_ota_schedule_app_ack(message_id, message_id_len);
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
    if (device_id[0] == '\0' || strcmp(device_id, "00:00:00:00:00:00:00:00") == 0)
        return ESP_ERR_INVALID_STATE;

    uint64_t counter = 0;
    ESP_RETURN_ON_ERROR(device_identity_next_enrollment_counter(&counter), TAG, "HELLO counter update failed");
    char canonical[96];
    int canonical_len = snprintf(canonical, sizeof(canonical), "H|%s|%" PRIu64, device_id, counter);
    if (canonical_len <= 0 || (size_t)canonical_len >= sizeof(canonical)) return ESP_ERR_INVALID_SIZE;

    uint8_t signature_raw[DEVICE_CREDENTIAL_SIGNATURE_RAW_LEN];
    ESP_RETURN_ON_ERROR(device_credentials_sign_raw64((const uint8_t *)canonical,
                                                       (size_t)canonical_len,
                                                       signature_raw),
                        TAG, "HELLO signing failed");
    char signature_b64[HELLO_SIGNATURE_B64_MAX];
    ESP_RETURN_ON_ERROR(base64url_encode(signature_raw, sizeof(signature_raw),
                                         signature_b64, sizeof(signature_b64)),
                        TAG, "HELLO signature encoding failed");

    char payload[ZIGBEE_OTA_HELLO_FRAME_MAX + 1];
    int payload_len = snprintf(payload, sizeof(payload), "H|%" PRIu64 "|%s", counter, signature_b64);
    if (payload_len <= 0 || payload_len > ZIGBEE_OTA_HELLO_FRAME_MAX) return ESP_ERR_INVALID_SIZE;
    ESP_LOGI(TAG,
             "HELLO sending signed frame counter=%" PRIu64 " bytes=%d transport=APS ACK one-shot",
             counter, payload_len);
    return zigbee_ota_send_aps_payload(payload);
}

static void zigbee_ota_hello_task(void *arg)
{
    (void)arg;
    if (s_hello_delay_ms > 0) vTaskDelay(pdMS_TO_TICKS(s_hello_delay_ms));

    unsigned wait_attempt = 0;
    while (!zigbee_ota_network_identity_valid() && wait_attempt < HELLO_WAIT_ATTEMPTS) {
        ++wait_attempt;
        ESP_LOGI(TAG, "HELLO waiting for joined Zigbee network attempt=%u/%u",
                 wait_attempt, HELLO_WAIT_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (!zigbee_ota_network_identity_valid()) {
        ESP_LOGE(TAG, "HELLO stopped: Zigbee network did not become valid");
        s_hello_task_started = false;
        vTaskDelete(NULL);
        return;
    }

    if (s_aps_tx_pending) {
        ESP_LOGW(TAG, "HELLO one-shot suppressed: APS request already pending");
    } else {
        const esp_err_t err = send_secure_hello();
        ESP_LOGI(TAG,
                 "HELLO one-shot request result=%s; automatic retry disabled until APS transport is proven",
                 esp_err_to_name(err));
    }

    s_hello_task_started = false;
    vTaskDelete(NULL);
}

void zigbee_ota_schedule_hello(uint32_t delay_ms)
{
    if (s_auth_challenge_received || s_hello_task_started) return;
    s_hello_delay_ms = delay_ms;
    s_hello_task_started = true;
    ESP_LOGI(TAG,
             "HELLO scheduled delay_ms=%lu transport=APS ACK one-shot no-auto-retry",
             (unsigned long)delay_ms);
    if (xTaskCreate(zigbee_ota_hello_task, "zb_ota_hello", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "HELLO task create failed");
        s_hello_task_started = false;
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
        esp_zb_aps_data_confirm_handler_register(zigbee_ota_aps_confirm_handler);
        ESP_LOGW(TAG,
                 "OTA transport registered: coordinator=0x0000 endpoint=1 cluster=0xfc00; uplink=APS ACK; HELLO one-shot; one outstanding request; max_payload=%u",
                 ZIGBEE_OTA_COMMAND_PAYLOAD_MAX);
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
    if (data->value == NULL || data->type != ESP_ZB_ZCL_ATTR_TYPE_CHAR_STRING || data->size < 1)
        return true;

    const uint8_t *zcl = (const uint8_t *)data->value;
    const size_t len = zcl[0];
    if (len == 0 || len > OTA_CONFIG_MAX_PAYLOAD_LEN || len + 1 > data->size) {
        ESP_LOGW(TAG, "OTA attribute rx invalid size=%u declared=%u", data->size, zcl[0]);
        return true;
    }

    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &zcl[1], len);
    payload[len] = '\0';
    ESP_LOGI(TAG, "OTA attribute rx bytes=%u payload=%s", (unsigned)len, payload);
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
        ESP_LOGW(TAG, "OTA custom command rx invalid wire_size=%u declared=%u",
                 message->data.size, wire[0]);
        return true;
    }

    char payload[OTA_CONFIG_MAX_PAYLOAD_LEN + 1];
    memcpy(payload, &wire[1], len);
    payload[len] = '\0';
    ESP_LOGI(TAG, "OTA custom command rx cmd=0x%02x bytes=%u payload=%s",
             message->info.command.id, (unsigned)len, payload);
    return zigbee_ota_process_payload(payload, len);
}
