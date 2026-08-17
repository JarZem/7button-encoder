#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PROTOCOL_STATE_START       0xAA
#define PROTOCOL_ACK_START         0x55
#define PROTOCOL_GET_STATE_START   0x5A

#define PROTOCOL_STATE_SIZE        7
#define PROTOCOL_ACK_SIZE          4
#define PROTOCOL_GET_STATE_SIZE    3

#define PROTOCOL_ACK_TIMEOUT_MS    100
#define PROTOCOL_MAX_RETRIES       3
#define PROTOCOL_SYNC_INTERVAL_MS  5000

#define PROTOCOL_SWITCH_MASK       0x3F
#define PROTOCOL_BRIGHTNESS_MAX    100
#define PROTOCOL_WHITE_TEMP_MIN    3000
#define PROTOCOL_WHITE_TEMP_MAX    6500

typedef enum {
    PROTOCOL_ACK_OK = 0x00,
    PROTOCOL_ACK_INVALID_DATA = 0x01,
    PROTOCOL_ACK_UNSUPPORTED = 0x02,
    PROTOCOL_ACK_INTERNAL_ERROR = 0x03,
} protocol_ack_status_t;

typedef struct {
    uint8_t sequence;
    uint8_t switches;
    uint8_t brightness;
    uint16_t white_temperature;
} protocol_state_t;

typedef struct {
    uint8_t sequence;
    protocol_ack_status_t status;
} protocol_ack_t;

size_t protocol_encode_state(const protocol_state_t *state, uint8_t *buffer, size_t buffer_size);
bool protocol_decode_state(const uint8_t *buffer, size_t length, protocol_state_t *state);

size_t protocol_encode_ack(uint8_t sequence, protocol_ack_status_t status,
                           uint8_t *buffer, size_t buffer_size);
bool protocol_decode_ack(const uint8_t *buffer, size_t length, protocol_ack_t *ack);

size_t protocol_encode_get_state(uint8_t request_sequence, uint8_t *buffer, size_t buffer_size);
bool protocol_decode_get_state(const uint8_t *buffer, size_t length, uint8_t *request_sequence);
