#include "status_led.h"

#include <stdbool.h>
#include <stdint.h>
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "led_strip.h"
#include "led_strip_rmt.h"
#include "ota_service.h"
#include "pins.h"

#define STATUS_LED_COUNT              1
#define STATUS_LED_TIMER_MS           100
#define STATUS_LED_RMT_RESOLUTION_HZ  10000000
#define STATUS_LED_PULSE_TICKS        2
#define STATUS_LED_PROVISION_TICKS    6
#define STATUS_LED_HEARTBEAT_TICKS    100

typedef struct {
    uint8_t red;
    uint8_t green;
    uint8_t blue;
} led_color_t;

typedef enum {
    PULSE_NONE,
    PULSE_BOOT_YELLOW,
    PULSE_LOCAL_GREEN,
    PULSE_HA_COMMAND_YELLOW,
    PULSE_HA_PUBLISH_GREEN,
    PULSE_HA_PUBLISH_YELLOW,
    PULSE_PROVISION_MAGENTA,
} pulse_kind_t;

static const char *TAG = "status_led";

static const led_color_t COLOR_OFF = {0, 0, 0};
static const led_color_t COLOR_GREEN = {0, 24, 0};
static const led_color_t COLOR_YELLOW = {24, 18, 0};
static const led_color_t COLOR_PURPLE = {18, 0, 24};
static const led_color_t COLOR_BLUE = {0, 0, 28};
static const led_color_t COLOR_CYAN = {0, 18, 24};
static const led_color_t COLOR_ORANGE = {28, 8, 0};
static const led_color_t COLOR_WHITE = {18, 18, 18};
static const led_color_t COLOR_RED = {28, 0, 0};
static const led_color_t COLOR_MAGENTA = {28, 0, 18};

static led_strip_handle_t s_strip;
static esp_timer_handle_t s_timer;
static SemaphoreHandle_t s_rmt_mutex;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static bool s_initialized;
static bool s_joined;
static bool s_pairing;
static bool s_failed;
static bool s_fatal;
static pulse_kind_t s_pulse = PULSE_NONE;
static uint8_t s_pulse_ticks;
static uint8_t s_ha_publish_step;
static uint32_t s_tick;

static void set_pixel(led_color_t color)
{
    if (s_strip == NULL || s_rmt_mutex == NULL) return;
    if (xSemaphoreTake(s_rmt_mutex, portMAX_DELAY) != pdTRUE) return;

    esp_err_t err = led_strip_set_pixel(s_strip, 0, color.red, color.green, color.blue);
    if (err == ESP_OK) err = led_strip_refresh(s_strip);

    xSemaphoreGive(s_rmt_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Status LED RMT update failed: %s", esp_err_to_name(err));
    }
}

static led_color_t ota_color_for_state(ota_state_t state)
{
    switch (state) {
        case OTA_STATE_PENDING: return COLOR_ORANGE;
        case OTA_STATE_CONNECTING_WIFI: return COLOR_CYAN;
        case OTA_STATE_DOWNLOADING: return COLOR_WHITE;
        case OTA_STATE_VERIFYING: return COLOR_BLUE;
        case OTA_STATE_SUCCESS: return COLOR_GREEN;
        case OTA_STATE_FAILED: return COLOR_RED;
        case OTA_STATE_IDLE:
        default: return COLOR_OFF;
    }
}

static bool ota_state_is_active(ota_state_t state)
{
    return state != OTA_STATE_IDLE;
}

static led_color_t pulse_color(pulse_kind_t pulse)
{
    switch (pulse) {
        case PULSE_BOOT_YELLOW:
        case PULSE_HA_COMMAND_YELLOW:
        case PULSE_HA_PUBLISH_YELLOW:
            return COLOR_YELLOW;
        case PULSE_LOCAL_GREEN:
        case PULSE_HA_PUBLISH_GREEN:
            return COLOR_GREEN;
        case PULSE_PROVISION_MAGENTA:
            return COLOR_MAGENTA;
        case PULSE_NONE:
        default:
            return COLOR_OFF;
    }
}

static uint8_t pulse_ticks_for_kind(pulse_kind_t pulse)
{
    return pulse == PULSE_PROVISION_MAGENTA ? STATUS_LED_PROVISION_TICKS : STATUS_LED_PULSE_TICKS;
}

