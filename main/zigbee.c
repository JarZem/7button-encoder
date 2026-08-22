#include "zigbee.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "jarzem_secure_ota.h"
#include "nwk/esp_zigbee_nwk.h"
#include "state.h"
#include "status_led.h"
#include "storage.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"

#if !defined ZB_ED_ROLE
#error "Remote control must be built as Zigbee End Device."
#endif

static const char *TAG = "zigbee";

#define ZB_ENDPOINT_COUNT        9
#define ZB_SWITCH_COUNT          6
#define ZB_LIGHT_ENDPOINT        8
#define ZB_RS232_ENDPOINT        9
#define ZB_DEFAULT_CHANNEL       CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL
#define ZB_SCAN_CHANNEL_MASK     CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK
#define ZB_FAST_ATTEMPTS         CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS
#define ZB_ED_AGING_TIMEOUT      ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ZB_ED_KEEP_ALIVE_MS      3000
#define ZB_TASK_STACK            4096
#define ZB_TASK_PRIORITY         5
#define ZB_MIN_MIREDS            154
#define ZB_MAX_MIREDS            333
#define ZB_COLOR_TEMP_CAPABILITY (1U << 4)

static bool s_started;
static bool s_joined;
static bool s_manual_repair_requested;
static uint8_t s_fast_pair_channel;
static uint8_t s_steering_fail_count;
static uint32_t s_current_channel_mask;
static bool s_on_off[ZB_ENDPOINT_COUNT];
static uint8_t s_level = 254;
static uint16_t s_color_temp = 250;
static uint16_t s_color_temp_min = ZB_MIN_MIREDS;
static uint16_t s_color_temp_max = ZB_MAX_MIREDS;
static uint16_t s_color_temp_startup = 250;

#define ESP_ZB_APP_ZED_CONFIG() { \
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, \
    .install_code_policy = false, \
    .nwk_cfg.zed_cfg = { \
        .ed_timeout = ZB_ED_AGING_TIMEOUT, \
        .keep_alive = ZB_ED_KEEP_ALIVE_MS, \
    }, \
}
#define ESP_ZB_APP_RADIO_CONFIG() { .radio_mode = ZB_RADIO_MODE_NATIVE }
#define ESP_ZB_APP_HOST_CONFIG()  { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }

static bool channel_valid(uint8_t channel)
{
    return channel >= 11 && channel <= 26;
}

static uint32_t channel_mask(uint8_t channel)
{
    return channel_valid(channel) ? (1UL << channel) : 0;
}

static uint32_t configured_scan_mask(void)
{
    return ZB_SCAN_CHANNEL_MASK != 0 ? ZB_SCAN_CHANNEL_MASK : 0x07FFF800UL;
}

static uint8_t percent_to_level(uint8_t percent)
{
    if (percent > DEVICE_BRIGHTNESS_MAX) percent = DEVICE_BRIGHTNESS_MAX;
    return (uint8_t)(((uint16_t)percent * 254U + 50U) / 100U);
}

static uint8_t level_to_percent(uint8_t level)
{
    return (uint8_t)(((uint16_t)level * 100U + 127U) / 254U);
}

static uint16_t kelvin_to_mired(uint16_t kelvin)
{
    if (kelvin < DEVICE_WHITE_TEMP_MIN) kelvin = DEVICE_WHITE_TEMP_MIN;
    if (kelvin > DEVICE_WHITE_TEMP_MAX) kelvin = DEVICE_WHITE_TEMP_MAX;
    return (uint16_t)((1000000UL + kelvin / 2U) / kelvin);
}

static uint16_t mired_to_kelvin(uint16_t mired)
{
    if (mired < ZB_MIN_MIREDS) mired = ZB_MIN_MIREDS;
    if (mired > ZB_MAX_MIREDS) mired = ZB_MAX_MIREDS;
    return (uint16_t)((1000000UL + mired / 2U) / mired);
}

