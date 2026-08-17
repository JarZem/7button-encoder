#include "device_auth.h"

#include "esp_hmac.h"
#include "esp_log.h"

static const char *TAG = "device_auth";
static bool s_available;

static hmac_key_id_t configured_hmac_key_id(void)
{
    switch (CONFIG_APP_DEVICE_AUTH_HMAC_KEY_ID) {
        case 0: return HMAC_KEY0;
        case 1: return HMAC_KEY1;
        case 2: return HMAC_KEY2;
        case 3: return HMAC_KEY3;
        case 4: return HMAC_KEY4;
        case 5: return HMAC_KEY5;
        default: return HMAC_KEY0;
    }
}

esp_err_t device_auth_init(void)
{
    uint8_t probe[DEVICE_AUTH_HMAC_LEN];
    static const uint8_t message[] = "device-auth-probe";
    const esp_err_t err = esp_hmac_calculate(configured_hmac_key_id(),
                                             message,
                                             sizeof(message) - 1,
                                             probe);
    if (err == ESP_OK) {
        s_available = true;
        ESP_LOGI(TAG, "device auth key: PROVISIONED, hmac_key_id=%d",
                 CONFIG_APP_DEVICE_AUTH_HMAC_KEY_ID);
    } else {
        s_available = false;
        ESP_LOGW(TAG, "device auth key: NOT PROVISIONED (%s)", esp_err_to_name(err));
    }
    return ESP_OK;
}

bool device_auth_available(void)
{
    return s_available;
}

esp_err_t device_auth_calculate_hmac(const uint8_t *message,
                                     size_t message_len,
                                     uint8_t hmac[DEVICE_AUTH_HMAC_LEN])
{
    if (!s_available) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_hmac_calculate(configured_hmac_key_id(), message, message_len, hmac);
}

