#pragma once

#include <stdbool.h>
#include <stdint.h>

#define OTA_CONTROL_WINDOW_SECONDS 300U

void ota_control_init(void);
void ota_control_activate_window(void);
bool ota_control_is_window_active(void);
