#pragma once

#include <stdbool.h>
#include "esp_err.h"

void fatal_error_restart(const char *tag, const char *message, esp_err_t err);

#define FATAL_ERROR_CHECK(expr) do {                       \
        esp_err_t _fatal_err = (expr);                     \
        if (_fatal_err != ESP_OK) {                        \
            fatal_error_restart(TAG, #expr, _fatal_err);   \
        }                                                  \
    } while (0)

#define FATAL_ERROR_IF(condition, message) do {             \
        if ((condition)) {                                  \
            fatal_error_restart(TAG, (message), ESP_FAIL);  \
        }                                                  \
    } while (0)