static uint16_t read_u16_le(const void *data)
{
    const uint8_t *p = (const uint8_t *)data;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void load_fast_pair_channel(void)
{
    uint8_t stored = 0;
    if (storage_load_zigbee_last_channel(&stored) && channel_valid(stored)) {
        s_fast_pair_channel = stored;
    } else {
        s_fast_pair_channel = channel_valid(ZB_DEFAULT_CHANNEL) ? ZB_DEFAULT_CHANNEL : 11;
    }
}

static void apply_channel_mask(uint32_t mask)
{
    if (mask == 0) mask = configured_scan_mask();
    s_current_channel_mask = mask;
    esp_zb_set_primary_network_channel_set(mask);
}

static void start_steering(const char *reason)
{
    status_led_set_failure(false);
    status_led_set_zigbee_pairing(true);
    ESP_LOGI(TAG, "network steering reason=%s mask=0x%08lx", reason, (unsigned long)s_current_channel_mask);
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (err != ESP_OK) status_led_set_failure(true);
}

static void steering_retry_cb(uint8_t mode)
{
    (void)mode;
    start_steering("retry");
}

static void repair_cb(uint8_t unused)
{
    (void)unused;
    s_manual_repair_requested = false;
    status_led_set_failure(false);
    status_led_set_zigbee_joined(false);
    status_led_set_zigbee_pairing(true);
    if (esp_zb_bdb_is_factory_new()) {
        s_steering_fail_count = 0;
        apply_channel_mask(channel_mask(s_fast_pair_channel));
        start_steering("manual");
    } else {
        esp_zb_factory_reset();
    }
}

static void set_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr, void *value)
{
    if (!s_started) return;
    if (!esp_zb_lock_acquire(portMAX_DELAY)) return;
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint, cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE, attr, value, false);
    esp_zb_lock_release();
    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set attr ep=%u cluster=0x%04x attr=0x%04x status=0x%x",
                 endpoint, cluster, attr, status);
    }
}

static void report_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    if (!s_joined) return;
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,
            .dst_endpoint = 1,
            .src_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = attr,
    };
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    if (err == ESP_OK) status_led_indicate_ha_publish();
}

static void cache_state(const device_state_t *state)
{
    for (uint8_t i = 0; i < ZB_SWITCH_COUNT; ++i) {
        s_on_off[i] = (state->switches & (1U << i)) != 0;
    }
    const bool any = (state->switches & DEVICE_SWITCH_MASK) != 0;
    s_on_off[6] = any;
    s_on_off[7] = any;
    s_on_off[8] = state->rs232_enabled;
    s_level = percent_to_level(state->brightness);
    s_color_temp = kelvin_to_mired(state->white_temperature);
    s_color_temp_startup = s_color_temp;
}

void zigbee_publish_state(const device_state_t *state)
{
    if (state == NULL) return;
    cache_state(state);
    if (!s_started) return;

    for (uint8_t endpoint = 1; endpoint <= ZB_ENDPOINT_COUNT; ++endpoint) {
        set_attr(endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                 ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &s_on_off[endpoint - 1]);
        report_attr(endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    }
    set_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
             ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &s_level);
    set_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
             ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temp);
    report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
    report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
}

static esp_err_t add_on_off_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    esp_zb_on_off_light_cfg_t cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    cfg.on_off_cfg.on_off = s_on_off[endpoint - 1];
    esp_zb_cluster_list_t *clusters = esp_zb_on_off_light_clusters_create(&cfg);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_NO_MEM, TAG, "endpoint cluster allocation failed");
    const esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    return esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
}

