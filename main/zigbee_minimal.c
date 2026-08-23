#include "zigbee_minimal.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"
#include "event_bus.h"
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
#include "status_led.h"
#include "storage.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"

#if !defined ZB_ED_ROLE
#error "Minimal Zigbee firmware must be built as Zigbee End Device."
#endif

static const char *TAG = "zb_minimal";

#define ZB_FIRST_SWITCH_ENDPOINT 1
#define ZB_ENDPOINT_COUNT        9
#define ZB_SWITCH_COUNT          6
#define ZB_LIGHT_ENDPOINT        8
#define ZB_RS232_ENDPOINT        9
#define ZB_MIN_DEFAULT_CHANNEL   CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL
#define ZB_MIN_SCAN_CHANNEL_MASK CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK
#define ZB_MIN_FAST_ATTEMPTS     CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS
#define ZB_MIN_ED_AGING_TIMEOUT  ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ZB_MIN_ED_KEEP_ALIVE_MS  3000
#define ZB_MIN_INSTALL_CODE      false
#define ZB_MIN_TASK_STACK        4096
#define ZB_MIN_TASK_PRIORITY     5
#define ZB_MIN_REPORT_DELAY_MS   100
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
static const uint8_t s_switch_endpoints[ZB_ENDPOINT_COUNT] = {1,2,3,4,5,6,7,8,9};

#define ESP_ZB_MIN_ZED_CONFIG() { \
    .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, \
    .install_code_policy = ZB_MIN_INSTALL_CODE, \
    .nwk_cfg.zed_cfg = { \
        .ed_timeout = ZB_MIN_ED_AGING_TIMEOUT, \
        .keep_alive = ZB_MIN_ED_KEEP_ALIVE_MS, \
    }, \
}
#define ESP_ZB_MIN_RADIO_CONFIG() { .radio_mode = ZB_RADIO_MODE_NATIVE }
#define ESP_ZB_MIN_HOST_CONFIG()  { .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE }

static int endpoint_to_index(uint8_t endpoint)
{
    for (size_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        if (s_switch_endpoints[i] == endpoint) return (int)i;
    }
    return -1;
}

static uint8_t percent_to_level(uint8_t percent)
{
    if (percent > 100) percent = 100;
    return (uint8_t)(((uint16_t)percent * 254U + 50U) / 100U);
}

static uint8_t level_to_percent(uint8_t level)
{
    return (uint8_t)(((uint16_t)level * 100U + 127U) / 254U);
}

static uint16_t kelvin_to_mired(uint16_t kelvin)
{
    if (kelvin < ZB_WHITE_TEMP_MIN_K) kelvin = ZB_WHITE_TEMP_MIN_K;
    if (kelvin > ZB_WHITE_TEMP_MAX_K) kelvin = ZB_WHITE_TEMP_MAX_K;
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
    return ZB_MIN_SCAN_CHANNEL_MASK != 0 ? ZB_MIN_SCAN_CHANNEL_MASK : 0x07FFF800UL;
}

static void apply_channel_mask(uint32_t mask, const char *reason)
{
    if (mask == 0) mask = configured_scan_mask();
    s_current_channel_mask = mask;
    esp_zb_set_primary_network_channel_set(mask);
    ESP_LOGI(TAG, "Zigbee channel mask %s=0x%08lx", reason, (unsigned long)mask);
}

static void load_fast_pair_channel(void)
{
    uint8_t stored = 0;
    if (storage_load_zigbee_last_channel(&stored) && channel_valid(stored)) {
        s_fast_pair_channel = stored;
    } else {
        s_fast_pair_channel = channel_valid(ZB_MIN_DEFAULT_CHANNEL) ? ZB_MIN_DEFAULT_CHANNEL : 11;
    }
    ESP_LOGI(TAG, "fast-pair channel=%u", s_fast_pair_channel);
}

static void start_network_steering(const char *reason)
{
    status_led_set_failure(false);
    status_led_set_zigbee_pairing(true);
    ESP_LOGI(TAG, "network steering reason=%s mask=0x%08lx", reason, (unsigned long)s_current_channel_mask);
    esp_err_t err = esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "network steering failed to start: %s", esp_err_to_name(err));
        status_led_set_failure(true);
    }
}

