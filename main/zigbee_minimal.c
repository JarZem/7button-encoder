#include "zigbee_minimal.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "sdkconfig.h"
#include "event_bus.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nwk/esp_zigbee_nwk.h"
#include "status_led.h"
#include "storage.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zigbee_ota_cluster.h"

#if !defined ZB_ED_ROLE
#error "Minimal Zigbee diagnostic firmware must be built as Zigbee End Device."
#endif

static const char *TAG = "zb_minimal";

#define ZB_FIRST_SWITCH_ENDPOINT 1
#define ZB_ENDPOINT_COUNT        9
#define ZB_SWITCH_COUNT          6
#define ZB_MIN_DEFAULT_CHANNEL   CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL
#define ZB_MIN_SCAN_CHANNEL_MASK CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK
#define ZB_MIN_FAST_ATTEMPTS     CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS
#define ZB_MIN_ED_AGING_TIMEOUT ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ZB_MIN_ED_KEEP_ALIVE_MS  3000
#define ZB_MIN_INSTALL_CODE      false
#define ZB_MIN_TASK_STACK        4096
#define ZB_MIN_TASK_PRIORITY     5
#define ZB_MIN_MANUFACTURER_LEN  5
#define ZB_MIN_MODEL_LEN         21
#define ZB_MIN_SW_BUILD_LEN      5
#define ZB_MIN_MANUFACTURER_TEXT "Jaros"
#define ZB_MIN_MODEL_TEXT        "RemoteControl7Encoder"
#define ZB_MIN_SW_BUILD_TEXT     "1.0.0"
#define ZB_MIN_REPORT_DELAY_MS   100
#define ZB_LIGHT_ENDPOINT        9
#define ZB_BRIGHTNESS_MIN        0
#define ZB_BRIGHTNESS_MAX        100
#define ZB_BRIGHTNESS_DEFAULT    100
#define ZB_WHITE_TEMP_MIN_K      3000
#define ZB_WHITE_TEMP_MAX_K      6500
#define ZB_WHITE_TEMP_DEFAULT_K  4000
#define ZB_MIN_MIREDS            154
#define ZB_MAX_MIREDS            333
#define ZB_COLOR_TEMP_CAPABILITY (1U << 4)
static bool s_joined;
static bool s_stack_started;
static bool s_manual_repair_requested;
static uint8_t s_fast_pair_channel;
static uint8_t s_steering_fail_count;
static uint32_t s_current_channel_mask;
static bool s_endpoint_on[ZB_ENDPOINT_COUNT];
static uint8_t s_brightness_percent = ZB_BRIGHTNESS_DEFAULT;
static uint16_t s_white_temperature_kelvin = ZB_WHITE_TEMP_DEFAULT_K;
static uint8_t s_level_current = 254;
static uint16_t s_color_temperature_mired = 250;
static uint16_t s_color_temperature_min_mired = ZB_MIN_MIREDS;
static uint16_t s_color_temperature_max_mired = ZB_MAX_MIREDS;
static uint16_t s_color_temperature_startup_mired = 250;
static uint8_t s_on_off_report_generation[ZB_ENDPOINT_COUNT];
static uint8_t s_level_report_generation;
static uint8_t s_color_temperature_report_generation;
static const uint8_t s_switch_endpoints[ZB_ENDPOINT_COUNT] = {1, 2, 3, 4, 5, 6, 7, 8, 9};

static void schedule_on_off_report(uint8_t endpoint);
static void schedule_on_off_report_delay(uint8_t endpoint, uint32_t delay_ms);
static void schedule_level_report(uint32_t delay_ms);
static void schedule_color_temperature_report(uint32_t delay_ms);

static uint16_t read_u16_le(const void *data)
{
    const uint8_t *bytes = (const uint8_t *)data;
    return (uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8);
}

static bool zigbee_channel_is_valid(uint8_t channel)
{
    return channel >= 11 && channel <= 26;
}

static uint32_t zigbee_channel_mask(uint8_t channel)
{
    return zigbee_channel_is_valid(channel) ? (1UL << channel) : 0;
}

static uint32_t zigbee_configured_scan_mask(void)
{
    uint32_t mask = ZB_MIN_SCAN_CHANNEL_MASK;
    if (mask == 0) {
        mask = 0x07FFF800UL;
    }
    return mask;
}

static void apply_steering_channel_mask(uint32_t mask, const char *reason)
{
    if (mask == 0) {
        mask = zigbee_configured_scan_mask();
    }
    s_current_channel_mask = mask;
    esp_zb_set_primary_network_channel_set(mask);
    ESP_LOGI(TAG, "Zigbee steering channel mask=%s 0x%08lx fast_channel=%u fallback_mask=0x%08lx attempts=%u",
             reason != NULL ? reason : "set",
             (unsigned long)s_current_channel_mask,
             s_fast_pair_channel,
             (unsigned long)zigbee_configured_scan_mask(),
             ZB_MIN_FAST_ATTEMPTS);
}

static void load_fast_pair_channel(void)
{
    uint8_t stored_channel = 0;
    if (storage_load_zigbee_last_channel(&stored_channel) &&
        zigbee_channel_is_valid(stored_channel)) {
        s_fast_pair_channel = stored_channel;
        ESP_LOGI(TAG, "Zigbee fast-pair channel from NVS: %u", s_fast_pair_channel);
        return;
    }

    s_fast_pair_channel = ZB_MIN_DEFAULT_CHANNEL;
    if (!zigbee_channel_is_valid(s_fast_pair_channel)) {
        s_fast_pair_channel = 11;
    }
    ESP_LOGI(TAG, "Zigbee fast-pair channel from build config: %u", s_fast_pair_channel);
}

