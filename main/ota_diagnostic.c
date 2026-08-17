#include "ota_diagnostic.h"

#include <stdio.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota_service.h"
#include "ota_wifi.h"

static const char *TAG = "ota_diag";

#if CONFIG_APP_OTA_DIAG_ENABLE

#define OTA_DIAG_TASK_STACK_SIZE 12288
#define OTA_DIAG_HTTP_TIMEOUT_MS 15000
#define OTA_DIAG_DOWNLOAD_PATH "/remotecontrol7andEncoder.bin"
#define OTA_DIAG_DEVICE_ID "TEST-DEVICE"
#define OTA_DIAG_BEARER_TOKEN "MIICfzCCAWcCAQAwGDEWMBQGA1UEAwwNMTkyLjE2OC4yLjEyMDCCASIwDQYJKoZI"

static esp_err_t ota_diagnostic_download_to_null(void)
{
    char url[128];
    int written = snprintf(url, sizeof(url), "https://%s:%d%s",
                           CONFIG_APP_OTA_DIAG_HOST,
                           CONFIG_APP_OTA_DIAG_PORT,
                           OTA_DIAG_DOWNLOAD_PATH);
    if (written < 0 || written >= (int)sizeof(url)) {
        ESP_LOGE(TAG, "DIAG: URL too long");
        return ESP_ERR_INVALID_SIZE;
    }

    char auth_header[128];
    written = snprintf(auth_header, sizeof(auth_header), "Bearer %s", OTA_DIAG_BEARER_TOKEN);
    if (written < 0 || written >= (int)sizeof(auth_header)) {
        ESP_LOGE(TAG, "DIAG: Authorization header too long");
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "DIAG: HTTPS GET %s", url);
    ESP_LOGI(TAG, "DIAG: header Authorization='Bearer <token:%u chars>'",
             (unsigned)strlen(OTA_DIAG_BEARER_TOKEN));
    ESP_LOGI(TAG, "DIAG: header X-Device-ID='%s'", OTA_DIAG_DEVICE_ID);

    const esp_http_client_config_t config = {
        .url = url,
        .cert_pem = ota_service_get_cert_pem(),
        .timeout_ms = OTA_DIAG_HTTP_TIMEOUT_MS,
        .keep_alive_enable = false,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == NULL) {
        ESP_LOGE(TAG, "DIAG: esp_http_client_init failed");
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = esp_http_client_set_header(client, "Authorization", auth_header);
    ESP_LOGI(TAG, "DIAG: esp_http_client_set_header(Authorization) -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_set_header(client, "X-Device-ID", OTA_DIAG_DEVICE_ID);
    ESP_LOGI(TAG, "DIAG: esp_http_client_set_header(X-Device-ID) -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    err = esp_http_client_open(client, 0);
    ESP_LOGI(TAG, "DIAG: esp_http_client_open() -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        esp_http_client_cleanup(client);
        return err;
    }

    const int64_t content_length = esp_http_client_fetch_headers(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "DIAG: HTTP status=%d Content-Length=%lld",
             status, (long long)content_length);
    if (status != 200) {
        ESP_LOGE(TAG, "DIAG: expected HTTP 200 for token download, got %d", status);
    }

    uint8_t buffer[2048];
    int64_t total_read = 0;
    int next_progress = 10;
    while (status == 200) {
        const int read_len = esp_http_client_read(client, (char *)buffer, sizeof(buffer));
        if (read_len < 0) {
            ESP_LOGE(TAG, "DIAG: esp_http_client_read failed");
            err = ESP_FAIL;
            break;
        }
        if (read_len == 0) {
            if (esp_http_client_is_complete_data_received(client)) {
                err = ESP_OK;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        total_read += read_len;
        if (content_length > 0) {
            const int progress = (int)((total_read * 100) / content_length);
            while (progress >= next_progress && next_progress <= 100) {
                ESP_LOGI(TAG, "DIAG: download progress %d%% (%lld/%lld)",
                         next_progress,
                         (long long)total_read,
                         (long long)content_length);
                next_progress += 10;
            }
        }
    }

    ESP_LOGI(TAG, "DIAG: downloaded %lld bytes to /dev/null", (long long)total_read);
    if (status == 200 && content_length > 0 && total_read != content_length) {
        ESP_LOGE(TAG, "DIAG: downloaded size mismatch");
        err = ESP_ERR_INVALID_SIZE;
    }

    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return status == 200 ? err : ESP_FAIL;
}

void ota_diagnostic_run(void)
{
    ESP_LOGW(TAG, "DIAG: running OTA WiFi HTTPS download diagnostic; Zigbee code is preserved but not started during this isolated test");

    size_t cert_len = 0;
    size_t cert_count = 0;
    ota_service_get_cert_bundle_info(&cert_len, &cert_count);
    ESP_LOGI(TAG, "DIAG: OTA cert bundle loaded bytes=%u cert_count=%u",
             (unsigned)cert_len, (unsigned)cert_count);
    if (cert_len == 0 || cert_count == 0) {
        ESP_LOGE(TAG, "DIAG: OTA cert bundle missing in running ESP");
        return;
    }

    (void)ota_wifi_scan_log(CONFIG_APP_OTA_DIAG_WIFI_SSID);

    ESP_LOGI(TAG, "DIAG: connecting WiFi SSID '%s'", CONFIG_APP_OTA_DIAG_WIFI_SSID);
    esp_err_t err = ota_wifi_connect(CONFIG_APP_OTA_DIAG_WIFI_SSID,
                                     CONFIG_APP_OTA_DIAG_WIFI_PASSWORD);
    ESP_LOGI(TAG, "DIAG: ota_wifi_connect() -> %s", esp_err_to_name(err));
    if (err != ESP_OK) {
        ota_wifi_disconnect();
        return;
    }

    err = ota_diagnostic_download_to_null();
    ESP_LOGI(TAG, "DIAG: OTA HTTPS download diagnostic finished: %s", esp_err_to_name(err));
    ota_wifi_disconnect();
}

static void ota_diagnostic_task(void *arg)
{
    (void)arg;
    ota_diagnostic_run();
    vTaskDelete(NULL);
}

void ota_diagnostic_start(void)
{
    BaseType_t created = xTaskCreate(ota_diagnostic_task,
                                     "ota_diag",
                                     OTA_DIAG_TASK_STACK_SIZE,
                                     NULL,
                                     4,
                                     NULL);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "DIAG: cannot create diagnostic task");
    }
}

#else

void ota_diagnostic_run(void)
{
    (void)TAG;
}

void ota_diagnostic_start(void)
{
    (void)TAG;
}

#endif
