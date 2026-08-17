#include "uart_transport.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "driver/uart.h"
#include "esp_log.h"
#include "fatal_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "pins.h"
#include "serial_protocol.h"
#include "zigbee.h"

#define UART_TRANSPORT_NUM          UART_NUM_1
#define UART_TRANSPORT_BAUD_RATE    115200
#define UART_TRANSPORT_RX_BUF_SIZE  256
#define UART_TRANSPORT_TX_BUF_SIZE  256
#define UART_TRANSPORT_TASK_STACK   4096
#define UART_TRANSPORT_TASK_PRIO    8
#define UART_TRANSPORT_BOOT_DELAY_MS 250
#define UART_PARSER_FRAME_TIMEOUT_MS 50

typedef enum {
    PARSED_FRAME_NONE,
    PARSED_FRAME_ACK,
    PARSED_FRAME_GET_STATE,
    PARSED_FRAME_STATE,
} parsed_frame_type_t;

typedef struct {
    parsed_frame_type_t type;
    protocol_ack_t ack;
    uint8_t get_state_sequence;
    uint8_t state_sequence;
    protocol_ack_status_t state_status;
    bool state_checksum_valid;
} parsed_frame_t;

typedef struct {
    uint8_t buffer[PROTOCOL_STATE_SIZE];
    size_t length;
    size_t expected_length;
    TickType_t started_at;
} parser_state_t;

static const char *TAG = "uart_transport";

static QueueHandle_t s_state_queue;
static device_state_t s_latest_state;
static bool s_has_latest_state;
static bool s_first_send = true;
static bool s_force_send;
static uint8_t s_next_sequence;
static uart_transport_diagnostics_t s_diagnostics;

static uint8_t xor_range(const uint8_t *buffer, size_t start, size_t end)
{
    uint8_t checksum = 0;
    for (size_t i = start; i <= end; ++i) {
        checksum ^= buffer[i];
    }
    return checksum;
}

static size_t expected_length_for_start(uint8_t start)
{
    switch (start) {
        case PROTOCOL_STATE_START:
            return PROTOCOL_STATE_SIZE;
        case PROTOCOL_ACK_START:
            return PROTOCOL_ACK_SIZE;
        case PROTOCOL_GET_STATE_START:
            return PROTOCOL_GET_STATE_SIZE;
        default:
            return 0;
    }
}

static void parser_reset(parser_state_t *parser)
{
    parser->length = 0;
    parser->expected_length = 0;
}

static void handle_completed_frame(parser_state_t *parser, parsed_frame_t *parsed)
{
    parsed->type = PARSED_FRAME_NONE;

    if (parser->buffer[0] == PROTOCOL_ACK_START) {
        if (protocol_decode_ack(parser->buffer, parser->expected_length, &parsed->ack)) {
            parsed->type = PARSED_FRAME_ACK;
        } else {
            s_diagnostics.checksum_error_count++;
        }
        return;
    }

    if (parser->buffer[0] == PROTOCOL_GET_STATE_START) {
        if (protocol_decode_get_state(parser->buffer, parser->expected_length,
                                      &parsed->get_state_sequence)) {
            parsed->type = PARSED_FRAME_GET_STATE;
        } else {
            s_diagnostics.checksum_error_count++;
        }
        return;
    }

    if (parser->buffer[0] == PROTOCOL_STATE_START) {
        parsed->state_sequence = parser->buffer[1];
        parsed->state_checksum_valid = parser->buffer[6] == xor_range(parser->buffer, 1, 5);
        if (!parsed->state_checksum_valid) {
            s_diagnostics.checksum_error_count++;
            return;
        }

        const uint8_t brightness = parser->buffer[3];
        const uint16_t white_temperature = (uint16_t)parser->buffer[4] |
                                           ((uint16_t)parser->buffer[5] << 8);
        if (brightness > PROTOCOL_BRIGHTNESS_MAX ||
            white_temperature < PROTOCOL_WHITE_TEMP_MIN ||
            white_temperature > PROTOCOL_WHITE_TEMP_MAX) {
            parsed->type = PARSED_FRAME_STATE;
            parsed->state_status = PROTOCOL_ACK_INVALID_DATA;
            return;
        }

        parsed->type = PARSED_FRAME_STATE;
        parsed->state_status = PROTOCOL_ACK_UNSUPPORTED;
    }
}