static void steering_retry_cb(uint8_t mode)
{
    (void)mode;
    start_network_steering("retry");
}

static void manual_repair_cb(uint8_t unused)
{
    (void)unused;
    s_manual_repair_requested = false;
    status_led_set_failure(false);
    status_led_set_zigbee_joined(false);
    status_led_set_zigbee_pairing(true);
    if (esp_zb_bdb_is_factory_new()) {
        s_steering_fail_count = 0;
        apply_channel_mask(channel_mask(s_fast_pair_channel), "manual");
        start_network_steering("manual");
    } else {
        ESP_LOGW(TAG, "manual re-pair: factory reset Zigbee storage and reboot");
        esp_zb_factory_reset();
    }
}

static void publish_input_event(input_event_type_t type, uint8_t input_id, int32_t value)
{
    const input_event_t event = {
        .type = type,
        .input_id = input_id,
        .value = value,
        .tick = xTaskGetTickCount(),
    };
    if (!event_bus_publish(&event)) ESP_LOGW(TAG, "event queue full type=%d", type);
}

static void set_attr_locked(uint8_t endpoint, uint16_t cluster, uint16_t attr, void *value)
{
    if (!s_stack_started) return;
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

static void sync_endpoint_attrs(bool report)
{
    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        set_attr_locked(s_switch_endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &s_endpoint_on[i]);
        if (report) report_attr(s_switch_endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                                ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
    }
    set_attr_locked(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &s_level_current);
    set_attr_locked(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                    ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temperature_mired);
    if (report) {
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                    ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
    }
}

static esp_err_t add_standard_on_off_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint)
{
    esp_zb_on_off_light_cfg_t cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    const int index = endpoint_to_index(endpoint);
    cfg.on_off_cfg.on_off = index >= 0 ? s_endpoint_on[index] : false;
    esp_zb_cluster_list_t *clusters = esp_zb_on_off_light_clusters_create(&cfg);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_NO_MEM, TAG, "cannot create endpoint %u", endpoint);
    const esp_zb_endpoint_config_t ep_cfg = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_LIGHT_DEVICE_ID,
        .app_device_version = 0,
    };
    return esp_zb_ep_list_add_ep(ep_list, clusters, ep_cfg);
}

static esp_err_t add_light_control_clusters(esp_zb_ep_list_t *ep_list)
{
    esp_zb_cluster_list_t *clusters = esp_zb_ep_list_get_ep(ep_list, ZB_LIGHT_ENDPOINT);
    ESP_RETURN_ON_FALSE(clusters != NULL, ESP_ERR_INVALID_ARG, TAG, "light endpoint missing");

    s_level_current = percent_to_level(s_brightness_percent);
    esp_zb_level_cluster_cfg_t level_cfg = {.current_level = s_level_current};
    esp_zb_attribute_list_t *level = esp_zb_level_cluster_create(&level_cfg);
    ESP_RETURN_ON_FALSE(level != NULL, ESP_ERR_NO_MEM, TAG, "level cluster allocation failed");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_level_cluster(
        clusters, level, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE), TAG, "level cluster add failed");

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
    esp_zb_attribute_list_t *color = esp_zb_color_control_cluster_create(&color_cfg);
    ESP_RETURN_ON_FALSE(color != NULL, ESP_ERR_NO_MEM, TAG, "color cluster allocation failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temperature_mired),
        TAG, "ColorTemperature attr add failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MIN_MIREDS_ID, &s_color_temperature_min_mired),
        TAG, "ColorTemperature min attr add failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMP_PHYSICAL_MAX_MIREDS_ID, &s_color_temperature_max_mired),
        TAG, "ColorTemperature max attr add failed");
    ESP_RETURN_ON_ERROR(esp_zb_color_control_cluster_add_attr(
        color, ESP_ZB_ZCL_ATTR_COLOR_CONTROL_START_UP_COLOR_TEMPERATURE_MIREDS_ID, &s_color_temperature_startup_mired),
        TAG, "ColorTemperature startup attr add failed");
    return esp_zb_cluster_list_add_color_control_cluster(
        clusters, color, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);
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
    ESP_RETURN_ON_FALSE(basic != NULL, ESP_ERR_INVALID_ARG, TAG, "Basic cluster missing");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, manufacturer), TAG, "manufacturer attr failed");
    ESP_RETURN_ON_ERROR(esp_zb_basic_cluster_add_attr(
        basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, model), TAG, "model failed");
    return esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID, sw);
}