static void start_network_steering_now(const char *reason)
{
    ESP_LOGI(TAG, "start network steering reason=%s mask=0x%08lx fail_count=%u/%u",
             reason != NULL ? reason : "unknown",
             (unsigned long)s_current_channel_mask,
             s_steering_fail_count,
             ZB_MIN_FAST_ATTEMPTS);
    status_led_set_failure(false);
    status_led_set_zigbee_pairing(true);
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_zb_bdb_start_top_level_commissioning failed: %s", esp_err_to_name(err));
        status_led_set_failure(true);
    }
}

#define ESP_ZB_MIN_ZED_CONFIG()                                      \
    {                                                                \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                        \
        .install_code_policy = ZB_MIN_INSTALL_CODE,                  \
        .nwk_cfg.zed_cfg = {                                         \
            .ed_timeout = ZB_MIN_ED_AGING_TIMEOUT,                   \
            .keep_alive = ZB_MIN_ED_KEEP_ALIVE_MS,                   \
        },                                                           \
    }

#define ESP_ZB_MIN_RADIO_CONFIG()                                    \
    {                                                                \
        .radio_mode = ZB_RADIO_MODE_NATIVE,                          \
    }

#define ESP_ZB_MIN_HOST_CONFIG()                                     \
    {                                                                \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE,         \
    }

static void log_network_state(const char *prefix)
{
    esp_zb_ieee_addr_t extended_pan_id = {0};
    esp_zb_get_extended_pan_id(extended_pan_id);

    ESP_LOGI(TAG,
             "%s: factory_new=%d channel=%u short=0x%04hx pan=0x%04hx ext_pan=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x free_heap=%lu",
             prefix,
             esp_zb_bdb_is_factory_new(),
             esp_zb_get_current_channel(),
             esp_zb_get_short_address(),
             esp_zb_get_pan_id(),
             extended_pan_id[7], extended_pan_id[6], extended_pan_id[5], extended_pan_id[4],
             extended_pan_id[3], extended_pan_id[2], extended_pan_id[1], extended_pan_id[0],
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_DEFAULT));
}

static void start_steering_cb(uint8_t mode_mask)
{
    (void)mode_mask;
    start_network_steering_now("retry");
}

static void manual_repair_cb(uint8_t unused)
{
    (void)unused;

    status_led_set_failure(false);
    status_led_set_zigbee_joined(false);
    status_led_set_zigbee_pairing(true);

    if (esp_zb_bdb_is_factory_new()) {
        ESP_LOGW(TAG, "manual re-pair: device is factory-new, start network steering now");
        s_steering_fail_count = 0;
        apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "manual fast");
        start_network_steering_now("manual");
        return;
    }

    ESP_LOGW(TAG, "manual re-pair: factory reset Zigbee storage and reboot");
    esp_zb_factory_reset();
}

static int endpoint_to_index(uint8_t endpoint)
{
    for (size_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        if (s_switch_endpoints[i] == endpoint) {
            return (int)i;
        }
    }
    return -1;
}

static uint8_t percent_to_level(uint8_t percent)
{
    if (percent > ZB_BRIGHTNESS_MAX) {
        percent = ZB_BRIGHTNESS_MAX;
    }
    return (uint8_t)(((uint16_t)percent * 254U + 50U) / 100U);
}

static uint8_t level_to_percent(uint8_t level)
{
    return (uint8_t)(((uint16_t)level * 100U + 127U) / 254U);
}

static uint16_t kelvin_to_mired(uint16_t kelvin)
{
    if (kelvin < ZB_WHITE_TEMP_MIN_K) {
        kelvin = ZB_WHITE_TEMP_MIN_K;
    } else if (kelvin > ZB_WHITE_TEMP_MAX_K) {
        kelvin = ZB_WHITE_TEMP_MAX_K;
    }
    return (uint16_t)((1000000UL + (kelvin / 2U)) / kelvin);
}

static uint16_t mired_to_kelvin(uint16_t mired)
{
    if (mired < ZB_MIN_MIREDS) {
        mired = ZB_MIN_MIREDS;
    } else if (mired > ZB_MAX_MIREDS) {
        mired = ZB_MAX_MIREDS;
    }
    return (uint16_t)((1000000UL + (mired / 2U)) / mired);
}

static void publish_input_event(input_event_type_t type, uint8_t input_id, int32_t value)
{
    const input_event_t event = {
        .type = type,
        .input_id = input_id,
        .value = value,
        .tick = xTaskGetTickCount(),
    };

    if (!event_bus_publish(&event)) {
        ESP_LOGW(TAG, "event queue full for Zigbee command type=%d id=%u value=%ld",
                 type,
                 input_id,
                 (long)value);
    }
}

static esp_zb_zcl_status_t set_zcl_attr_locked(uint8_t endpoint, uint16_t cluster_id,
                                               uint16_t attr_id, void *value)
{
    if (!s_stack_started) {
        return ESP_ZB_ZCL_STATUS_SUCCESS;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint,
        cluster_id,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        attr_id,
        value,
        false);
    esp_zb_lock_release();

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set attr endpoint=%u cluster=0x%04x attr=0x%04x status=0x%x",
                 endpoint,
                 cluster_id,
                 attr_id,
                 status);
    }

    return status;
}

static void set_zcl_attr_from_callback(uint8_t endpoint, uint16_t cluster_id,
                                       uint16_t attr_id, void *value)
{
    if (!s_stack_started) {
        return;
    }

    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint,
        cluster_id,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        attr_id,
        value,
        false);

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "callback set attr endpoint=%u cluster=0x%04x attr=0x%04x status=0x%x",
                 endpoint,
                 cluster_id,
                 attr_id,
                 status);
    }
}

