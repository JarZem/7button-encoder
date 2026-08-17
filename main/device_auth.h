#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "device_enrollment.h"

esp_err_t device_auth_init(void);
bool device_auth_available(void);
esp_err_t device_auth_calculate_hmac(const uint8_t *message,
                                     size_t message_len,
                                     uint8_t hmac[DEVICE_AUTH_HMAC_LEN]);