static esp_err_t add_light_clusters(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *clusters = esp_zb_ep_list_get_ep(ep_list, ZB_LIGHT_ENDPOINT);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_INVALID_ARG, TAG, "light endpoint missing");

    esp_zb_level_cluster_cfg_t level_cfg = {.current_level = s_level};
    esp_zb_attribute_list_t *level = esp_zb_level_cluster_create(&level_cfg);
    ESP_RETURN_ON_FALSE(level != NULL, ESP_ERR_NO_MEM, TAG, "level cluster allocation failed");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_level_cluster(
        clusters, level, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE), TAG, "level cluster add failed");

    esp_zb_color_cluster_cfg_t color_cfg = {
        .current_x = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_X_DEF_VALUE,
        .current_y = ESP_ZB_ZCL_COLOR_CONTROL_CURRENT_Y_DEF_VALUE,
        .color_mode = 0x02,
        .options = ESP_ZB_ZCL_COLOR_CONTROL_OPTIONS_DEFAULT_VALUE,
        .enhanced_color_mode = 0x02,
        .color_capabilities = ZB_COLOR_TEMP_CAPABILITY,
    };
    esp_zb_attribute_list_t *color = esp_zb_color_control_cluster_create(&color_cfg);
    ESP_RETURN_ON_FALSE(color != NULL, ESP_ERR_NO_MEM, TAG, "color cluster allocation failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temp), TAG, "color temp attr failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &s_color_temp_min), TAG, "min temp attr failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &s_color_temp_max), TAG, "max temp attr failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID, &s_color_temp_startup), TAG, "startup temp attr failed");
    return esp_zb_cluster_list_add_color_control_cluster(clusters, color, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
}

static esp_err_t add_basic_identity(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    static uint8_t manufacturer[] = "\x05" "Jaros";
    static uint8_t model[] = "\x15" "RemoteControl7Encoder";
    static uint8_t sw[] = "\x05" "1.0.0";
    esp_zb_cluster_list_t *clusters = esp_zb_ep_list_get_ep(ep_list, endpoint);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_INVALID_ARG, TAG, "endpoint missing");
    esp_zb_attribute_list_t *basic = esp_zb_cluster_list_get_cluster(
        clusters, ESP_ZB_ZCL_CLUSTER_ID_BASIC, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
    ESP_RETURN_ON_FALSE(basic != NULL, ESP_ERR_INVALID_ARG, TAG, "basic cluster missing");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer), TAG, "manufacturer failed");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model), TAG, "model failed");
    return esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw);
}

static esp_err_t attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_ERR_INVALID_ARG;
    status_led_indicate_ha_command();

    const uint8_t endpoint = message->info.dst_endpoint;
    if (endpoint >= 1 && endpoint <= ZB_ENDPOINT_COUNT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL && message->attribute.data.value != NULL) {
        const bool on = *(bool *)message->attribute.data.value;
        if (endpoint <= ZB_SWITCH_COUNT) state_set_switch(endpoint, on, STATE_CHANGE_ZIGBEE);
        else if (endpoint == 7 || endpoint == ZB_LIGHT_ENDPOINT) {
            if (on) state_restore_last(STATE_CHANGE_ZIGBEE); else state_all_off(STATE_CHANGE_ZIGBEE);
        } else if (endpoint == ZB_RS232_ENDPOINT) {
            state_set_rs232_enabled_source(on, STATE_CHANGE_ZIGBEE);
        }
        return ESP_OK;
    }

    if (endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && message->attribute.data.value != NULL) {
        state_set_brightness(level_to_percent(*(uint8_t *)message->attribute.data.value), STATE_CHANGE_ZIGBEE);
        return ESP_OK;
    }

    if (endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && message->attribute.data.value != NULL) {
        state_set_white_temperature(mired_to_kelvin(*(uint16_t *)message->attribute.data.value), STATE_CHANGE_ZIGBEE);
    }
    return ESP_OK;
}

static esp_err_t privilege_handler(const esp_zb_zcl_privilege_command_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS || message->data == NULL) return ESP_ERR_INVALID_ARG;
    if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->info.command.id == ESP_ZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE &&
        message->size >= sizeof(uint16_t)) {
        status_led_indicate_ha_command();
        state_set_white_temperature(mired_to_kelvin(read_u16_le(message->data)), STATE_CHANGE_ZIGBEE);
    }
    return ESP_OK;
}