static void sync_endpoint_attrs(bool report)
{
    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        const uint8_t endpoint = s_switch_endpoints[i];
        set_zcl_attr_locked(endpoint,
                            ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                            ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                            &s_endpoint_on[i]);
        if (report) {
            schedule_on_off_report(endpoint);
        }
    }

    set_zcl_attr_locked(ZB_LIGHT_ENDPOINT,
                        ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                        ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                        &s_level_current);
    set_zcl_attr_locked(ZB_LIGHT_ENDPOINT,
                        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                        &s_color_temperature_mired);

    if (report) {
        schedule_level_report(ZB_MIN_REPORT_DELAY_MS);
        schedule_color_temperature_report(ZB_MIN_REPORT_DELAY_MS);
    }
}

static const char *endpoint_label(uint8_t endpoint)
{
    const int endpoint_index = endpoint_to_index(endpoint);
    if (endpoint_index < 0) {
        return "unknown";
    }
    if (endpoint_index == 6) {
        return "EnableRS232";
    }
    if (endpoint_index == 7) {
        return "EnableOTA";
    }
    if (endpoint_index == 8) {
        return "Light";
    }

    static const char *const switch_names[ZB_SWITCH_COUNT] = {
        "Switch1",
        "Switch2",
        "Switch3",
        "Switch4",
        "Switch5",
        "Switch6",
    };
    return switch_names[endpoint_index];
}

static void report_standard_attr(uint8_t endpoint, uint16_t cluster_id, uint16_t attr_id)
{
    if (!s_joined) {
        return;
    }

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,
            .dst_endpoint = 1,
            .src_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = attr_id,
    };

    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err == ESP_OK) {
        status_led_indicate_ha_publish();
    }
    ESP_LOGI(TAG, "report standard endpoint=%u cluster=0x%04x attr=0x%04x ret=%s(0x%x)",
             endpoint,
             cluster_id,
             attr_id,
             esp_err_to_name(err),
             err);
}

static void report_level_cb(uint8_t endpoint)
{
    if (endpoint != s_level_report_generation) {
        ESP_LOGI(TAG, "skip stale Brightness report generation=%u current=%u",
                 endpoint,
                 s_level_report_generation);
        return;
    }
    report_standard_attr(ZB_LIGHT_ENDPOINT,
                         ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                         ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
}

static void report_color_temperature_cb(uint8_t endpoint)
{
    if (endpoint != s_color_temperature_report_generation) {
        ESP_LOGI(TAG, "skip stale WhiteTemperature report generation=%u current=%u",
                 endpoint,
                 s_color_temperature_report_generation);
        return;
    }
    report_standard_attr(ZB_LIGHT_ENDPOINT,
                         ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                         ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
}

static void schedule_level_report(uint32_t delay_ms)
{
    ++s_level_report_generation;
    esp_zb_scheduler_alarm((esp_zb_callback_t)report_level_cb,
                           s_level_report_generation,
                           delay_ms);
    ESP_LOGI(TAG, "scheduled Brightness report generation=%u delay_ms=%lu",
             s_level_report_generation,
             (unsigned long)delay_ms);
}

static void schedule_color_temperature_report(uint32_t delay_ms)
{
    ++s_color_temperature_report_generation;
    esp_zb_scheduler_alarm((esp_zb_callback_t)report_color_temperature_cb,
                           s_color_temperature_report_generation,
                           delay_ms);
    ESP_LOGI(TAG, "scheduled WhiteTemperature report generation=%u delay_ms=%lu",
             s_color_temperature_report_generation,
             (unsigned long)delay_ms);
}

static void report_on_off_cb(uint8_t endpoint)
{
    const uint8_t generation = endpoint >> 4;
    endpoint &= 0x0f;

    if (!s_joined || endpoint_to_index(endpoint) < 0) {
        return;
    }

    const int endpoint_index = endpoint_to_index(endpoint);
    if (generation != (s_on_off_report_generation[endpoint_index] & 0x0f)) {
        ESP_LOGI(TAG, "skip stale OnOff report endpoint=%u generation=%u current=%u",
                 endpoint,
                 generation,
                 s_on_off_report_generation[endpoint_index] & 0x0f);
        return;
    }

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,
            .dst_endpoint = 1,
            .src_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
    };

    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err == ESP_OK) {
        status_led_indicate_ha_publish();
    }
    ESP_LOGI(TAG, "report OnOff endpoint=%u dst=0x0000/1 attr=0x%04x ret=%s(0x%x)",
             endpoint,
             ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
             esp_err_to_name(err),
             err);
}

static void schedule_on_off_report(uint8_t endpoint)
{
    schedule_on_off_report_delay(endpoint, ZB_MIN_REPORT_DELAY_MS);
}

static void schedule_on_off_report_delay(uint8_t endpoint, uint32_t delay_ms)
{
    const int endpoint_index = endpoint_to_index(endpoint);
    if (endpoint_index < 0) {
        return;
    }
    s_on_off_report_generation[endpoint_index] =
        (uint8_t)((s_on_off_report_generation[endpoint_index] + 1U) & 0x0f);
    const uint8_t packed = (uint8_t)((s_on_off_report_generation[endpoint_index] << 4) | (endpoint & 0x0f));
    esp_zb_scheduler_alarm((esp_zb_callback_t)report_on_off_cb,
                           packed,
                           delay_ms);
    ESP_LOGI(TAG, "scheduled OnOff report endpoint=%u generation=%u delay_ms=%lu",
             endpoint,
             s_on_off_report_generation[endpoint_index],
             (unsigned long)delay_ms);
}

static void schedule_initial_on_off_reports(void)
{
    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        schedule_on_off_report_delay(s_switch_endpoints[i], ZB_MIN_REPORT_DELAY_MS + (100 * i));
    }
    schedule_level_report(ZB_MIN_REPORT_DELAY_MS + (100 * ZB_ENDPOINT_COUNT));
    schedule_color_temperature_report(ZB_MIN_REPORT_DELAY_MS + (100 * (ZB_ENDPOINT_COUNT + 1)));
    ESP_LOGI(TAG, "scheduled initial OnOff reports endpoints=1..%u including Light endpoint=%u",
             ZB_ENDPOINT_COUNT,
             ZB_LIGHT_ENDPOINT);
}