static void timer_cb(void *arg)
{
    (void)arg;
    bool joined, pairing, failed, fatal;
    pulse_kind_t pulse;
    uint8_t pulse_ticks, ha_publish_step;
    uint32_t tick;

    portENTER_CRITICAL(&s_lock);
    joined = s_joined;
    pairing = s_pairing;
    failed = s_failed;
    fatal = s_fatal;
    pulse = s_pulse;
    pulse_ticks = s_pulse_ticks;
    ha_publish_step = s_ha_publish_step;
    tick = ++s_tick;

    if (s_pulse_ticks > 0) {
        --s_pulse_ticks;
        pulse_ticks = s_pulse_ticks;
        if (s_pulse_ticks == 0) {
            if (s_pulse == PULSE_HA_PUBLISH_GREEN && s_ha_publish_step == 1) {
                s_pulse = PULSE_HA_PUBLISH_YELLOW;
                s_pulse_ticks = STATUS_LED_PULSE_TICKS;
                s_ha_publish_step = 2;
                pulse = s_pulse;
                pulse_ticks = s_pulse_ticks;
                ha_publish_step = s_ha_publish_step;
            } else {
                s_pulse = PULSE_NONE;
                s_ha_publish_step = 0;
            }
        }
    }
    portEXIT_CRITICAL(&s_lock);

    (void)pulse_ticks;
    (void)ha_publish_step;

    if (fatal) { set_pixel((tick % 2) == 0 ? COLOR_RED : COLOR_OFF); return; }

    ota_state_t ota_state = ota_service_get_state();
    if (ota_state_is_active(ota_state)) {
        led_color_t color = ota_color_for_state(ota_state);
        set_pixel((tick % 2) == 0 ? color : COLOR_OFF);
        return;
    }

    if (pulse != PULSE_NONE) { set_pixel(pulse_color(pulse)); return; }
    if (failed) { set_pixel((tick % 6) < 3 ? COLOR_RED : COLOR_OFF); return; }
    if (pairing) { set_pixel((tick % 4) < 2 ? COLOR_BLUE : COLOR_OFF); return; }
    if (!joined) { set_pixel((tick % 6) < 3 ? COLOR_PURPLE : COLOR_OFF); return; }
    set_pixel((tick % STATUS_LED_HEARTBEAT_TICKS) == 0 ? COLOR_GREEN : COLOR_OFF);
}

static void start_pulse(pulse_kind_t pulse)
{
    portENTER_CRITICAL(&s_lock);
    s_pulse = pulse;
    s_pulse_ticks = pulse_ticks_for_kind(pulse);
    s_ha_publish_step = pulse == PULSE_HA_PUBLISH_GREEN ? 1 : 0;
    portEXIT_CRITICAL(&s_lock);
    set_pixel(pulse_color(pulse));
}

void status_led_init(void)
{
    if (s_initialized) return;

    s_rmt_mutex = xSemaphoreCreateMutex();
    if (s_rmt_mutex == NULL) {
        ESP_LOGW(TAG, "Status LED mutex allocation failed");
        return;
    }

    led_strip_config_t strip_config = {
        .strip_gpio_num = PIN_STATUS_LED,
        .max_leds = STATUS_LED_COUNT,
        .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
        .led_model = LED_MODEL_WS2812,
        .flags = {.invert_out = false},
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = STATUS_LED_RMT_RESOLUTION_HZ,
        .mem_block_symbols = 0,
        .flags = {.with_dma = false},
    };

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) { ESP_LOGW(TAG, "Status LED init failed: %s", esp_err_to_name(err)); return; }

    esp_timer_create_args_t timer_args = {.callback = timer_cb, .name = "status_led"};
    err = esp_timer_create(&timer_args, &s_timer);
    if (err != ESP_OK) { ESP_LOGW(TAG, "Status LED timer failed: %s", esp_err_to_name(err)); return; }

    set_pixel(COLOR_OFF);
    s_initialized = true;
    ESP_ERROR_CHECK(esp_timer_start_periodic(s_timer, STATUS_LED_TIMER_MS * 1000ULL));
    ESP_LOGI(TAG, "Status LED active on GPIO%d; serialized RMT enabled", PIN_STATUS_LED);
}

void status_led_indicate_boot(void) { start_pulse(PULSE_BOOT_YELLOW); }

void status_led_set_zigbee_joined(bool joined)
{
    portENTER_CRITICAL(&s_lock);
    s_joined = joined;
    if (joined) { s_pairing = false; s_failed = false; }
    portEXIT_CRITICAL(&s_lock);
}

void status_led_set_zigbee_pairing(bool pairing)
{
    portENTER_CRITICAL(&s_lock);
    s_pairing = pairing;
    if (pairing) { s_joined = false; s_failed = false; }
    portEXIT_CRITICAL(&s_lock);
}

void status_led_set_failure(bool failed)
{
    portENTER_CRITICAL(&s_lock);
    s_failed = failed;
    if (failed) s_pairing = false;
    portEXIT_CRITICAL(&s_lock);
}

void status_led_indicate_local_activity(void) { start_pulse(PULSE_LOCAL_GREEN); }
void status_led_indicate_ha_command(void) { start_pulse(PULSE_HA_COMMAND_YELLOW); }
void status_led_indicate_ha_publish(void) { start_pulse(PULSE_HA_PUBLISH_GREEN); }
void status_led_indicate_provision_step(void) { start_pulse(PULSE_PROVISION_MAGENTA); }

void status_led_fatal(void)
{
    portENTER_CRITICAL(&s_lock);
    s_fatal = true;
    portEXIT_CRITICAL(&s_lock);
}