static esp_err_t attribute_handler(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_ERR_INVALID_ARG;

    status_led_indicate_ha_command();
    const int index = endpoint_to_index(message->info.dst_endpoint);
    if (index >= 0 && message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_BOOL && message->attribute.data.value != NULL) {
        bool on = *(bool *)message->attribute.data.value;
        s_endpoint_on[index] = on;
        report_attr(message->info.dst_endpoint, ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
        if (index < ZB_SWITCH_COUNT) publish_input_event(INPUT_EVENT_ZIGBEE_SWITCH_SET, (uint8_t)(index + 1), on);
        else if (index == 6 || index == 7) publish_input_event(INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET, 0, on);
        else if (index == 8) publish_input_event(INPUT_EVENT_ZIGBEE_RS232_SET, 0, on);
        return ESP_OK;
    }

    if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U8 && message->attribute.data.value != NULL) {
        s_level_current = *(uint8_t *)message->attribute.data.value;
        s_brightness_percent = level_to_percent(s_level_current);
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
        publish_input_event(INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET, 0, s_brightness_percent);
        return ESP_OK;
    }

    if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->attribute.id == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID &&
        message->attribute.data.type == ESP_ZB_ZCL_ATTR_TYPE_U16 && message->attribute.data.value != NULL) {
        s_color_temperature_mired = *(uint16_t *)message->attribute.data.value;
        if (s_color_temperature_mired < ZB_MIN_MIREDS) s_color_temperature_mired = ZB_MIN_MIREDS;
        if (s_color_temperature_mired > ZB_MAX_MIREDS) s_color_temperature_mired = ZB_MAX_MIREDS;
        s_white_temperature_kelvin = mired_to_kelvin(s_color_temperature_mired);
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                    ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
        publish_input_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, 0, s_white_temperature_kelvin);
    }
    return ESP_OK;
}

static esp_err_t privilege_handler(const esp_zb_zcl_privilege_command_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS || message->data == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (message->info.dst_endpoint == ZB_LIGHT_ENDPOINT &&
        message->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
        message->info.command.id == ESP_ZB_ZCL_CMD_COLOR_CONTROL_MOVE_TO_COLOR_TEMPERATURE &&
        message->size >= sizeof(uint16_t)) {
        status_led_indicate_ha_command();
        s_color_temperature_mired = read_u16_le(message->data);
        if (s_color_temperature_mired < ZB_MIN_MIREDS) s_color_temperature_mired = ZB_MIN_MIREDS;
        if (s_color_temperature_mired > ZB_MAX_MIREDS) s_color_temperature_mired = ZB_MAX_MIREDS;
        s_white_temperature_kelvin = mired_to_kelvin(s_color_temperature_mired);
        publish_input_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, 0, s_white_temperature_kelvin);
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
                apply_channel_mask(channel_mask(s_fast_pair_channel), "factory-new");
                start_network_steering("factory-new");
            } else {
                status_led_set_zigbee_joined(true);
                const uint8_t channel = esp_zb_get_current_channel();
                if (channel_valid(channel)) storage_save_zigbee_last_channel(channel);
                sync_endpoint_attrs(true);
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
                sync_endpoint_attrs(true);
            } else {
                s_joined = false;
                ++s_steering_fail_count;
                if (s_steering_fail_count >= ZB_MIN_FAST_ATTEMPTS &&
                    s_current_channel_mask == channel_mask(s_fast_pair_channel)) {
                    apply_channel_mask(configured_scan_mask(), "fallback");
                }
                status_led_set_zigbee_pairing(true);
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

    esp_zb_cfg_t cfg = ESP_ZB_MIN_ZED_CONFIG();
    esp_zb_init(&cfg);

    esp_zb_on_off_light_cfg_t first_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    first_cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    first_cfg.on_off_cfg.on_off = s_endpoint_on[0];
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(ZB_FIRST_SWITCH_ENDPOINT, &first_cfg);
    ESP_ERROR_CHECK(ep_list != NULL ? ESP_OK : ESP_ERR_NO_MEM);

    for (uint8_t endpoint = 2; endpoint <= ZB_ENDPOINT_COUNT; ++endpoint) {
        ESP_ERROR_CHECK(add_standard_on_off_endpoint(ep_list, endpoint));
    }
    ESP_ERROR_CHECK(add_light_control_clusters(ep_list));
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
    apply_channel_mask(channel_mask(s_fast_pair_channel), "initial");
    ESP_ERROR_CHECK(esp_zb_start(false));
    s_stack_started = true;
    ESP_LOGI(TAG, "application endpoints 1..9 + OTA endpoints 10/11 registered");
    esp_zb_stack_main_loop();
}

