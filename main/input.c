#include "input.h"
#include "pins.h"
#include "event_bus.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "fatal_error.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#define DEBOUNCE_MS            30
#define BUTTON_COUNT            9
#define INPUT_ISR_QUEUE_LENGTH 64
#define REPAIR_BUTTON_A         1
#define REPAIR_BUTTON_B         7
#define BOOT_BUTTON_ID          9

static const char *TAG = "input";

static const gpio_num_t s_button_pins[BUTTON_COUNT] = {
    PIN_BUTTON_1,
    PIN_BUTTON_2,
    PIN_BUTTON_3,
    PIN_BUTTON_4,
    PIN_BUTTON_5,
    PIN_BUTTON_6,
    PIN_BUTTON_7,
    PIN_ENCODER_BUTTON,
    PIN_BOOT_BUTTON,
};

typedef struct {
    bool stable_pressed;
    bool debounce_pending;
    TickType_t debounce_deadline;
} button_state_t;

typedef enum {
    RAW_INPUT_BUTTON_EDGE,
    RAW_INPUT_ENCODER_EDGE,
} raw_input_type_t;

typedef struct {
    raw_input_type_t type;
    uint8_t id;
} isr_pin_context_t;

typedef struct {
    raw_input_type_t type;
    uint8_t id;
    TickType_t tick;
} raw_input_event_t;

static button_state_t s_buttons[BUTTON_COUNT];
static QueueHandle_t s_raw_input_queue;
static uint8_t s_previous_encoder_ab = 0xFF;
static int8_t s_encoder_accumulator = 0;
static bool s_repair_combo_latched = false;

static const isr_pin_context_t s_button_isr_contexts[BUTTON_COUNT] = {
    { RAW_INPUT_BUTTON_EDGE, 1 },
    { RAW_INPUT_BUTTON_EDGE, 2 },
    { RAW_INPUT_BUTTON_EDGE, 3 },
    { RAW_INPUT_BUTTON_EDGE, 4 },
    { RAW_INPUT_BUTTON_EDGE, 5 },
    { RAW_INPUT_BUTTON_EDGE, 6 },
    { RAW_INPUT_BUTTON_EDGE, 7 },
    { RAW_INPUT_BUTTON_EDGE, 8 },
    { RAW_INPUT_BUTTON_EDGE, 9 },
};

static const isr_pin_context_t s_encoder_a_isr_context = { RAW_INPUT_ENCODER_EDGE, 0 };
static const isr_pin_context_t s_encoder_b_isr_context = { RAW_INPUT_ENCODER_EDGE, 0 };

static TickType_t ticks_from_ms_min_one(uint32_t ms)
{
    const TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks > 0 ? ticks : 1;
}

static void publish(input_event_type_t type, uint8_t id, int32_t value)
{
    input_event_t event = {
        .type = type,
        .input_id = id,
        .value = value,
        .tick = xTaskGetTickCount(),
    };

    if (!event_bus_publish(&event)) {
        ESP_LOGW(TAG, "Fronta událostí je plná");
    }
}

static bool tick_reached(TickType_t now, TickType_t deadline)
{
    return (int32_t)(now - deadline) >= 0;
}

static void publish_repair_request(uint8_t source_id, int32_t value)
{
    ESP_LOGW(TAG, "Manual Zigbee pairing requested source=%u value=%ld",
             source_id,
             (long)value);
    publish(INPUT_EVENT_ZIGBEE_REPAIR_REQUEST, source_id, value);
}

static void IRAM_ATTR input_gpio_isr_handler(void *arg)
{
    const isr_pin_context_t *context = (const isr_pin_context_t *)arg;
    const raw_input_event_t event = {
        .type = context->type,
        .id = context->id,
        .tick = xTaskGetTickCountFromISR(),
    };

    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_raw_input_queue != NULL) {
        (void)xQueueSendFromISR(s_raw_input_queue, &event, &higher_priority_task_woken);
    }

    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

