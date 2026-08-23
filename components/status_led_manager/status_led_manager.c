#include "status_led_manager.h"

#include <string.h>
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "led_strip_rmt.h"

#define FIFO_LEN 16

typedef struct { uint8_t r, g, b; } rgb_t;
typedef struct { uint8_t color; uint16_t ticks; } pulse_t;
typedef struct {
    bool active;
    uint8_t color;
    uint16_t blink_period_ms;
    uint8_t priority;
} mode_t;

static const char *TAG = "status_led_mgr";
static const rgb_t COLORS[STATUS_LED_COLOR_TABLE_SIZE] = {
    [STATUS_LED_COLOR_OFF]     = {0, 0, 0},
    [STATUS_LED_COLOR_GREEN]   = {0, 24, 0},
    [STATUS_LED_COLOR_YELLOW]  = {24, 18, 0},
    [STATUS_LED_COLOR_PURPLE]  = {18, 0, 24},
    [STATUS_LED_COLOR_BLUE]    = {0, 0, 28},
    [STATUS_LED_COLOR_CYAN]    = {0, 18, 24},
    [STATUS_LED_COLOR_ORANGE]  = {28, 8, 0},
    [STATUS_LED_COLOR_WHITE]   = {18, 18, 18},
    [STATUS_LED_COLOR_RED]     = {28, 0, 0},
    [STATUS_LED_COLOR_MAGENTA] = {28, 0, 18},
};

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_heartbeat_timer;
static esp_timer_handle_t s_scheduler_timer;
static SemaphoreHandle_t s_io_mutex;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static status_led_manager_config_t s_cfg;
static pulse_t s_fifo[FIFO_LEN];
static uint8_t s_head, s_count;
static pulse_t s_current;
static uint8_t s_gap_ticks;
static mode_t s_modes[STATUS_LED_MODE_SLOTS];
static uint32_t s_scheduler_tick;
static uint16_t s_heartbeat_ticks;
static bool s_heartbeat_due;
static uint16_t s_block_count;
static uint8_t s_rendered_color = 0xff;
static bool s_initialized;

static uint16_t ms_to_ticks(uint32_t ms)
{
    const uint32_t period = s_cfg.scheduler_period_ms ? s_cfg.scheduler_period_ms : 50;
    uint32_t ticks = (ms + period - 1) / period;
    if (ticks == 0) ticks = 1;
    if (ticks > UINT16_MAX) ticks = UINT16_MAX;
    return (uint16_t)ticks;
}

static void heartbeat_cb(void *arg)
{
    (void)arg;
    portENTER_CRITICAL(&s_lock);
    s_heartbeat_due = true;
    portEXIT_CRITICAL(&s_lock);
}

static bool select_mode(mode_t *selected)
{
    bool found = false;
    memset(selected, 0, sizeof(*selected));

    portENTER_CRITICAL(&s_lock);
    for (unsigned i = 0; i < STATUS_LED_MODE_SLOTS; ++i) {
        if (s_modes[i].active && (!found || s_modes[i].priority > selected->priority)) {
            *selected = s_modes[i];
            found = true;
        }
    }
    portEXIT_CRITICAL(&s_lock);

    if (s_cfg.external_state_cb) {
        status_led_external_state_t ext = {0};
        if (s_cfg.external_state_cb(&ext, s_cfg.external_state_ctx) && ext.active &&
            (!found || ext.priority > selected->priority)) {
            selected->active = true;
            selected->color = ext.color;
            selected->blink_period_ms = ext.blink_period_ms;
            selected->priority = ext.priority;
            found = true;
        }
    }
    return found;
}

static uint8_t mode_color(const mode_t *mode)
{
    if (!mode->active) return STATUS_LED_COLOR_OFF;
    if (mode->blink_period_ms == 0) return mode->color;
    uint32_t period_ticks = ms_to_ticks(mode->blink_period_ms);
    if (period_ticks < 2) period_ticks = 2;
    uint32_t half = period_ticks / 2;
    return (s_scheduler_tick % period_ticks) < half ? mode->color : STATUS_LED_COLOR_OFF;
}

