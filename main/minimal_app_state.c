#include "minimal_app_state.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "state.h"
#include "storage.h"
#include "zigbee_minimal.h"

typedef enum {
    ENCODER_MODE_BRIGHTNESS,
    ENCODER_MODE_WHITE_TEMPERATURE,
} encoder_mode_t;

static const char *TAG = "minimal_state";

static device_state_t s_state;
static bool s_ota_enabled;
static encoder_mode_t s_encoder_mode = ENCODER_MODE_BRIGHTNESS;
static TickType_t s_last_encoder_tick;

static uint8_t clamp_u8(int32_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return (uint8_t)value;
}

static uint16_t clamp_u16(int32_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < min_value) {
        return min_value;
    }
    if (value > max_value) {
        return max_value;
    }
    return (uint16_t)value;
}

static bool any_switch_on(void)
{
    return (s_state.switches & DEVICE_SWITCH_MASK) != 0;
}

static void log_state(const char *reason)
{
    ESP_LOGI(TAG,
             "STATE reason=%s switch1=%s switch2=%s switch3=%s switch4=%s switch5=%s switch6=%s EnableRS232=%s EnableOTA=%s Light=%s LightBrightness=%u%% WhiteTemperature=%uK",
             reason,
             (s_state.switches & (1U << 0)) ? "ON" : "OFF",
             (s_state.switches & (1U << 1)) ? "ON" : "OFF",
             (s_state.switches & (1U << 2)) ? "ON" : "OFF",
             (s_state.switches & (1U << 3)) ? "ON" : "OFF",
             (s_state.switches & (1U << 4)) ? "ON" : "OFF",
             (s_state.switches & (1U << 5)) ? "ON" : "OFF",
             s_state.rs232_enabled ? "ON" : "OFF",
             s_ota_enabled ? "ON" : "OFF",
             any_switch_on() ? "ON" : "OFF",
             s_state.brightness,
             s_state.white_temperature);
}

static void commit_state_change(const char *reason)
{
    /* Stabilization mode: update Zigbee attributes but do not fan out the
     * complete 9x OnOff + Level + Color report burst after every state
     * change. Incoming HA commands are already acknowledged/reported by the
     * Zigbee attribute callback itself. Initial reports after join remain in
     * zigbee_minimal.c. Local input reporting will be restored selectively
     * after the transport is stable. */
    zigbee_minimal_apply_state(&s_state, s_ota_enabled, false);
    storage_schedule_save(&s_state);
    log_state(reason);
}

static int32_t accelerated_step(TickType_t event_tick, int32_t slow_step, int32_t medium_step,
                                int32_t fast_step, int32_t very_fast_step)
{
    const TickType_t elapsed = event_tick - s_last_encoder_tick;
    s_last_encoder_tick = event_tick;

    if (elapsed <= pdMS_TO_TICKS(60)) {
        return very_fast_step;
    }
    if (elapsed <= pdMS_TO_TICKS(130)) {
        return fast_step;
    }
    if (elapsed <= pdMS_TO_TICKS(300)) {
        return medium_step;
    }
    return slow_step;
}

static void set_switch_mask(uint8_t mask, const char *reason)
{
    const uint8_t next_switches = mask & DEVICE_SWITCH_MASK;
    if (s_state.switches == next_switches) {
        return;
    }

    s_state.switches = next_switches;
    if (any_switch_on()) {
        s_state.last_active_switches = s_state.switches;
    }
    commit_state_change(reason);
}

static void set_switch(uint8_t switch_id, bool enabled, const char *reason)
{
    if (switch_id < 1 || switch_id > DEVICE_SWITCH_COUNT) {
        return;
    }

    const uint8_t bit = (uint8_t)(1U << (switch_id - 1U));
    const uint8_t next = enabled ? (uint8_t)(s_state.switches | bit)
                                 : (uint8_t)(s_state.switches & (uint8_t)~bit);
    set_switch_mask(next, reason);
}

static void set_brightness(uint8_t percent, const char *reason)
{
    const uint8_t next = clamp_u8(percent, DEVICE_BRIGHTNESS_MIN, DEVICE_BRIGHTNESS_MAX);
    if (s_state.brightness == next) {
        return;
    }

    s_state.brightness = next;
    commit_state_change(reason);
}

static void set_white_temperature(uint16_t kelvin, const char *reason)
{
    const uint16_t next = clamp_u16(kelvin, DEVICE_WHITE_TEMP_MIN, DEVICE_WHITE_TEMP_MAX);
    if (s_state.white_temperature == next) {
        return;
    }

    s_state.white_temperature = next;
    commit_state_change(reason);
}

static void set_rs232_enabled(bool enabled, const char *reason)
{
    if (s_state.rs232_enabled == enabled) {
        return;
    }

    s_state.rs232_enabled = enabled;
    commit_state_change(reason);
}