static esp_err_t verify_endpoint_clusters(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    ESP_RETURN_ON_FALSE(cluster_list != NULL, ESP_ERR_INVALID_ARG, TAG, "endpoint %u not found", endpoint);

    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
        cluster_list,
        ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    esp_zb_attribute_list_t *on_off_cluster = esp_zb_cluster_list_get_cluster(
        cluster_list,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_RETURN_ON_FALSE(basic_cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "Basic cluster not found on endpoint %u", endpoint);
    ESP_RETURN_ON_FALSE(on_off_cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "OnOff cluster not found on endpoint %u", endpoint);

    if (endpoint == ZB_LIGHT_ENDPOINT) {
        esp_zb_attribute_list_t *level_cluster = esp_zb_cluster_list_get_cluster(
            cluster_list,
            ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
        esp_zb_attribute_list_t *color_cluster = esp_zb_cluster_list_get_cluster(
            cluster_list,
            ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
            ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

        ESP_LOGI(TAG, "endpoint %u clusters: Basic(server)=yes OnOff(server)=yes Level(server)=%s Color(server)=%s",
                 endpoint,
                 level_cluster != NULL ? "yes" : "no",
                 color_cluster != NULL ? "yes" : "no");
        ESP_RETURN_ON_FALSE(level_cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "Level cluster not found on endpoint %u", endpoint);
        ESP_RETURN_ON_FALSE(color_cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "Color cluster not found on endpoint %u", endpoint);
    } else {
        ESP_LOGI(TAG, "endpoint %u clusters: Basic(server)=yes OnOff(server)=yes", endpoint);
    }

    return ESP_OK;
}

static esp_err_t add_light_control_clusters(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, ZB_LIGHT_ENDPOINT);
    ESP_RETURN_ON_FALSE(cluster_list != NULL, ESP_ERR_INVALID_ARG, TAG, "light endpoint %u not found", ZB_LIGHT_ENDPOINT);

    s_level_current = percent_to_level(s_brightness_percent);
    esp_zb_level_cluster_cfg_t level_cfg = {
        .current_level = s_level_current,
    };
    esp_zb_attribute_list_t *level_cluster = esp_zb_level_cluster_create(&level_cfg);
    ESP_RETURN_ON_FALSE(level_cluster != NULL, ESP_ERR_NO_MEM, TAG, "failed to create Level cluster");
    esp_err_t ret = esp_zb_cluster_list_add_level_cluster(
        cluster_list,
        level_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_LOGI(TAG, "add Level cluster endpoint=%u current_level=%u brightness=%u%% ret=%s(0x%x)",
             ZB_LIGHT_ENDPOINT,
             s_level_current,
             s_brightness_percent,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add Level cluster");

    s_color_temperature_mired = kelvin_to_mired(s_white_temperature_kelvin);
    s_color_temperature_startup_mired = s_color_temperature_mired;
    esp_zb_color_cluster_cfg_t color_cfg = {
        .current_x = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_X_DEF_VALUE,
        .current_y = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_Y_DEF_VALUE,
        .color_mode = 0x02,
        .options = ESP_ZB_ZCL_COLOR_CONTROL_OPTIONS_DEFAULT_VALUE,
        .enhanced_color_mode = 0x02,
        .color_capabilities = ZB_COLOR_TEMP_CAPABILITY,
    };
    esp_zb_attribute_list_t *color_cluster = esp_zb_color_control_cluster_create(&color_cfg);
    ESP_RETURN_ON_FALSE(color_cluster != NULL, ESP_ERR_NO_MEM, TAG, "failed to create Color Control cluster");

    ret = esp_zb_color_control_cluster_add_attr(
        color_cluster,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
        &s_color_temperature_mired);
    ESP_LOGI(TAG, "add Color attr ColorTemperature(0x%04x) mired=%u kelvin=%u ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
             s_color_temperature_mired,
             s_white_temperature_kelvin,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add ColorTemperature");

    ret = esp_zb_color_control_cluster_add_attr(
        color_cluster,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID,
        &s_color_temperature_min_mired);
    ESP_LOGI(TAG, "add Color attr PhysicalMinMireds(0x%04x)=%u ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID,
             s_color_temperature_min_mired,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add PhysicalMinMireds");

    ret = esp_zb_color_control_cluster_add_attr(
        color_cluster,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID,
        &s_color_temperature_max_mired);
    ESP_LOGI(TAG, "add Color attr PhysicalMaxMireds(0x%04x)=%u ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID,
             s_color_temperature_max_mired,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add PhysicalMaxMireds");

    ret = esp_zb_color_control_cluster_add_attr(
        color_cluster,
        ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID,
        &s_color_temperature_startup_mired);
    ESP_LOGI(TAG, "add Color attr StartUpColorTemperature(0x%04x)=%u ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID,
             s_color_temperature_startup_mired,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add StartUpColorTemperature");

    ret = esp_zb_cluster_list_add_color_control_cluster(
        cluster_list,
        color_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_LOGI(TAG, "add Color Control cluster endpoint=%u temp=%uK mired=%u range=%u..%uK mired=%u..%u ret=%s(0x%x)",
             ZB_LIGHT_ENDPOINT,
             s_white_temperature_kelvin,
             s_color_temperature_mired,
             ZB_WHITE_TEMP_MIN_K,
             ZB_WHITE_TEMP_MAX_K,
             s_color_temperature_min_mired,
             s_color_temperature_max_mired,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add Color Control cluster");

    return ESP_OK;
}

static esp_err_t add_ota_command_cluster(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, ZB_FIRST_SWITCH_ENDPOINT);
    ESP_RETURN_ON_FALSE(cluster_list != NULL,
                        ESP_ERR_INVALID_ARG,
                        TAG,
                        "OTA endpoint %u not found",
                        ZB_FIRST_SWITCH_ENDPOINT);

    esp_zb_attribute_list_t *ota_cluster = esp_zb_zcl_attr_list_create(ZIGBEE_OTA_CLUSTER_ID);
    ESP_RETURN_ON_FALSE(ota_cluster != NULL, ESP_ERR_NO_MEM, TAG, "failed to create OTA custom cluster");

    esp_err_t ret = zigbee_ota_cluster_add_attrs(ota_cluster);
    ESP_LOGI(TAG,
             "add OTA custom attr endpoint=%u cluster=0x%04x attr=0x%04x ret=%s(0x%x)",
             ZB_FIRST_SWITCH_ENDPOINT,
             ZIGBEE_OTA_CLUSTER_ID,
             ZIGBEE_OTA_CONFIG_ATTR_ID,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add OTA custom attr");

    ret = esp_zb_cluster_list_add_custom_cluster(
        cluster_list,
        ota_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_LOGI(TAG,
             "add OTA custom cluster endpoint=%u cluster=0x%04x ret=%s(0x%x)",
             ZB_FIRST_SWITCH_ENDPOINT,
             ZIGBEE_OTA_CLUSTER_ID,
             esp_err_to_name(ret),
             ret);
    return ret;
}

static esp_err_t add_basic_identity(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    static uint8_t manufacturer_name[] = "\x05" ZB_MIN_MANUFACTURER_TEXT;
    static uint8_t model_identifier[] = "\x15" ZB_MIN_MODEL_TEXT;
    static uint8_t sw_build_id[] = "\x05" ZB_MIN_SW_BUILD_TEXT;

    _Static_assert(sizeof(manufacturer_name) == ZB_MIN_MANUFACTURER_LEN + 2,
                   "ManufacturerName ZCL string length byte does not match text length");
    _Static_assert(sizeof(model_identifier) == ZB_MIN_MODEL_LEN + 2,
                   "ModelIdentifier ZCL string length byte does not match text length");
    _Static_assert(sizeof(sw_build_id) == ZB_MIN_SW_BUILD_LEN + 2,
                   "SWBuildID ZCL string length byte does not match text length");

    ESP_LOGI(TAG, "Basic identity target: endpoint=%u cluster=0x%04x role=server/input",
             endpoint,
             ESP_ZB_ZCL_CLUSTER_ID_BASIC);
    ESP_LOGI(TAG, "ManufacturerName: %s len=%u", ZB_MIN_MANUFACTURER_TEXT, ZB_MIN_MANUFACTURER_LEN);
    ESP_LOGI(TAG, "ModelIdentifier: %s len=%u", ZB_MIN_MODEL_TEXT, ZB_MIN_MODEL_LEN);
    ESP_LOGI(TAG, "SWBuildID: %s len=%u", ZB_MIN_SW_BUILD_TEXT, ZB_MIN_SW_BUILD_LEN);

    esp_zb_cluster_list_t *cluster_list = esp_zb_ep_list_get_ep(ep_list, endpoint);
    ESP_RETURN_ON_FALSE(cluster_list != NULL, ESP_ERR_INVALID_ARG, TAG, "endpoint %u not found", endpoint);

    esp_zb_attribute_list_t *basic_cluster = esp_zb_cluster_list_get_cluster(
        cluster_list,
        ESP_ZB_ZCL_CLUSTER_ID_BASIC,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_RETURN_ON_FALSE(basic_cluster != NULL, ESP_ERR_INVALID_ARG, TAG, "Basic cluster not found");
    ESP_LOGI(TAG, "Basic server cluster found: yes");

    esp_err_t ret = esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        manufacturer_name);
    ESP_LOGI(TAG, "add Basic attr ManufacturerName(0x%04x) ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add ManufacturerName");

    ret = esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        model_identifier);
    ESP_LOGI(TAG, "add Basic attr ModelIdentifier(0x%04x) ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add ModelIdentifier");

    ret = esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
        sw_build_id);
    ESP_LOGI(TAG, "add Basic attr SWBuildID(0x%04x) ret=%s(0x%x)",
             ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
             esp_err_to_name(ret),
             ret);
    ESP_RETURN_ON_ERROR(ret, TAG, "failed to add SWBuildID");

    return ESP_OK;
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    if (signal_struct == NULL || signal_struct->p_app_signal == NULL) {
        ESP_LOGE(TAG, "Zigbee signal callback with empty signal_struct");
        return;
    }

    const esp_zb_app_signal_type_t sig_type = *(esp_zb_app_signal_type_t *)signal_struct->p_app_signal;
    const esp_err_t err_status = signal_struct->esp_err_status;

    ESP_LOGI(TAG, "signal=%s(0x%x) status=%s",
             esp_zb_zdo_signal_to_string(sig_type),
             sig_type,
             esp_err_to_name(err_status));

    switch (sig_type) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            log_network_state("before_initialization");
            ESP_LOGI(TAG, "commissioning start: initialization");
            if (esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION) != ESP_OK) {
                ESP_LOGE(TAG, "ESP_ZB_BDB_MODE_INITIALIZATION failed");
            }
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            log_network_state(sig_type == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START ? "device_first_start" : "device_reboot");
            s_joined = !esp_zb_bdb_is_factory_new();
            if (err_status != ESP_OK) {
                ESP_LOGW(TAG,
                         "Zigbee stack initialization status is %s; continuing as joined=%s factory_new=%s for diagnostics",
                         esp_err_to_name(err_status),
                         s_joined ? "true" : "false",
                         esp_zb_bdb_is_factory_new() ? "true" : "false");
            }
            if (esp_zb_bdb_is_factory_new()) {
                apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "factory-new fast");
                start_network_steering_now("factory-new");
            } else {
                status_led_set_zigbee_joined(true);
                const uint8_t channel = esp_zb_get_current_channel();
                if (zigbee_channel_is_valid(channel)) {
                    storage_save_zigbee_last_channel(channel);
                    s_fast_pair_channel = channel;
                }
                ESP_LOGI(TAG, "device is not factory-new; it will not rejoin another coordinator until Zigbee NVS is erased");
                schedule_initial_on_off_reports();
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (err_status == ESP_OK) {
                s_joined = true;
                s_steering_fail_count = 0;
                status_led_set_failure(false);
                status_led_set_zigbee_joined(true);
                log_network_state("steering_success");
                const uint8_t channel = esp_zb_get_current_channel();
                if (zigbee_channel_is_valid(channel)) {
                    storage_save_zigbee_last_channel(channel);
                    s_fast_pair_channel = channel;
                }
                ESP_LOGI(TAG, "joined network successfully");
                schedule_initial_on_off_reports();
            } else {
                s_joined = false;
                s_steering_fail_count++;
                if (s_steering_fail_count >= ZB_MIN_FAST_ATTEMPTS &&
                    s_current_channel_mask == zigbee_channel_mask(s_fast_pair_channel)) {
                    apply_steering_channel_mask(zigbee_configured_scan_mask(), "fallback scan");
                }
                status_led_set_failure(s_current_channel_mask != zigbee_configured_scan_mask());
                status_led_set_zigbee_pairing(true);
                status_led_set_zigbee_joined(false);
                log_network_state("steering_failed");
                ESP_LOGW(TAG, "network steering failed: %s; retry in 1 s fail_count=%u mask=0x%08lx",
                         esp_err_to_name(err_status),
                         s_steering_fail_count,
                         (unsigned long)s_current_channel_mask);
                esp_zb_scheduler_alarm((esp_zb_callback_t)start_steering_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                       1000);
            }
            break;

        default:
            log_network_state("signal_state");
            break;
    }
}

static esp_err_t zb_attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL) {
        ESP_LOGE(TAG, "empty SET_ATTR callback");
        return ESP_ERR_INVALID_ARG;
    }
    if (message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "SET_ATTR status=%u", message->info.status);
        return ESP_ERR_INVALID_ARG;
    }

    if (zigbee_ota_cluster_handle_set_attr(message)) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "SET_ATTR endpoint=%u cluster=0x%04hx attr=0x%04hx type=0x%02x size=%u",
             message->info.dst_endpoint,
             message->info.cluster,
             message->attribute.id,
             message->attribute.data.type,
             message->attribute.data.size);

    const int endpoint_index = endpoint_to_index(message->info.dst_endpoint);
    if (endpoint_index >= 0 &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL &&
        message->attribute.data.value != NULL) {
        const bool on = *(bool *)message->attribute.data.value;
        s_endpoint_on[endpoint_index] = on;
        set_zcl_attr_from_callback(message->info.dst_endpoint,
                                   ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                   ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID,
                                   &s_endpoint_on[endpoint_index]);
        schedule_on_off_report_delay(message->info.dst_endpoint, 0);
        ESP_LOGI(TAG, "HA/ZHA OnOff command endpoint=%u %s: %s",
                 message->info.dst_endpoint,
                 endpoint_label(message->info.dst_endpoint),
                 on ? "ON" : "OFF");
        ESP_LOGI(TAG, "minimal states: switch1=%s switch2=%s switch3=%s switch4=%s switch5=%s switch6=%s EnableRS232=%s EnableOTA=%s Light=%s Brightness=%u%% WhiteTemperature=%uK",
                 s_endpoint_on[0] ? "ON" : "OFF",
                 s_endpoint_on[1] ? "ON" : "OFF",
                 s_endpoint_on[2] ? "ON" : "OFF",
                 s_endpoint_on[3] ? "ON" : "OFF",
                 s_endpoint_on[4] ? "ON" : "OFF",
                 s_endpoint_on[5] ? "ON" : "OFF",
                 s_endpoint_on[6] ? "ON" : "OFF",
                 s_endpoint_on[7] ? "ON" : "OFF",
                 s_endpoint_on[8] ? "ON" : "OFF",
                 s_brightness_percent,
                 s_white_temperature_kelvin);
        if (endpoint_index >= 0 && endpoint_index < ZB_SWITCH_COUNT) {
            publish_input_event(INPUT_EVENT_ZIGBEE_SWITCH_SET,
                                (uint8_t)(endpoint_index + 1),
                                on ? 1 : 0);
        } else if (endpoint_index == 6) {
            publish_input_event(INPUT_EVENT_ZIGBEE_RS232_SET, 0, on ? 1 : 0);
        } else if (endpoint_index == 7) {
            publish_input_event(INPUT_EVENT_ZIGBEE_OTA_SET, 0, on ? 1 : 0);
        } else if (endpoint_index == 8) {
            publish_input_event(INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET, 0, on ? 1 : 0);
        }
    } else if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
               message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
               message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
               message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 &&
               message->attribute.data.value != NULL) {
        s_level_current = *(uint8_t *)message->attribute.data.value;
        s_brightness_percent = level_to_percent(s_level_current);
        set_zcl_attr_from_callback(ZB_LIGHT_ENDPOINT,
                                   ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                                   ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID,
                                   &s_level_current);
        schedule_level_report(0);
        ESP_LOGI(TAG, "HA/ZHA brightness command endpoint=%u Brightness=%u%% level=%u",
                 message->info.dst_endpoint,
                 s_brightness_percent,
                 s_level_current);
        publish_input_event(INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET, 0, s_brightness_percent);
    } else if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
               message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
               message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID &&
               message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 &&
               message->attribute.data.value != NULL) {
        s_color_temperature_mired = *(uint16_t *)message->attribute.data.value;
        if (s_color_temperature_mired < ZB_MIN_MIREDS) {
            s_color_temperature_mired = ZB_MIN_MIREDS;
        } else if (s_color_temperature_mired > ZB_MAX_MIREDS) {
            s_color_temperature_mired = ZB_MAX_MIREDS;
        }
        s_white_temperature_kelvin = mired_to_kelvin(s_color_temperature_mired);
        set_zcl_attr_from_callback(ZB_LIGHT_ENDPOINT,
                                   ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                                   ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                                   &s_color_temperature_mired);
        schedule_color_temperature_report(0);
        ESP_LOGI(TAG, "HA/ZHA white temperature command endpoint=%u WhiteTemperature=%uK mired=%u",
                 message->info.dst_endpoint,
                 s_white_temperature_kelvin,
                 s_color_temperature_mired);
        publish_input_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, 0, s_white_temperature_kelvin);
    }

    return ESP_OK;
}