static uint8_t next_nonpriority_color(void)
{
    uint8_t color = STATUS_LED_COLOR_OFF;
    portENTER_CRITICAL(&s_lock);

    if (s_gap_ticks > 0) {
        --s_gap_ticks;
        portEXIT_CRITICAL(&s_lock);
        return STATUS_LED_COLOR_OFF;
    }

    if (s_current.ticks == 0 && s_count > 0) {
        s_current = s_fifo[s_head];
        s_head = (uint8_t)((s_head + 1) % FIFO_LEN);
        --s_count;
    }

    if (s_current.ticks > 0) {
        color = s_current.color;
        --s_current.ticks;
        if (s_current.ticks == 0) s_gap_ticks = 1;
    } else {
        if (s_heartbeat_ticks == 0 && s_heartbeat_due) {
            s_heartbeat_due = false;
            s_heartbeat_ticks = ms_to_ticks(s_cfg.heartbeat_pulse_ms);
        }
        if (s_heartbeat_ticks > 0) {
            color = s_cfg.heartbeat_color;
            --s_heartbeat_ticks;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return color;
}

static void render_color(uint8_t color)
{
    if (color >= STATUS_LED_COLOR_TABLE_SIZE || !s_strip || !s_io_mutex) return;
    if (xSemaphoreTake(s_io_mutex, 0) != pdTRUE) return;

    bool blocked;
    portENTER_CRITICAL(&s_lock);
    blocked = s_block_count != 0;
    portEXIT_CRITICAL(&s_lock);
    if (blocked) {
        xSemaphoreGive(s_io_mutex);
        return;
    }

    if (color != s_rendered_color) {
        const rgb_t rgb = COLORS[color];
        esp_err_t err = led_strip_set_pixel(s_strip, 0, rgb.r, rgb.g, rgb.b);
        if (err == ESP_OK) err = led_strip_refresh(s_strip);
        if (err == ESP_OK) {
            s_rendered_color = color;
        } else {
            ESP_LOGW(TAG, "LED RMT refresh failed: %s", esp_err_to_name(err));
        }
    }
    xSemaphoreGive(s_io_mutex);
}

static void scheduler_cb(void *arg)
{
    (void)arg;
    ++s_scheduler_tick;
    mode_t mode;
    const uint8_t color = select_mode(&mode) ? mode_color(&mode) : next_nonpriority_color();
    render_color(color);
}

esp_err_t status_led_manager_init(const status_led_manager_config_t *config)
{
    if (!config || config->gpio_num < 0) return ESP_ERR_INVALID_ARG;
    if (s_initialized) return ESP_OK;
    s_cfg = *config;
    if (!s_cfg.scheduler_period_ms) s_cfg.scheduler_period_ms = 50;
    if (!s_cfg.heartbeat_period_ms) s_cfg.heartbeat_period_ms = 10000;
    if (!s_cfg.heartbeat_pulse_ms) s_cfg.heartbeat_pulse_ms = 150;
    if (!s_cfg.rmt_resolution_hz) s_cfg.rmt_resolution_hz = 10000000;
    if (s_cfg.heartbeat_color <= STATUS_LED_COLOR_OFF || s_cfg.heartbeat_color >= STATUS_LED_COLOR_COUNT)
        s_cfg.heartbeat_color = STATUS_LED_COLOR_GREEN;

    s_io_mutex = xSemaphoreCreateMutex();
    if (!s_io_mutex) return ESP_ERR_NO_MEM;

    led_strip_config_t strip_cfg = {
        .strip_gpio_num = s_cfg.gpio_num,
        .max_leds = 1,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_cfg = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = s_cfg.rmt_resolution_hz,
        .mem_block_symbols = 0,
        .flags = {.with_dma = false},
    };
    esp_err_t err = led_strip_new_rmt_device(&strip_cfg, &rmt_cfg, &s_strip);
    if (err != ESP_OK) return err;

    esp_timer_create_args_t hb_args = {.callback = heartbeat_cb, .name = "led_heartbeat"};
    esp_timer_create_args_t sched_args = {.callback = scheduler_cb, .name = "led_scheduler"};
    if ((err = esp_timer_create(&hb_args, &s_heartbeat_timer)) != ESP_OK) return err;
    if ((err = esp_timer_create(&sched_args, &s_scheduler_timer)) != ESP_OK) return err;

    s_initialized = true;
    render_color(STATUS_LED_COLOR_OFF);
    if ((err = esp_timer_start_periodic(s_scheduler_timer, (uint64_t)s_cfg.scheduler_period_ms * 1000ULL)) != ESP_OK) return err;
    if ((err = esp_timer_start_periodic(s_heartbeat_timer, (uint64_t)s_cfg.heartbeat_period_ms * 1000ULL)) != ESP_OK) return err;
    ESP_LOGI(TAG, "portable LED manager active gpio=%d fifo=%u colors=%u heartbeat=%lums scheduler=%lums",
             s_cfg.gpio_num, FIFO_LEN, STATUS_LED_COLOR_TABLE_SIZE,
             (unsigned long)s_cfg.heartbeat_period_ms,
             (unsigned long)s_cfg.scheduler_period_ms);
    return ESP_OK;
}

void status_led_manager_enqueue(status_led_color_id_t color, uint16_t duration_ms)
{
    if (!s_initialized || color <= STATUS_LED_COLOR_OFF || color >= STATUS_LED_COLOR_TABLE_SIZE) return;
    const pulse_t item = {.color = (uint8_t)color, .ticks = ms_to_ticks(duration_ms)};
    portENTER_CRITICAL(&s_lock);
    if (s_count == FIFO_LEN) {
        s_head = (uint8_t)((s_head + 1) % FIFO_LEN);
        --s_count;
    }
    const uint8_t tail = (uint8_t)((s_head + s_count) % FIFO_LEN);
    s_fifo[tail] = item;
    ++s_count;
    portEXIT_CRITICAL(&s_lock);
}

void status_led_manager_set_mode(uint8_t slot, bool active, status_led_color_id_t color,
                                 uint16_t blink_period_ms, uint8_t priority)
{
    if (slot >= STATUS_LED_MODE_SLOTS) return;
    if (color >= STATUS_LED_COLOR_TABLE_SIZE) color = STATUS_LED_COLOR_OFF;
    portENTER_CRITICAL(&s_lock);
    s_modes[slot].active = active;
    s_modes[slot].color = (uint8_t)color;
    s_modes[slot].blink_period_ms = blink_period_ms;
    s_modes[slot].priority = priority;
    portEXIT_CRITICAL(&s_lock);
}

void status_led_manager_block(void)
{
    if (!s_initialized || !s_io_mutex) return;
    if (xSemaphoreTake(s_io_mutex, portMAX_DELAY) != pdTRUE) return;
    portENTER_CRITICAL(&s_lock);
    if (s_block_count != UINT16_MAX) ++s_block_count;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreGive(s_io_mutex);
}

void status_led_manager_unblock(void)
{
    if (!s_initialized) return;
    portENTER_CRITICAL(&s_lock);
    if (s_block_count > 0) --s_block_count;
    portEXIT_CRITICAL(&s_lock);
}