static void process_button_edge(uint8_t button_id, TickType_t now)
{
    if (button_id < 1 || button_id > BUTTON_COUNT) {
        return;
    }

    button_state_t *state = &s_buttons[button_id - 1];
    state->debounce_pending = true;
    state->debounce_deadline = now + ticks_from_ms_min_one(DEBOUNCE_MS);
}

static void process_due_buttons(TickType_t now)
{
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        button_state_t *state = &s_buttons[i];
        if (!state->debounce_pending || !tick_reached(now, state->debounce_deadline)) {
            continue;
        }

        state->debounce_pending = false;
        const bool pressed = gpio_get_level(s_button_pins[i]) == 0;

        if (state->stable_pressed != pressed) {
            state->stable_pressed = pressed;
            const uint8_t button_id = (uint8_t)(i + 1);

            if (button_id == BOOT_BUTTON_ID) {
                if (pressed) {
                    publish_repair_request(button_id, 1);
                }
                continue;
            }

            publish(
                state->stable_pressed ? INPUT_EVENT_BUTTON_PRESSED
                                      : INPUT_EVENT_BUTTON_RELEASED,
                button_id,
                state->stable_pressed ? 1 : 0
            );

            if (button_id == REPAIR_BUTTON_A || button_id == REPAIR_BUTTON_B) {
                const bool combo_pressed =
                    s_buttons[REPAIR_BUTTON_A - 1].stable_pressed &&
                    s_buttons[REPAIR_BUTTON_B - 1].stable_pressed;
                if (combo_pressed && !s_repair_combo_latched) {
                    s_repair_combo_latched = true;
                    publish_repair_request(0, 0x0107);
                } else if (!combo_pressed) {
                    s_repair_combo_latched = false;
                }
            }
        }
    }
}

static TickType_t next_button_timeout(TickType_t now)
{
    bool has_pending = false;
    TickType_t nearest = 0;

    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        const button_state_t *state = &s_buttons[i];
        if (!state->debounce_pending) {
            continue;
        }

        if (tick_reached(now, state->debounce_deadline)) {
            return 0;
        }

        if (!has_pending || tick_reached(nearest, state->debounce_deadline)) {
            nearest = state->debounce_deadline;
            has_pending = true;
        }
    }

    return has_pending ? nearest - now : portMAX_DELAY;
}

static uint8_t read_encoder_ab(void)
{
    return ((uint8_t)gpio_get_level(PIN_ENCODER_A) << 1) |
           (uint8_t)gpio_get_level(PIN_ENCODER_B);
}

static void process_encoder_edge(void)
{
    static const int8_t transition_table[16] = {
         0, -1,  1,  0,
         1,  0,  0, -1,
        -1,  0,  0,  1,
         0,  1, -1,  0
    };

    const uint8_t current_ab = read_encoder_ab();

    if (s_previous_encoder_ab == 0xFF) {
        s_previous_encoder_ab = current_ab;
        return;
    }

    if (current_ab == s_previous_encoder_ab) {
        return;
    }

    const uint8_t index = (uint8_t)((s_previous_encoder_ab << 2) | current_ab);
    s_encoder_accumulator += transition_table[index & 0x0F];
    s_previous_encoder_ab = current_ab;

    if (s_encoder_accumulator >= 4) {
        s_encoder_accumulator = 0;
        publish(INPUT_EVENT_ENCODER_CW, 0, 1);
    } else if (s_encoder_accumulator <= -4) {
        s_encoder_accumulator = 0;
        publish(INPUT_EVENT_ENCODER_CCW, 0, -1);
    }
}

static void configure_light_sleep_wakeup(gpio_num_t pin)
{
    ESP_LOGD(TAG, "Light sleep GPIO wakeup temporarily disabled for GPIO%d", pin);
}

static void input_task(void *arg)
{
    (void)arg;
    raw_input_event_t raw_event;

    while (true) {
        TickType_t now = xTaskGetTickCount();
        const TickType_t timeout = next_button_timeout(now);

        if (xQueueReceive(s_raw_input_queue, &raw_event, timeout) == pdTRUE) {
            if (raw_event.type == RAW_INPUT_BUTTON_EDGE) {
                process_button_edge(raw_event.id, raw_event.tick);
            } else {
                process_encoder_edge();
            }
        }

        now = xTaskGetTickCount();
        process_due_buttons(now);
    }
}

