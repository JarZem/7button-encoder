#pragma once

#include <stdbool.h>
#include "state.h"
#include "ota_config.h"

void storage_init(void);
void storage_load(device_state_t *state);
void storage_schedule_save(const device_state_t *state);
bool storage_load_zigbee_last_channel(uint8_t *channel);
void storage_save_zigbee_last_channel(uint8_t channel);
bool storage_load_ota_config(ota_config_t *config);
bool storage_save_ota_config(const ota_config_t *config);
