#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    STATUS_LED_COLOR_OFF = 0,
    STATUS_LED_COLOR_GREEN,
    STATUS_LED_COLOR_YELLOW,
    STATUS_LED_COLOR_PURPLE,
    STATUS_LED_COLOR_BLUE,
    STATUS_LED_COLOR_CYAN,
    STATUS_LED_COLOR_ORANGE,
    STATUS_LED_COLOR_WHITE,
    STATUS_LED_COLOR_RED,
    STATUS_LED_COLOR_MAGENTA,
    STATUS_LED_COLOR_COUNT,
} status_led_color_id_t;

typedef struct {
    bool active;
    status_led_color_id_t color;
    uint16_t blink_period_ms; /* 0 = steady */
    uint8_t priority;
} status_led_external_state_t;

typedef bool (*status_led_external_state_cb_t)(status_led_external_state_t *state, void *ctx);

typedef struct {
    int gpio_num;
    uint32_t rmt_resolution_hz;
    uint32_t heartbeat_period_ms;
    uint32_t scheduler_period_ms;
    uint16_t heartbeat_pulse_ms;
    status_led_color_id_t heartbeat_color;
    status_led_external_state_cb_t external_state_cb;
    void *external_state_ctx;
} status_led_manager_config_t;

#define STATUS_LED_MODE_SLOTS 8

esp_err_t status_led_manager_init(const status_led_manager_config_t *config);

/* FIFO of short indications. When full, the oldest entry is discarded. */
void status_led_manager_enqueue(status_led_color_id_t color, uint16_t duration_ms);

/* Persistent/priority states. Highest active priority wins over queued pulses and heartbeat. */
void status_led_manager_set_mode(uint8_t slot, bool active, status_led_color_id_t color,
                                 uint16_t blink_period_ms, uint8_t priority);

/*
 * Prevent any WS2812/RMT transmission while RF-critical code runs.
 * Timers and the FIFO keep advancing; after unblock, rendering resumes on the next scheduler tick.
 * Calls may be nested.
 */
void status_led_manager_block(void);
void status_led_manager_unblock(void);

#ifdef __cplusplus
}
#endif
