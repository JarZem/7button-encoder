#include "serial_protocol.h"

static uint8_t xor_bytes(const uint8_t *buffer, size_t start, size_t end)
{
    uint8_t checksum = 0;
    for (size_t i = start; i <= end; ++i) {
        checksum ^= buffer[i];
    }
    return checksum;
}

static bool valid_state_values(const protocol_state_t *state)
{
    return state != NULL &&
           state->brightness <= PROTOCOL_BRIGHTNESS_MAX &&
           state->white_temperature >= PROTOCOL_WHITE_TEMP_MIN &&
           state->white_temperature <= PROTOCOL_WHITE_TEMP_MAX;
}

size_t protocol_encode_state(const protocol_state_t *state, uint8_t *buffer, size_t buffer_size)
{
    if (!valid_state_values(state) || buffer == NULL || buffer_size < PROTOCOL_STATE_SIZE) {
        return 0;
    }

    buffer[0] = PROTOCOL_STATE_START;
    buffer[1] = state->sequence;
    buffer[2] = state->switches & PROTOCOL_SWITCH_MASK;
    buffer[3] = state->brightness;
    buffer[4] = (uint8_t)(state->white_temperature & 0xFF);
    buffer[5] = (uint8_t)(state->white_temperature >> 8);
    buffer[6] = xor_bytes(buffer, 1, 5);

    return PROTOCOL_STATE_SIZE;
}

bool protocol_decode_state(const uint8_t *buffer, size_t length, protocol_state_t *state)
{
    if (buffer == NULL || state == NULL || length != PROTOCOL_STATE_SIZE) {
        return false;
    }
    if (buffer[0] != PROTOCOL_STATE_START || buffer[6] != xor_bytes(buffer, 1, 5)) {
        return false;
    }

    protocol_state_t decoded = {
        .sequence = buffer[1],
        .switches = buffer[2] & PROTOCOL_SWITCH_MASK,
        .brightness = buffer[3],
        .white_temperature = (uint16_t)buffer[4] | ((uint16_t)buffer[5] << 8),
    };
    if (!valid_state_values(&decoded)) {
        return false;
    }

    *state = decoded;
    return true;
}

size_t protocol_encode_ack(uint8_t sequence, protocol_ack_status_t status,
                           uint8_t *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < PROTOCOL_ACK_SIZE) {
        return 0;
    }

    buffer[0] = PROTOCOL_ACK_START;
    buffer[1] = sequence;
    buffer[2] = (uint8_t)status;
    buffer[3] = buffer[1] ^ buffer[2];

    return PROTOCOL_ACK_SIZE;
}

bool protocol_decode_ack(const uint8_t *buffer, size_t length, protocol_ack_t *ack)
{
    if (buffer == NULL || ack == NULL || length != PROTOCOL_ACK_SIZE) {
        return false;
    }
    if (buffer[0] != PROTOCOL_ACK_START || buffer[3] != (uint8_t)(buffer[1] ^ buffer[2])) {
        return false;
    }
    if (buffer[2] > PROTOCOL_ACK_INTERNAL_ERROR) {
        return false;
    }

    ack->sequence = buffer[1];
    ack->status = (protocol_ack_status_t)buffer[2];
    return true;
}

size_t protocol_encode_get_state(uint8_t request_sequence, uint8_t *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < PROTOCOL_GET_STATE_SIZE) {
        return 0;
    }

    buffer[0] = PROTOCOL_GET_STATE_START;
    buffer[1] = request_sequence;
    buffer[2] = request_sequence;

    return PROTOCOL_GET_STATE_SIZE;
}

bool protocol_decode_get_state(const uint8_t *buffer, size_t length, uint8_t *request_sequence)
{
    if (buffer == NULL || request_sequence == NULL || length != PROTOCOL_GET_STATE_SIZE) {
        return false;
    }
    if (buffer[0] != PROTOCOL_GET_STATE_START || buffer[2] != buffer[1]) {
        return false;
    }

    *request_sequence = buffer[1];
    return true;
}
