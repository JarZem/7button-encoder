#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "state.h"

void debug_console_init(void);
void debug_console_publish_state(const char *reason, const device_state_t *state);
void debug_console_publish_ha_change(const device_state_t *state);
void debug_console_publish_ota_window(bool active, uint32_t timeout_seconds);
void debug_console_publish_zigbee_pairing(const char *event,
                                          bool joined,
                                          bool factory_new,
                                          bool steering,
                                          uint32_t channel_mask,
                                          uint8_t current_channel,
                                          const char *status);
