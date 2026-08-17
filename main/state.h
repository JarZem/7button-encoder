#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "event_bus.h"

#define DEVICE_SWITCH_COUNT 6
#define DEVICE_SWITCH_MASK  0x3F

#define DEVICE_BRIGHTNESS_DEFAULT 100
#define DEVICE_BRIGHTNESS_MIN       0
#define DEVICE_BRIGHTNESS_MAX     100

#define DEVICE_WHITE_TEMP_DEFAULT 4000
#define DEVICE_WHITE_TEMP_MIN     3000
#define DEVICE_WHITE_TEMP_MAX     6500

typedef struct {
    uint8_t switches;
    uint8_t last_active_switches;
    uint8_t brightness;
    uint16_t white_temperature;
    bool rs232_enabled;
} device_state_t;

typedef enum {
    STATE_CHANGE_LOCAL,
    STATE_CHANGE_ZIGBEE,
    STATE_CHANGE_INTERNAL,
} state_change_source_t;

void state_init(void);
void state_handle_input_event(const input_event_t *event);
const device_state_t *state_get(void);
void state_set_switch(uint8_t switch_id, bool enabled, state_change_source_t source);
void state_set_switch_mask(uint8_t mask, state_change_source_t source);
void state_set_brightness(uint8_t percent, state_change_source_t source);
void state_set_white_temperature(uint16_t kelvin, state_change_source_t source);
void state_set_rs232_enabled_source(bool enabled, state_change_source_t source);
void state_all_off(state_change_source_t source);
void state_restore_last(state_change_source_t source);
void state_set_rs232_enabled(bool enabled);