static bool parser_feed(parser_state_t *parser, uint8_t byte, parsed_frame_t *parsed)
{
    const TickType_t now = xTaskGetTickCount();
    parsed->type = PARSED_FRAME_NONE;

    if (parser->length > 0 &&
        (now - parser->started_at) > pdMS_TO_TICKS(UART_PARSER_FRAME_TIMEOUT_MS)) {
        parser_reset(parser);
    }

    if (parser->length == 0) {
        const size_t expected = expected_length_for_start(byte);
        if (expected == 0 || expected > sizeof(parser->buffer)) {
            return false;
        }
        parser->buffer[0] = byte;
        parser->length = 1;
        parser->expected_length = expected;
        parser->started_at = now;
        return false;
    }

    if (parser->length >= sizeof(parser->buffer)) {
        parser_reset(parser);
        s_diagnostics.checksum_error_count++;
        return false;
    }

    parser->buffer[parser->length++] = byte;
    if (parser->length < parser->expected_length) {
        return false;
    }

    handle_completed_frame(parser, parsed);
    parser_reset(parser);
    return parsed->type != PARSED_FRAME_NONE;
}

static void send_ack(uint8_t sequence, protocol_ack_status_t status)
{
    uint8_t frame[PROTOCOL_ACK_SIZE];
    const size_t length = protocol_encode_ack(sequence, status, frame, sizeof(frame));
    if (length == PROTOCOL_ACK_SIZE) {
        uart_write_bytes(UART_TRANSPORT_NUM, frame, length);
    }
}

static void poll_uart(parser_state_t *parser, parsed_frame_t *parsed)
{
    uint8_t byte;
    while (uart_read_bytes(UART_TRANSPORT_NUM, &byte, 1, 0) == 1) {
        parsed_frame_t frame;
        if (!parser_feed(parser, byte, &frame)) {
            continue;
        }

        if (frame.type == PARSED_FRAME_GET_STATE) {
            s_force_send = true;
        } else if (frame.type == PARSED_FRAME_STATE) {
            send_ack(frame.state_sequence, frame.state_status);
        } else if (frame.type == PARSED_FRAME_ACK) {
            *parsed = frame;
            return;
        }
    }

    parsed->type = PARSED_FRAME_NONE;
}

static bool wait_for_ack(parser_state_t *parser, uint8_t sequence)
{
    const TickType_t started_at = xTaskGetTickCount();
    const TickType_t timeout = pdMS_TO_TICKS(PROTOCOL_ACK_TIMEOUT_MS);

    while ((xTaskGetTickCount() - started_at) < timeout) {
        parsed_frame_t parsed;
        poll_uart(parser, &parsed);
        if (parsed.type == PARSED_FRAME_ACK && parsed.ack.sequence == sequence) {
            s_diagnostics.received_ack_count++;
            s_diagnostics.last_ack_sequence = sequence;
            if (parsed.ack.status == PROTOCOL_ACK_OK) {
                return true;
            }
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }

    return false;
}

static bool send_state_with_ack(parser_state_t *parser, const device_state_t *state)
{
    const uint8_t sequence = s_next_sequence++;
    protocol_state_t protocol_state = {
        .sequence = sequence,
        .switches = state->switches & DEVICE_SWITCH_MASK,
        .brightness = state->brightness,
        .white_temperature = state->white_temperature,
    };
    uint8_t frame[PROTOCOL_STATE_SIZE];
    const size_t length = protocol_encode_state(&protocol_state, frame, sizeof(frame));
    if (length != PROTOCOL_STATE_SIZE) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < PROTOCOL_MAX_RETRIES; ++attempt) {
        if (attempt > 0) {
            s_diagnostics.retry_count++;
        }

        uart_write_bytes(UART_TRANSPORT_NUM, frame, length);
        s_diagnostics.sent_frame_count++;

        if (wait_for_ack(parser, sequence)) {
            if (s_diagnostics.communication_error) {
                zigbee_publish_communication_error(false, &s_diagnostics);
            }
            s_diagnostics.communication_error = false;
            s_diagnostics.consecutive_failures = 0;
            return true;
        }
    }

    s_diagnostics.communication_error = true;
    s_diagnostics.consecutive_failures++;
    zigbee_publish_communication_error(true, &s_diagnostics);
    return false;
}