static void set_ota_enabled(bool enabled, const char *reason)
{
    if (s_ota_enabled == enabled) {
        return;
    }

    s_ota_enabled = enabled;
    commit_state_change(reason);
}

static void all_off(const char *reason)
{
    if (any_switch_on()) {
        s_state.last_active_switches = s_state.switches & DEVICE_SWITCH_MASK;
    }
    set_switch_mask(0, reason);
}

static void restore_last(const char *reason)
{
    uint8_t restored = s_state.last_active_switches & DEVICE_SWITCH_MASK;
    if (restored == 0) {
        restored = DEVICE_SWITCH_MASK;
    }
    set_switch_mask(restored, reason);
}

static void handle_button_press(uint8_t button_id)
{
    if (button_id >= 1 && button_id <= DEVICE_SWITCH_COUNT) {
        const bool enabled = ((s_state.switches >> (button_id - 1U)) & 1U) == 0;
        set_switch(button_id, enabled, "button");
        return;
    }

    if (button_id == 7) {
        if (any_switch_on()) {
            all_off("button7_all_off");
        } else {
            restore_last("button7_restore");
        }
        return;
    }

    if (button_id == 8) {
        s_encoder_mode = (s_encoder_mode == ENCODER_MODE_BRIGHTNESS)
                             ? ENCODER_MODE_WHITE_TEMPERATURE
                             : ENCODER_MODE_BRIGHTNESS;
        ESP_LOGI(TAG, "Encoder mode: %s",
                 s_encoder_mode == ENCODER_MODE_BRIGHTNESS ? "brightness" : "white_temperature");
    }
}

static void handle_encoder_rotation(int32_t direction, TickType_t tick)
{
    if (s_encoder_mode == ENCODER_MODE_BRIGHTNESS) {
        const int32_t step = accelerated_step(tick, 1, 2, 5, 10);
        const uint8_t next = clamp_u8((int32_t)s_state.brightness + direction * step,
                                      DEVICE_BRIGHTNESS_MIN,
                                      DEVICE_BRIGHTNESS_MAX);
        set_brightness(next, "encoder_brightness");
    } else {
        const int32_t step = accelerated_step(tick, 100, 200, 300, 500);
        const uint16_t next = clamp_u16((int32_t)s_state.white_temperature + direction * step,
                                        DEVICE_WHITE_TEMP_MIN,
                                        DEVICE_WHITE_TEMP_MAX);
        set_white_temperature(next, "encoder_white_temperature");
    }
}

void minimal_app_state_init(void)
{
    s_state = (device_state_t) {
        .switches = 0,
        .last_active_switches = DEVICE_SWITCH_MASK,
        .brightness = DEVICE_BRIGHTNESS_DEFAULT,
        .white_temperature = DEVICE_WHITE_TEMP_DEFAULT,
        .rs232_enabled = false,
    };
    s_ota_enabled = false;

    storage_load(&s_state);
    s_state.switches &= DEVICE_SWITCH_MASK;
    s_state.last_active_switches &= DEVICE_SWITCH_MASK;
    s_state.brightness = clamp_u8(s_state.brightness, DEVICE_BRIGHTNESS_MIN, DEVICE_BRIGHTNESS_MAX);
    s_state.white_temperature = clamp_u16(s_state.white_temperature,
                                          DEVICE_WHITE_TEMP_MIN,
                                          DEVICE_WHITE_TEMP_MAX);
    if (s_state.last_active_switches == 0) {
        s_state.last_active_switches = DEVICE_SWITCH_MASK;
    }

    s_last_encoder_tick = xTaskGetTickCount();
    zigbee_minimal_apply_state(&s_state, s_ota_enabled, false);
    log_state("boot_loaded");
}

void minimal_app_state_handle_input_event(const input_event_t *event)
{
    if (event == NULL) {
        return;
    }

    switch (event->type) {
        case INPUT_EVENT_BUTTON_PRESSED:
            handle_button_press(event->input_id);
            break;

        case INPUT_EVENT_ENCODER_CW:
            handle_encoder_rotation(1, event->tick);
            break;

        case INPUT_EVENT_ENCODER_CCW:
            handle_encoder_rotation(-1, event->tick);
            break;

        case INPUT_EVENT_ZIGBEE_SWITCH_SET:
            set_switch(event->input_id, event->value != 0, "ha_switch");
            break;

        case INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET:
            if (event->value != 0) {
                restore_last("ha_light_on");
            } else {
                all_off("ha_light_off");
            }
            break;

        case INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET:
            set_brightness((uint8_t)event->value, "ha_brightness");
            break;

        case INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET:
            set_white_temperature((uint16_t)event->value, "ha_white_temperature");
            break;

        case INPUT_EVENT_ZIGBEE_RS232_SET:
            set_rs232_enabled(event->value != 0, "ha_rs232");
            break;

        case INPUT_EVENT_ZIGBEE_OTA_SET:
            set_ota_enabled(event->value != 0, "ha_ota");
            break;

        default:
            break;
    }
}
