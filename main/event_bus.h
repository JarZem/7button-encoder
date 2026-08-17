#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

typedef enum {
    INPUT_EVENT_BUTTON_PRESSED,
    INPUT_EVENT_BUTTON_RELEASED,
    INPUT_EVENT_ENCODER_CW,
    INPUT_EVENT_ENCODER_CCW,
    INPUT_EVENT_ZIGBEE_REPAIR_REQUEST,
    INPUT_EVENT_OTA_REQUEST,
    INPUT_EVENT_ZIGBEE_SWITCH_SET,
    INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET,
    INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET,
    INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET,
    INPUT_EVENT_ZIGBEE_RS232_SET,
    INPUT_EVENT_ZIGBEE_OTA_SET,
} input_event_type_t;

typedef struct {
    input_event_type_t type;
    uint8_t input_id;
    int32_t value;
    TickType_t tick;
} input_event_t;

void event_bus_init(void);
bool event_bus_publish(const input_event_t *event);
bool event_bus_receive(input_event_t *event, TickType_t timeout);
