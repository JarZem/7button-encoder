#include "storage.h"

#include <string.h>
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fatal_error.h"
#include "nvs.h"
#include "nvs_flash.h"

#define STORAGE_NAMESPACE "remote"
#define SAVE_DELAY_US     (5 * 1000 * 1000)
#define OTA_CONFIG_KEY    "ota_cfg"

static const char *TAG = "storage";
static esp_timer_handle_t s_save_timer;
static device_state_t s_pending_state;

static void save_timer_callback(void *arg)
{
    (void)arg;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nelze otevřít NVS pro zápis: %s", esp_err_to_name(err));
        return;
    }

    err = nvs_set_u8(handle, "switches", s_pending_state.switches);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "last", s_pending_state.last_active_switches);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "bright", s_pending_state.brightness);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "temp", s_pending_state.white_temperature);
    }
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "rs232", s_pending_state.rs232_enabled ? 1 : 0);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Uložení stavu do NVS selhalo: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Stav uložen do NVS: switches=0x%02x last=0x%02x brightness=%u temp=%uK rs232=%s",
                 s_pending_state.switches,
                 s_pending_state.last_active_switches,
                 s_pending_state.brightness,
                 s_pending_state.white_temperature,
                 s_pending_state.rs232_enabled ? "ON" : "OFF");
    }
}

void storage_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = save_timer_callback,
        .name = "state_save",
    };
    FATAL_ERROR_CHECK(esp_timer_create(&args, &s_save_timer));
}

void storage_load(device_state_t *state)
{
    if (state == NULL) {
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "NVS stav zatím neexistuje, používám výchozí hodnoty");
        return;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Nelze otevřít NVS pro čtení: %s", esp_err_to_name(err));
        return;
    }

    uint8_t value_u8;
    uint16_t value_u16;

    if (nvs_get_u8(handle, "switches", &value_u8) == ESP_OK) {
        state->switches = value_u8;
    }
    if (nvs_get_u8(handle, "last", &value_u8) == ESP_OK) {
        state->last_active_switches = value_u8;
    }
    if (nvs_get_u8(handle, "bright", &value_u8) == ESP_OK) {
        state->brightness = value_u8;
    }
    if (nvs_get_u16(handle, "temp", &value_u16) == ESP_OK) {
        state->white_temperature = value_u16;
    }
    if (nvs_get_u8(handle, "rs232", &value_u8) == ESP_OK) {
        state->rs232_enabled = value_u8 != 0;
    }

    nvs_close(handle);
}

void storage_schedule_save(const device_state_t *state)
{
    if (state == NULL || s_save_timer == NULL) {
        return;
    }

    memcpy(&s_pending_state, state, sizeof(s_pending_state));
    esp_err_t err = esp_timer_stop(s_save_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fatal_error_restart(TAG, "esp_timer_stop state_save", err);
    }
    FATAL_ERROR_CHECK(esp_timer_start_once(s_save_timer, SAVE_DELAY_US));
}

bool storage_load_zigbee_last_channel(uint8_t *channel)
{
    if (channel == NULL) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Zigbee last channel not loaded: %s", esp_err_to_name(err));
        return false;
    }

    uint8_t value = 0;
    err = nvs_get_u8(handle, "zb_ch", &value);
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGI(TAG, "Zigbee last channel not stored yet: %s", esp_err_to_name(err));
        return false;
    }

    *channel = value;
    ESP_LOGI(TAG, "Zigbee last successful channel loaded from NVS: %u", value);
    return true;
}

void storage_save_zigbee_last_channel(uint8_t channel)
{
    if (channel < 11 || channel > 26) {
        ESP_LOGW(TAG, "Refusing to store invalid Zigbee channel: %u", channel);
        return;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, "zb_ch", channel);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Zigbee last channel save failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Zigbee last successful channel saved to NVS: %u", channel);
    }
}

bool storage_load_ota_config(ota_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        ESP_LOGI(TAG, "OTA provisioning config not loaded: %s", esp_err_to_name(err));
        return false;
    }

    size_t size = sizeof(*config);
    err = nvs_get_blob(handle, OTA_CONFIG_KEY, config, &size);
    nvs_close(handle);

    if (err != ESP_OK || size != sizeof(*config)) {
        ESP_LOGI(TAG, "OTA provisioning config missing or invalid: %s size=%u",
                 esp_err_to_name(err),
                 (unsigned)size);
        ota_config_clear(config);
        return false;
    }

    ESP_LOGI(TAG, "OTA provisioning config loaded from NVS: ssid=%s host=%s code=%s token_len=%u",
             config->ssid,
             config->host,
             config->code,
             (unsigned)strlen(config->token));
    return true;
}

bool storage_save_ota_config(const ota_config_t *config)
{
    if (config == NULL) {
        return false;
    }

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, OTA_CONFIG_KEY, config, sizeof(*config));
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (handle != 0) {
        nvs_close(handle);
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA provisioning config save failed: %s", esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG, "OTA provisioning config saved to NVS: ssid=%s host=%s code=%s token_len=%u",
             config->ssid,
             config->host,
             config->code,
             (unsigned)strlen(config->token));
    return true;
}
