#include "state.h"

#include <stdbool.h>
#include <stdint.h>

#include "debug_console.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "status_led.h"
#include "storage.h"
#include "uart_transport.h"
#include "zigbee.h"

typedef enum {
    ENCODER_MODE_BRIGHTNESS,
    ENCODER_MODE_WHITE_TEMPERATURE,
} encoder_mode_t;

static const char *TAG = "state";
static device_state_t s_state;
static encoder_mode_t s_encoder_mode = ENCODER_MODE_BRIGHTNESS;
static TickType_t s_last_encoder_tick;

static uint8_t clamp_u8(int32_t value, uint8_t min_value, uint8_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return (uint8_t)value;
}

static uint16_t clamp_u16(int32_t value, uint16_t min_value, uint16_t max_value)
{
    if (value < min_value) return min_value;
    if (value > max_value) return max_value;
    return (uint16_t)value;
}

static bool any_switch_on(void)
{
    return (s_state.switches & DEVICE_SWITCH_MASK) != 0;
}

static const char *source_name(state_change_source_t source)
{
    switch (source) {
        case STATE_CHANGE_LOCAL: return "local";
        case STATE_CHANGE_ZIGBEE: return "zigbee";
        case STATE_CHANGE_INTERNAL: return "internal";
        default: return "unknown";
    }
}

static void commit_state_change(state_change_source_t source)
{
    zigbee_publish_state(&s_state);
    uart_transport_send_state(&s_state);
    if (source == STATE_CHANGE_LOCAL) status_led_indicate_local_activity();
    else if (source == STATE_CHANGE_ZIGBEE) status_led_indicate_ha_command();
    if (source == STATE_CHANGE_ZIGBEE) debug_console_publish_ha_change(&s_state);
    debug_console_publish_state(source_name(source), &s_state);
    storage_schedule_save(&s_state);
}

static void publish_loaded_state(void)
{
    zigbee_publish_state(&s_state);
    uart_transport_send_state(&s_state);
    debug_console_publish_state("boot_loaded", &s_state);
}

static int32_t accelerated_step(TickType_t event_tick, int32_t slow_step,
                                int32_t medium_step, int32_t fast_step,
                                int32_t very_fast_step)
{
    const TickType_t elapsed = event_tick - s_last_encoder_tick;
    s_last_encoder_tick = event_tick;
    if (elapsed <= pdMS_TO_TICKS(60)) return very_fast_step;
    if (elapsed <= pdMS_TO_TICKS(130)) return fast_step;
    if (elapsed <= pdMS_TO_TICKS(300)) return medium_step;
    return slow_step;
}

static void handle_switch_button(uint8_t button_id)
{
    if (button_id >= 1 && button_id <= DEVICE_SWITCH_COUNT) {
        const bool enabled = ((s_state.switches >> (button_id - 1U)) & 1U) == 0;
        state_set_switch(button_id, enabled, STATE_CHANGE_LOCAL);
    } else if (button_id == 7) {
        if (any_switch_on()) state_all_off(STATE_CHANGE_LOCAL);
        else state_restore_last(STATE_CHANGE_LOCAL);
    }
}

static void handle_encoder_button(void)
{
    s_encoder_mode = s_encoder_mode == ENCODER_MODE_BRIGHTNESS
                         ? ENCODER_MODE_WHITE_TEMPERATURE
                         : ENCODER_MODE_BRIGHTNESS;
    ESP_LOGI(TAG, "Encoder mode=%s",
             s_encoder_mode == ENCODER_MODE_BRIGHTNESS ? "brightness" : "white_temperature");
}

static void handle_encoder_rotation(int32_t direction, TickType_t tick)
{
    if (s_encoder_mode == ENCODER_MODE_BRIGHTNESS) {
        const int32_t step = accelerated_step(tick, 1, 2, 5, 10);
        state_set_brightness(clamp_u8((int32_t)s_state.brightness + direction * step,
                                      DEVICE_BRIGHTNESS_MIN, DEVICE_BRIGHTNESS_MAX),
                             STATE_CHANGE_LOCAL);
    } else {
        const int32_t step = accelerated_step(tick, 100, 200, 300, 500);
        state_set_white_temperature(clamp_u16((int32_t)s_state.white_temperature + direction * step,
                                              DEVICE_WHITE_TEMP_MIN, DEVICE_WHITE_TEMP_MAX),
                                    STATE_CHANGE_LOCAL);
    }
}