static esp_err_t zb_privilege_command_handler(const esp_zb_zcl_privilege_command_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS || message->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG,
             "PRIV_CMD endpoint=%u cluster=0x%04hx cmd=0x%02x size=%u",
             message->info.dst_endpoint,
             message->info.cluster,
             message->info.command.id,
             message->size);

    if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->info.command.id == ESP_ZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE &&
        message->size >= sizeof(uint16_t)) {
        s_color_temperature_mired = read_u16_le(message->data);
        if (s_color_temperature_mired < ZB_MIN_MIREDS) {
            s_color_temperature_mired = ZB_MIN_MIREDS;
        } else if (s_color_temperature_mired > ZB_MAX_MIREDS) {
            s_color_temperature_mired = ZB_MAX_MIREDS;
        }
        s_white_temperature_kelvin = mired_to_kelvin(s_color_temperature_mired);
        set_zcl_attr_from_callback(ZB_LIGHT_ENDPOINT,
                                   ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                                   ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID,
                                   &s_color_temperature_mired);
        schedule_color_temperature_report(0);
        ESP_LOGI(TAG,
                 "HA/ZHA white temperature command endpoint=%u WhiteTemperature=%uK mired=%u",
                 message->info.dst_endpoint,
                 s_white_temperature_kelvin,
                 s_color_temperature_mired);
        publish_input_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, 0, s_white_temperature_kelvin);
    }

    return ESP_OK;
}