static void uart_transport_task(void *arg)
{
    (void)arg;

    parser_state_t parser = { 0 };
    TickType_t next_sync = xTaskGetTickCount() + pdMS_TO_TICKS(PROTOCOL_SYNC_INTERVAL_MS);

    while (true) {
        device_state_t queued_state;
        if (xQueueReceive(s_state_queue, &queued_state, pdMS_TO_TICKS(20)) == pdTRUE) {
            s_latest_state = queued_state;
            s_has_latest_state = true;
            s_force_send = true;
        }

        parsed_frame_t parsed;
        poll_uart(&parser, &parsed);

        const TickType_t now = xTaskGetTickCount();
        if (s_has_latest_state && s_latest_state.rs232_enabled &&
            (s_force_send || now >= next_sync)) {
            if (s_first_send) {
                vTaskDelay(pdMS_TO_TICKS(UART_TRANSPORT_BOOT_DELAY_MS));
                s_first_send = false;
            }

            s_force_send = false;
            send_state_with_ack(&parser, &s_latest_state);
            next_sync = xTaskGetTickCount() + pdMS_TO_TICKS(PROTOCOL_SYNC_INTERVAL_MS);
        }
    }
}

void uart_transport_init(void)
{
    const uart_config_t uart_config = {
        .baud_rate = UART_TRANSPORT_BAUD_RATE,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    FATAL_ERROR_CHECK(uart_driver_install(UART_TRANSPORT_NUM,
                                          UART_TRANSPORT_RX_BUF_SIZE,
                                          UART_TRANSPORT_TX_BUF_SIZE,
                                          0,
                                          NULL,
                                          0));
    FATAL_ERROR_CHECK(uart_param_config(UART_TRANSPORT_NUM, &uart_config));
    FATAL_ERROR_CHECK(uart_set_pin(UART_TRANSPORT_NUM,
                                   PIN_FUTURE_UART_TX,
                                   PIN_FUTURE_UART_RX,
                                   UART_PIN_NO_CHANGE,
                                   UART_PIN_NO_CHANGE));

    s_state_queue = xQueueCreate(1, sizeof(device_state_t));
    FATAL_ERROR_IF(s_state_queue == NULL, "Nelze vytvořit frontu UART transportu");

    BaseType_t created = xTaskCreate(uart_transport_task,
                                     "uart_transport",
                                     UART_TRANSPORT_TASK_STACK,
                                     NULL,
                                     UART_TRANSPORT_TASK_PRIO,
                                     NULL);
    if (created != pdPASS) {
        fatal_error_restart(TAG, "Nelze vytvořit uart_transport task", ESP_ERR_NO_MEM);
    }

    ESP_LOGI(TAG, "UART transport inicializován na GPIO%d/GPIO%d",
             PIN_FUTURE_UART_TX,
             PIN_FUTURE_UART_RX);
}

void uart_transport_send_state(const device_state_t *state)
{
    if (state == NULL || s_state_queue == NULL) {
        return;
    }

    xQueueOverwrite(s_state_queue, state);
}

void uart_transport_get_diagnostics(uart_transport_diagnostics_t *diagnostics)
{
    if (diagnostics != NULL) {
        *diagnostics = s_diagnostics;
    }
}
