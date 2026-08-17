#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "state.h"

typedef struct {
    uint8_t last_ack_sequence;
    bool communication_error;
    uint32_t consecutive_failures;
    uint32_t sent_frame_count;
    uint32_t received_ack_count;
    uint32_t checksum_error_count;
    uint32_t retry_count;
} uart_transport_diagnostics_t;

void uart_transport_init(void);
void uart_transport_send_state(const device_state_t *state);
void uart_transport_get_diagnostics(uart_transport_diagnostics_t *diagnostics);