static esp_zb_cluster_list_t *create_on_off_light_clusters(uint8_t endpoint)
{
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    light_cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    const int endpoint_index = endpoint_to_index(endpoint);
    light_cfg.on_off_cfg.on_off = endpoint_index >= 0 ? s_endpoint_on[endpoint_index] : false;

    return esp_zb_on_off_light_clusters_create(&light_cfg);
}

static esp_err_t add_standard_on_off_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    esp_zb_cluster_list_t *clusters = create_on_off_light_clusters(endpoint);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_NO_MEM, TAG, "failed to create clusters for endpoint %u", endpoint);

    const esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };

    esp_err_t ret = esp_zb_ep_list_add_ep(ep_list, clusters, endpoint_config);
    ESP_LOGI(TAG, "add standard On/Off endpoint=%u profile=0x%04x device=0x%04x ret=%s(0x%x)",
             endpoint,
             endpoint_config.app_profile_id,
             endpoint_config.app_device_id,
             esp_err_to_name(ret),
             ret);
    return ret;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
            return zb_attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID:
            return zb_privilege_command_handler((const esp_zb_zcl_privilege_command_message_t *)message);
        default:
            ESP_LOGI(TAG, "action callback=0x%x", callback_id);
            return ESP_OK;
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;

    ESP_LOGI(TAG, "ESP Zigbee SDK: %s", esp_zb_get_version_string());
    ESP_LOGI(TAG, "minimal descriptor: endpoints=1..%u profile=0x%04x device=HA On/Off Light(0x0100) endpoints 1-6 switches 7 EnableRS232 8 EnableOTA 9 Light default=OFF Brightness=%u%% WhiteTemperature=%uK range=%u..%uK",
             ZB_ENDPOINT_COUNT,
             ESP_ZB_AF_HA_PROFILE_ID,
             s_brightness_percent,
             s_white_temperature_kelvin,
             ZB_WHITE_TEMP_MIN_K,
             ZB_WHITE_TEMP_MAX_K);
    load_fast_pair_channel();

    ESP_LOGI(TAG, "ZED config: install_code=%d keep_alive_ms=%u aging_timeout=%u fast_channel=%u fallback_mask=0x%08lx",
             ZB_MIN_INSTALL_CODE,
             ZB_MIN_ED_KEEP_ALIVE_MS,
             ZB_MIN_ED_AGING_TIMEOUT,
             s_fast_pair_channel,
             (unsigned long)zigbee_configured_scan_mask());

    esp_zb_cfg_t nwk_cfg = ESP_ZB_MIN_ZED_CONFIG();
    esp_zb_init(&nwk_cfg);
    ESP_LOGI(TAG, "esp_zb_init done");

    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    light_cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    light_cfg.on_off_cfg.on_off = s_endpoint_on[0];

    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(ZB_FIRST_SWITCH_ENDPOINT, &light_cfg);
    if (ep_list == NULL) {
        ESP_LOGE(TAG, "esp_zb_on_off_light_ep_create failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "standard On/Off Light endpoint created endpoint=%u default=OFF", ZB_FIRST_SWITCH_ENDPOINT);

    for (uint8_t i = 1; i < ZB_ENDPOINT_COUNT; ++i) {
        ESP_ERROR_CHECK(add_standard_on_off_endpoint(ep_list, s_switch_endpoints[i]));
    }
    ESP_ERROR_CHECK(add_light_control_clusters(ep_list));
    ESP_ERROR_CHECK(add_ota_command_cluster(ep_list));
    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        ESP_ERROR_CHECK(verify_endpoint_clusters(ep_list, s_switch_endpoints[i]));
        ESP_ERROR_CHECK(add_basic_identity(ep_list, s_switch_endpoints[i]));
    }
    ESP_LOGI(TAG, "registered endpoint count=%u", ZB_ENDPOINT_COUNT);
    ESP_ERROR_CHECK(esp_zb_device_register(ep_list));
    ESP_LOGI(TAG, "esp_zb_device_register done");

    ESP_ERROR_CHECK(esp_zb_zcl_add_privilege_command(
        ZB_LIGHT_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
        ESP_ZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE));
    ESP_LOGI(TAG, "privilege ColorControl MoveToColorTemperature command registered");

    ESP_ERROR_CHECK(esp_zb_zcl_add_privilege_command(
        ZIGBEE_OTA_ENDPOINT,
        ZIGBEE_OTA_CLUSTER_ID,
        ZIGBEE_OTA_CMD_DEVICE_AUTH_CHALLENGE_ID));
    ESP_ERROR_CHECK(esp_zb_zcl_add_privilege_command(
        ZIGBEE_OTA_ENDPOINT,
        ZIGBEE_OTA_CLUSTER_ID,
        ZIGBEE_OTA_CMD_DEVICE_ENROLL_ID));
    ESP_ERROR_CHECK(esp_zb_zcl_add_privilege_command(
        ZIGBEE_OTA_ENDPOINT,
        ZIGBEE_OTA_CLUSTER_ID,
        ZIGBEE_OTA_CMD_COMMAND_ACK_ID));
    ESP_LOGI(TAG, "privilege OTA enrollment commands registered");

    esp_zb_core_action_handler_register(zb_action_handler);
    ESP_LOGI(TAG, "action handler registered");

    esp_zb_set_rx_on_when_idle(true);
    ESP_LOGI(TAG, "rx_on_when_idle=%s", esp_zb_get_rx_on_when_idle() ? "true" : "false");

    apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "initial fast");

    ESP_ERROR_CHECK(esp_zb_start(false));
    s_stack_started = true;
    ESP_LOGI(TAG, "esp_zb_start returned; entering stack main loop");

    esp_zb_stack_main_loop();
}

