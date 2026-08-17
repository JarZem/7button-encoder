#include "ota_control.h"

#include "debug_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "fatal_error.h"

static const char *TAG = "ota_control";

static esp_timer_handle_t s_ota_window_timer;
static bool s_ota_window_active;

static void ota_window_timeout(void *arg)
{
    (void)arg;

    s_ota_window_active = false;
    ESP_LOGI(TAG, "OTA okno ukončeno po timeoutu");
    debug_console_publish_ota_window(false, 0);
}

void ota_control_init(void)
{
    const esp_timer_create_args_t args = {
        .callback = ota_window_timeout,
        .name = "ota_window",
    };
    FATAL_ERROR_CHECK(esp_timer_create(&args, &s_ota_window_timer));
}

void ota_control_activate_window(void)
{
    if (s_ota_window_timer == NULL) {
        return;
    }

    esp_err_t err = esp_timer_stop(s_ota_window_timer);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fatal_error_restart(TAG, "esp_timer_stop ota_window", err);
    }

    s_ota_window_active = true;
    FATAL_ERROR_CHECK(esp_timer_start_once(s_ota_window_timer,
                                           OTA_CONTROL_WINDOW_SECONDS * 1000ULL * 1000ULL));
    ESP_LOGI(TAG, "OTA okno aktivní na %u sekund", OTA_CONTROL_WINDOW_SECONDS);
    debug_console_publish_ota_window(true, OTA_CONTROL_WINDOW_SECONDS);
}

bool ota_control_is_window_active(void)
{
    return s_ota_window_active;
}