static esp_err_t action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    switch (callback_id) {
        case ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID:
            return attribute_handler((const esp_zb_zcl_set_attr_value_message_t *)message);
        case ESP_ZB_CORE_CMD_PRIVILEGE_COMMAND_REQ_CB_ID:
            return privilege_handler((const esp_zb_zcl_privilege_command_message_t *)message);
        default:
            return ESP_OK;
    }
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    if (signal_struct == NULL || signal_struct->p_app_signal == NULL) return;
    const esp_zb_app_signal_type_t sig = *(esp_zb_app_signal_type_t *)signal_struct->p_app_signal;
    const esp_err_t status = signal_struct->esp_err_status;
    switch (sig) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            (void)esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
            break;
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            s_joined = !esp_zb_bdb_is_factory_new();
            if (esp_zb_bdb_is_factory_new()) {
                apply_channel_mask(channel_mask(s_fast_pair_channel));
                start_steering("factory-new");
            } else {
                status_led_set_zigbee_joined(true);
                const uint8_t channel = esp_zb_get_current_channel();
                if (channel_valid(channel)) storage_save_zigbee_last_channel(channel);
                zigbee_publish_state(state_get());
            }
            break;
        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (status == ESP_OK) {
                s_joined = true;
                s_steering_fail_count = 0;
                status_led_set_failure(false);
                status_led_set_zigbee_joined(true);
                const uint8_t channel = esp_zb_get_current_channel();
                if (channel_valid(channel)) {
                    storage_save_zigbee_last_channel(channel);
                    s_fast_pair_channel = channel;
                }
                zigbee_publish_state(state_get());
            } else {
                s_joined = false;
                ++s_steering_fail_count;
                if (s_steering_fail_count >= ZB_FAST_ATTEMPTS &&
                    s_current_channel_mask == channel_mask(s_fast_pair_channel)) {
                    apply_channel_mask(configured_scan_mask());
                }
                esp_zb_scheduler_alarm((esp_zb_callback_t)steering_retry_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
            }
            break;
        default:
            break;
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;
    load_fast_pair_channel();
    cache_state(state_get());

    esp_zb_cfg_t cfg = ESP_ZB_APP_ZED_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_on_off_light_cfg_t first_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    first_cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    first_cfg.on_off_cfg.on_off = s_on_off[0];
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(1, &first_cfg);
    ESP_ERROR_CHECK(ep_list != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    for (uint8_t endpoint = 2; endpoint <= ZB_ENDPOINT_COUNT; ++endpoint) {
        ESP_ERROR_CHECK(add_on_off_endpoint(ep_list, endpoint));
    }
    ESP_ERROR_CHECK(add_light_clusters(ep_list));
    for (uint8_t endpoint = 1; endpoint <= ZB_ENDPOINT_COUNT; ++endpoint) {
        ESP_ERROR_CHECK(add_basic_identity(ep_list, endpoint));
    }

    ESP_LOGI(TAG, "registering application endpoints 1..9 plus OTA endpoints 10/11");
    ESP_ERROR_CHECK(jarzem_ota_device_register(ep_list));
    ESP_ERROR_CHECK(esp_zb_zcl_add_privilege_command(
        ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
        ESP_ZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE));
    jarzem_ota_action_handler_register(action_handler);
    esp_zb_set_rx_on_when_idle(true);
    apply_channel_mask(channel_mask(s_fast_pair_channel));
    ESP_ERROR_CHECK(esp_zb_start(false));
    s_started = true;
    ESP_LOGI(TAG, "application endpoints 1..9 + OTA endpoints 10/11 registered");
    esp_zb_stack_main_loop();
}

void zigbee_init(void)
{
    const esp_zb_platform_config_t platform = {
        .radio_config = ESP_ZB_APP_RADIO_CONFIG(),
        .host_config = ESP_ZB_APP_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config((esp_zb_platform_config_t *)&platform));
    BaseType_t created = xTaskCreate(zigbee_task, "Zigbee_main", ZB_TASK_STACK,
                                     NULL, ZB_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);
}

void zigbee_request_repair(void)
{
    if (!s_started || s_manual_repair_requested) return;
    s_manual_repair_requested = true;
    esp_zb_scheduler_alarm((esp_zb_callback_t)repair_cb, 0, 0);
}

void zigbee_handle_rs232_enabled_from_ha(bool enabled)
{
    state_set_rs232_enabled_source(enabled, STATE_CHANGE_ZIGBEE);
}

void zigbee_publish_communication_error(bool communication_error, const void *diagnostics)
{
    (void)communication_error;
    (void)diagnostics;
}