void zigbee_minimal_apply_state(const device_state_t *state, bool ota_enabled, bool report)
{
    if (state == NULL) {
        return;
    }

    for (uint8_t i = 0; i < ZB_SWITCH_COUNT; ++i) {
        s_endpoint_on[i] = (state->switches & (1U << i)) != 0;
    }
    s_endpoint_on[6] = state->rs232_enabled;
    s_endpoint_on[7] = ota_enabled;
    s_endpoint_on[8] = (state->switches & ((1U << ZB_SWITCH_COUNT) - 1U)) != 0;

    s_brightness_percent = state->brightness > ZB_BRIGHTNESS_MAX ? ZB_BRIGHTNESS_MAX : state->brightness;
    s_white_temperature_kelvin = state->white_temperature;
    if (s_white_temperature_kelvin < ZB_WHITE_TEMP_MIN_K) {
        s_white_temperature_kelvin = ZB_WHITE_TEMP_MIN_K;
    } else if (s_white_temperature_kelvin > ZB_WHITE_TEMP_MAX_K) {
        s_white_temperature_kelvin = ZB_WHITE_TEMP_MAX_K;
    }

    s_level_current = percent_to_level(s_brightness_percent);
    s_color_temperature_mired = kelvin_to_mired(s_white_temperature_kelvin);
    s_color_temperature_startup_mired = s_color_temperature_mired;

    ESP_LOGI(TAG,
             "apply app state report=%s switches=0x%02x EnableRS232=%s EnableOTA=%s Light=%s Brightness=%u%% level=%u WhiteTemperature=%uK mired=%u",
             report ? "true" : "false",
             state->switches & ((1U << ZB_SWITCH_COUNT) - 1U),
             s_endpoint_on[6] ? "ON" : "OFF",
             s_endpoint_on[7] ? "ON" : "OFF",
             s_endpoint_on[8] ? "ON" : "OFF",
             s_brightness_percent,
             s_level_current,
             s_white_temperature_kelvin,
             s_color_temperature_mired);

    sync_endpoint_attrs(report);
}

void zigbee_minimal_request_repair(void)
{
    if (!s_stack_started) {
        ESP_LOGW(TAG, "manual re-pair requested before Zigbee stack start; ignored");
        return;
    }

    if (s_manual_repair_requested) {
        ESP_LOGW(TAG, "manual re-pair already requested");
        return;
    }

    s_manual_repair_requested = true;
    ESP_LOGW(TAG, "manual re-pair requested");
    status_led_set_failure(false);
    status_led_set_zigbee_joined(false);
    status_led_set_zigbee_pairing(true);
    esp_zb_scheduler_alarm((esp_zb_callback_t)manual_repair_cb, 0, 0);
}

void zigbee_minimal_init(void)
{
    const esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_MIN_RADIO_CONFIG(),
        .host_config = ESP_ZB_MIN_HOST_CONFIG(),
    };

    ESP_LOGI(TAG, "configure Zigbee platform: native radio, no host connection");
    ESP_ERROR_CHECK(esp_zb_platform_config((esp_zb_platform_config_t *)&config));

    BaseType_t created = xTaskCreate(zigbee_task,
                                     "Zigbee_main",
                                     ZB_MIN_TASK_STACK,
                                     NULL,
                                     ZB_MIN_TASK_PRIORITY,
                                     NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);
}
