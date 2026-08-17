#pragma once

#include <stdbool.h>

#include "state.h"

void zigbee_minimal_init(void);
void zigbee_minimal_apply_state(const device_state_t *state, bool ota_enabled, bool report);
void zigbee_minimal_request_repair(void);