void zigbee_minimal_apply_state(const device_state_t *state, bool ota_enabled, bool report)
{
    (void)ota_enabled;
    if (state == NULL) return;

    bool previous_endpoint_on[ZB_ENDPOINT_COUNT];
    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        previous_endpoint_on[i] = s_endpoint_on[i];
    }
    const uint8_t previous_level = s_level_current;
    const uint16_t previous_color_temperature = s_color_temperature_mired;

    for (uint8_t i = 0; i < ZB_SWITCH_COUNT; ++i) {
        s_endpoint_on[i] = (state->switches & (1U << i)) != 0;
    }
    const bool any_on = (state->switches & ((1U << ZB_SWITCH_COUNT) - 1U)) != 0;
    s_endpoint_on[6] = any_on;
    s_endpoint_on[7] = any_on;
    s_endpoint_on[8] = state->rs232_enabled;
    s_brightness_percent = state->brightness > 100 ? 100 : state->brightness;
    s_white_temperature_kelvin = state->white_temperature;
    if (s_white_temperature_kelvin < ZB_WHITE_TEMP_MIN_K) s_white_temperature_kelvin = ZB_WHITE_TEMP_MIN_K;
    if (s_white_temperature_kelvin > ZB_WHITE_TEMP_MAX_K) s_white_temperature_kelvin = ZB_WHITE_TEMP_MAX_K;
    s_level_current = percent_to_level(s_brightness_percent);
    s_color_temperature_mired = kelvin_to_mired(s_white_temperature_kelvin);
    s_color_temperature_startup_mired = s_color_temperature_mired;

    for (uint8_t i = 0; i < ZB_ENDPOINT_COUNT; ++i) {
        set_attr_locked(s_switch_endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &s_endpoint_on[i]);
        if (report && previous_endpoint_on[i] != s_endpoint_on[i]) {
            report_attr(s_switch_endpoints[i], ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
        }
    }

    set_attr_locked(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID, &s_level_current);
    if (report && previous_level != s_level_current) {
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL,
                    ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID);
    }

    set_attr_locked(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                    ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID, &s_color_temperature_mired);
    if (report && previous_color_temperature != s_color_temperature_mired) {
        report_attr(ZB_LIGHT_ENDPOINT, ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL,
                    ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID);
    }
}

void zigbee_minimal_request_repair(void)
{
    if (!s_stack_started || s_manual_repair_requested) return;
    s_manual_repair_requested = true;
    esp_zb_scheduler_alarm((esp_zb_callback_t)manual_repair_cb, 0, 0);
}

void zigbee_minimal_init(void)
{
    const esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_MIN_RADIO_CONFIG(),
        .host_config = ESP_ZB_MIN_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config((esp_zb_platform_config_t *)&config));
    BaseType_t created = xTaskCreate(zigbee_task, "Zigbee_main", ZB_MIN_TASK_STACK,
                                     NULL, ZB_MIN_TASK_PRIORITY, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);
}
