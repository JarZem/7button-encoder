#include "status_led.h"

#include "esp_log.h"
#include "ota_service.h"
#include "pins.h"
#include "status_led_manager.h"

#define MODE_NOT_JOINED 0
#define MODE_PAIRING    1
#define MODE_FAILURE    2
#define MODE_FATAL      3

static const char *TAG = "status_led";

static bool ota_external_state(status_led_external_state_t *state, void *ctx)
{
    (void)ctx;
    switch (ota_service_get_state()) {
        case OTA_STATE_PENDING:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_ORANGE, 200, 85};
            return true;
        case OTA_STATE_CONNECTING_WIFI:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_CYAN, 200, 85};
            return true;
        case OTA_STATE_DOWNLOADING:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_WHITE, 200, 90};
            return true;
        case OTA_STATE_VERIFYING:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_BLUE, 200, 90};
            return true;
        case OTA_STATE_SUCCESS:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_GREEN, 200, 90};
            return true;
        case OTA_STATE_FAILED:
            *state = (status_led_external_state_t){true, STATUS_LED_COLOR_RED, 200, 95};
            return true;
        case OTA_STATE_IDLE:
        default:
            return false;
    }
}

void status_led_init(void)
{
    status_led_manager_config_t cfg = {
        .gpio_num = PIN_STATUS_LED,
        .rmt_resolution_hz = 10000000,
        .heartbeat_period_ms = 10000,
        .scheduler_period_ms = 50,
        .heartbeat_pulse_ms = 150,
        .heartbeat_color = STATUS_LED_COLOR_GREEN,
        .external_state_cb = ota_external_state,
    };
    esp_err_t err = status_led_manager_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status LED manager init failed: %s", esp_err_to_name(err));
        return;
    }
    status_led_manager_set_mode(MODE_NOT_JOINED, true, STATUS_LED_COLOR_PURPLE, 600, 40);
}

void status_led_indicate_boot(void)
{
    status_led_manager_enqueue(STATUS_LED_COLOR_YELLOW, 200);
}

void status_led_set_zigbee_joined(bool joined)
{
    status_led_manager_set_mode(MODE_NOT_JOINED, !joined, STATUS_LED_COLOR_PURPLE, 600, 40);
    if (joined) {
        status_led_manager_set_mode(MODE_PAIRING, false, STATUS_LED_COLOR_BLUE, 400, 60);
        status_led_manager_set_mode(MODE_FAILURE, false, STATUS_LED_COLOR_RED, 600, 70);
    }
}

void status_led_set_zigbee_pairing(bool pairing)
{
    status_led_manager_set_mode(MODE_PAIRING, pairing, STATUS_LED_COLOR_BLUE, 400, 60);
    if (pairing) {
        status_led_manager_set_mode(MODE_NOT_JOINED, false, STATUS_LED_COLOR_PURPLE, 600, 40);
        status_led_manager_set_mode(MODE_FAILURE, false, STATUS_LED_COLOR_RED, 600, 70);
    }
}

void status_led_set_failure(bool failed)
{
    status_led_manager_set_mode(MODE_FAILURE, failed, STATUS_LED_COLOR_RED, 600, 70);
    if (failed) status_led_manager_set_mode(MODE_PAIRING, false, STATUS_LED_COLOR_BLUE, 400, 60);
}

void status_led_indicate_local_activity(void)
{
    status_led_manager_enqueue(STATUS_LED_COLOR_GREEN, 200);
}

void status_led_indicate_ha_command(void)
{
    status_led_manager_enqueue(STATUS_LED_COLOR_YELLOW, 200);
}

void status_led_indicate_ha_publish(void)
{
    status_led_manager_enqueue(STATUS_LED_COLOR_GREEN, 200);
    status_led_manager_enqueue(STATUS_LED_COLOR_YELLOW, 200);
}

void status_led_indicate_provision_step(void)
{
    status_led_manager_enqueue(STATUS_LED_COLOR_MAGENTA, 600);
}

void status_led_fatal(void)
{
    status_led_manager_set_mode(MODE_FATAL, true, STATUS_LED_COLOR_RED, 200, 100);
}

void status_led_block_rf_critical(void)
{
    status_led_manager_block();
}

void status_led_unblock_rf_critical(void)
{
    status_led_manager_unblock();
}
