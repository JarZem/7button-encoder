#pragma once

#include <stdbool.h>
#include "state.h"

void storage_init(void);
void storage_load(device_state_t *state);
void storage_schedule_save(const device_state_t *state);
bool storage_load_zigbee_last_channel(uint8_t *channel);
void storage_save_zigbee_last_channel(uint8_t channel);