void input_init(void)
{
    ESP_LOGI(TAG, "input_init start");
    s_raw_input_queue = xQueueCreate(INPUT_ISR_QUEUE_LENGTH, sizeof(raw_input_event_t));
    FATAL_ERROR_IF(s_raw_input_queue == NULL, "Nelze vytvořit frontu GPIO přerušení");

    uint64_t mask = 0;
    for (size_t i = 0; i < BUTTON_COUNT; ++i) {
        mask |= 1ULL << s_button_pins[i];
    }
    mask |= (1ULL << PIN_ENCODER_A) | (1ULL << PIN_ENCODER_B);

    const gpio_config_t config = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    ESP_LOGI(TAG, "input GPIO config mask=0x%llx", (unsigned long long)mask);
    FATAL_ERROR_CHECK(gpio_config(&config));

    ESP_LOGI(TAG, "install GPIO ISR service");
    esp_err_t err = gpio_install_isr_service(0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        fatal_error_restart(TAG, "gpio_install_isr_service", err);
    }

    ESP_LOGI(TAG, "add button GPIO ISR handlers");
    for (uint8_t i = 0; i < BUTTON_COUNT; ++i) {
        ESP_LOGI(TAG, "button%u GPIO%d init", (unsigned)(i + 1), s_button_pins[i]);
        const bool pressed = gpio_get_level(s_button_pins[i]) == 0;
        s_buttons[i].stable_pressed = pressed;
        s_buttons[i].debounce_pending = false;
        s_buttons[i].debounce_deadline = 0;
        FATAL_ERROR_CHECK(gpio_isr_handler_add(s_button_pins[i],
                                               input_gpio_isr_handler,
                                               (void *)&s_button_isr_contexts[i]));
        ESP_LOGI(TAG, "button%u GPIO%d ISR added", (unsigned)(i + 1), s_button_pins[i]);
        configure_light_sleep_wakeup(s_button_pins[i]);
        ESP_LOGI(TAG, "button%u GPIO%d wakeup configured", (unsigned)(i + 1), s_button_pins[i]);
    }

    ESP_LOGI(TAG, "add encoder GPIO ISR handlers");
    ESP_LOGI(TAG, "encoder A GPIO%d init", PIN_ENCODER_A);
    FATAL_ERROR_CHECK(gpio_isr_handler_add(PIN_ENCODER_A,
                                           input_gpio_isr_handler,
                                           (void *)&s_encoder_a_isr_context));
    ESP_LOGI(TAG, "encoder A GPIO%d ISR added", PIN_ENCODER_A);
    ESP_LOGI(TAG, "encoder B GPIO%d init", PIN_ENCODER_B);
    FATAL_ERROR_CHECK(gpio_isr_handler_add(PIN_ENCODER_B,
                                           input_gpio_isr_handler,
                                           (void *)&s_encoder_b_isr_context));
    ESP_LOGI(TAG, "encoder B GPIO%d ISR added", PIN_ENCODER_B);
    configure_light_sleep_wakeup(PIN_ENCODER_A);
    configure_light_sleep_wakeup(PIN_ENCODER_B);
    ESP_LOGI(TAG, "GPIO wakeup temporarily disabled");

    s_previous_encoder_ab = read_encoder_ab();
    s_encoder_accumulator = 0;

    ESP_LOGI(TAG, "Inicializováno 7 tlačítek, tlačítko encoderu, BOOT párovací tlačítko a encoder A/B přes GPIO přerušení");
    ESP_LOGI(TAG, "create input_task");
    BaseType_t created = xTaskCreate(input_task, "input_task", 4096, NULL, 10, NULL);
    if (created != pdPASS) {
        fatal_error_restart(TAG, "Nelze vytvořit input_task", ESP_ERR_NO_MEM);
    }
}
