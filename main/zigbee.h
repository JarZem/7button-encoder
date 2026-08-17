#pragma once

#include <stdbool.h>
#include "state.h"

void zigbee_init(void);
void zigbee_request_repair(void);
void zigbee_publish_state(const device_state_t *state);
void zigbee_handle_rs232_enabled_from_ha(bool enabled);
bool zigbee_handle_start_ota_from_ha(void);
void zigbee_publish_communication_error(bool communication_error, const void *diagnostics);