void state_init(void)
{
    s_state = (device_state_t) {
        .switches = 0,
        .last_active_switches = DEVICE_SWITCH_MASK,
        .brightness = DEVICE_BRIGHTNESS_DEFAULT,
        .white_temperature = DEVICE_WHITE_TEMP_DEFAULT,
        .rs232_enabled = false,
    };
    storage_load(&s_state);
    s_state.switches &= DEVICE_SWITCH_MASK;
    s_state.last_active_switches &= DEVICE_SWITCH_MASK;
    s_state.brightness = clamp_u8(s_state.brightness, DEVICE_BRIGHTNESS_MIN, DEVICE_BRIGHTNESS_MAX);
    s_state.white_temperature = clamp_u16(s_state.white_temperature, DEVICE_WHITE_TEMP_MIN, DEVICE_WHITE_TEMP_MAX);
    if (s_state.last_active_switches == 0) s_state.last_active_switches = DEVICE_SWITCH_MASK;
    s_last_encoder_tick = xTaskGetTickCount();
    publish_loaded_state();
}

void state_handle_input_event(const input_event_t *event)
{
    if (event == NULL) return;
    switch (event->type) {
        case INPUT_EVENT_BUTTON_PRESSED:
            if (event->input_id >= 1 && event->input_id <= 7) handle_switch_button(event->input_id);
            else if (event->input_id == 8) handle_encoder_button();
            break;
        case INPUT_EVENT_ENCODER_CW:
            handle_encoder_rotation(1, event->tick);
            break;
        case INPUT_EVENT_ENCODER_CCW:
            handle_encoder_rotation(-1, event->tick);
            break;
        case INPUT_EVENT_ZIGBEE_SWITCH_SET:
            if (event->input_id == 0) state_set_switch_mask((uint8_t)event->value, STATE_CHANGE_ZIGBEE);
            else state_set_switch(event->input_id, event->value != 0, STATE_CHANGE_ZIGBEE);
            break;
        case INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET:
            if (event->value != 0) state_restore_last(STATE_CHANGE_ZIGBEE);
            else state_all_off(STATE_CHANGE_ZIGBEE);
            break;
        case INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET:
            state_set_brightness((uint8_t)event->value, STATE_CHANGE_ZIGBEE);
            break;
        case INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET:
            state_set_white_temperature((uint16_t)event->value, STATE_CHANGE_ZIGBEE);
            break;
        case INPUT_EVENT_ZIGBEE_RS232_SET:
            state_set_rs232_enabled_source(event->value != 0, STATE_CHANGE_ZIGBEE);
            break;
        default:
            break;
    }
}

const device_state_t *state_get(void)
{
    return &s_state;
}

void state_set_switch(uint8_t switch_id, bool enabled, state_change_source_t source)
{
    if (switch_id < 1 || switch_id > DEVICE_SWITCH_COUNT) return;
    const uint8_t bit = (uint8_t)(1U << (switch_id - 1U));
    state_set_switch_mask(enabled ? (uint8_t)(s_state.switches | bit)
                                  : (uint8_t)(s_state.switches & (uint8_t)~bit), source);
}

void state_set_switch_mask(uint8_t mask, state_change_source_t source)
{
    const uint8_t next = mask & DEVICE_SWITCH_MASK;
    if (s_state.switches == next) return;
    s_state.switches = next;
    if (any_switch_on()) s_state.last_active_switches = s_state.switches;
    commit_state_change(source);
}

void state_set_brightness(uint8_t percent, state_change_source_t source)
{
    const uint8_t next = clamp_u8(percent, DEVICE_BRIGHTNESS_MIN, DEVICE_BRIGHTNESS_MAX);
    if (s_state.brightness == next) return;
    s_state.brightness = next;
    commit_state_change(source);
}

void state_set_white_temperature(uint16_t kelvin, state_change_source_t source)
{
    const uint16_t next = clamp_u16(kelvin, DEVICE_WHITE_TEMP_MIN, DEVICE_WHITE_TEMP_MAX);
    if (s_state.white_temperature == next) return;
    s_state.white_temperature = next;
    commit_state_change(source);
}

void state_set_rs232_enabled_source(bool enabled, state_change_source_t source)
{
    if (s_state.rs232_enabled == enabled) return;
    s_state.rs232_enabled = enabled;
    commit_state_change(source);
}

void state_all_off(state_change_source_t source)
{
    if (any_switch_on()) s_state.last_active_switches = s_state.switches & DEVICE_SWITCH_MASK;
    state_set_switch_mask(0, source);
}

void state_restore_last(state_change_source_t source)
{
    uint8_t restored = s_state.last_active_switches & DEVICE_SWITCH_MASK;
    if (restored == 0) restored = DEVICE_SWITCH_MASK;
    state_set_switch_mask(restored, source);
}

void state_set_rs232_enabled(bool enabled)
{
    state_set_rs232_enabled_source(enabled, STATE_CHANGE_INTERNAL);
}
