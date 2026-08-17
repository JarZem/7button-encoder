#include "zigbee.h"

#include <stdbool.h>
#include <stdint.h>
#include "sdkconfig.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "esp_zigbee_endpoint.h"
#include "debug_console.h"
#include "event_bus.h"
#include "fatal_error.h"
#include "fw_identity.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ha/esp_zigbee_ha_standard.h"
#include "nwk/esp_zigbee_nwk.h"
#include "platform/esp_zigbee_platform.h"
#include "zcl/esp_zigbee_zcl_basic.h"
#include "zcl/esp_zigbee_zcl_color_control.h"
#include "zcl/esp_zigbee_zcl_command.h"
#include "zcl/esp_zigbee_zcl_common.h"
#include "zcl/esp_zigbee_zcl_level.h"
#include "zcl/esp_zigbee_zcl_on_off.h"
#include "power_manager.h"
#include "status_led.h"
#include "storage.h"
#include "zigbee_ota_cluster.h"

static const char *TAG = "zigbee";

#define ZB_REMOTE_ENDPOINT       1
#define ZB_SWITCH_FIRST_ENDPOINT 1
#define ZB_SWITCH_LAST_ENDPOINT  6
#define ZB_LIGHT_ENDPOINT        ZB_REMOTE_ENDPOINT
#define ZB_RS232_ENDPOINT        ZB_REMOTE_ENDPOINT
#define ZB_OTA_ENDPOINT          ZB_REMOTE_ENDPOINT

#define ZB_REMOTE_CLUSTER_ID             ZIGBEE_OTA_CLUSTER_ID
#define ZB_REMOTE_ATTR_SWITCHES          0x0000
#define ZB_REMOTE_ATTR_SWITCH1           0x0002
#define ZB_REMOTE_ATTR_SWITCH2           0x0003
#define ZB_REMOTE_ATTR_SWITCH3           0x0004
#define ZB_REMOTE_ATTR_SWITCH4           0x0005
#define ZB_REMOTE_ATTR_SWITCH5           0x0006
#define ZB_REMOTE_ATTR_SWITCH6           0x0007
#define ZB_REMOTE_ATTR_BRIGHTNESS        0x0010
#define ZB_REMOTE_ATTR_WHITE_TEMPERATURE 0x0011
#define ZB_REMOTE_ATTR_RS232_ENABLED     0x0020
#define ZB_REMOTE_ATTR_OTA_REQUEST       0x0021

#define ZB_DEFAULT_CHANNEL       CONFIG_APP_ZIGBEE_DEFAULT_CHANNEL
#define ZB_SCAN_CHANNEL_MASK     CONFIG_APP_ZIGBEE_SCAN_CHANNEL_MASK
#define ZB_FAST_STEERING_ATTEMPTS CONFIG_APP_ZIGBEE_FAST_STEERING_ATTEMPTS
#define ZB_TARGET_PAN_ID         0x1857
#define ZB_ED_AGING_TIMEOUT      ESP_ZB_ED_AGING_TIMEOUT_64MIN
#define ZB_ED_KEEP_ALIVE_MS      3000
#define ZB_REPORT_MIN_INTERVAL_S 1
#define ZB_REPORT_MAX_INTERVAL_S 300
#define ZB_PAIRING_DEBUG_INTERVAL_MS 10000
#define ZB_PAIRING_MINIMAL_ENDPOINTS 0
#define ZB_PAIRING_SWITCH_ENDPOINTS_ONLY 0

#define ZB_COLOR_MODE_TEMP_MIREDS 0x02
#define ZB_COLOR_CAP_TEMP_MIREDS  0x0010
#define ZB_MIN_MIREDS             154
#define ZB_MAX_MIREDS             333

#define INSTALLCODE_POLICY_ENABLE false

#define ESP_ZB_REMOTE_CONFIG()                                         \
    {                                                                  \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED,                          \
        .install_code_policy = INSTALLCODE_POLICY_ENABLE,              \
        .nwk_cfg.zed_cfg = {                                           \
            .ed_timeout = ZB_ED_AGING_TIMEOUT,                         \
            .keep_alive = ZB_ED_KEEP_ALIVE_MS,                         \
        },                                                             \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG()                 \
    {                                                  \
        .radio_mode = ZB_RADIO_MODE_NATIVE,            \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG()                   \
    {                                                  \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }

static bool s_started;
static bool s_joined;
static bool s_factory_new = true;
static bool s_steering_active;
static bool s_manual_repair_requested;
static uint8_t s_fast_pair_channel;
static uint8_t s_steering_fail_count;
static uint32_t s_current_channel_mask;
static uint8_t s_current_channel;
static esp_timer_handle_t s_pairing_debug_timer;
static device_state_t s_last_published;
static uint8_t s_attr_switches;
static bool s_attr_switch_state[DEVICE_SWITCH_COUNT];
static uint8_t s_attr_brightness;
static uint16_t s_attr_white_temperature;
static bool s_attr_rs232_enabled;
static bool s_attr_ota_request;
static uint8_t s_basic_manufacturer[] = {9, 'J', 'a', 'r', 'o', 's', 'l', 'a', 'v', 'Z'};
static uint8_t s_basic_model[] = {12, 'E', 'S', 'P', '3', '2', '-', 'C', '6', '-', 'E', 'N', 'C'};
static uint8_t s_basic_sw_build[] = {21, 'r', 'e', 'm', 'o', 't', 'e', 'c', 'o', 'n', 't', 'r', 'o', 'l', ':', 'e', 'n', 'c', 'o', 'd', 'e', 'r'};

static void debug_pairing_state(const char *event, esp_err_t status);

static uint8_t level_to_percent(uint8_t level)
{
    return (uint8_t)(((uint16_t)level * 100U + 127U) / 254U);
}

static uint16_t mired_to_kelvin(uint16_t mired)
{
    if (mired < ZB_MIN_MIREDS) {
        mired = ZB_MIN_MIREDS;
    } else if (mired > ZB_MAX_MIREDS) {
        mired = ZB_MAX_MIREDS;
    }

    uint32_t kelvin = (1000000UL + (mired / 2U)) / mired;
    if (kelvin < DEVICE_WHITE_TEMP_MIN) {
        kelvin = DEVICE_WHITE_TEMP_MIN;
    } else if (kelvin > DEVICE_WHITE_TEMP_MAX) {
        kelvin = DEVICE_WHITE_TEMP_MAX;
    }
    return (uint16_t)kelvin;
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
    uint32_t mask = ZB_SCAN_CHANNEL_MASK;
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
    esp_zb_set_secondary_network_channel_set(mask);
    ESP_LOGI(TAG, "Zigbee steering channel mask=%s 0x%08lx fast_channel=%u fallback_mask=0x%08lx attempts=%u",
             reason != NULL ? reason : "set",
             (unsigned long)s_current_channel_mask,
             s_fast_pair_channel,
             (unsigned long)zigbee_configured_scan_mask(),
             ZB_FAST_STEERING_ATTEMPTS);
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

    s_fast_pair_channel = ZB_DEFAULT_CHANNEL;
    if (!zigbee_channel_is_valid(s_fast_pair_channel)) {
        s_fast_pair_channel = 11;
    }
    ESP_LOGI(TAG, "Zigbee fast-pair channel from build config: %u", s_fast_pair_channel);
}

static void start_network_steering_now(const char *reason)
{
    s_steering_active = true;
    ESP_LOGI(TAG, "start network steering reason=%s mask=0x%08lx fail_count=%u/%u",
             reason != NULL ? reason : "unknown",
             (unsigned long)s_current_channel_mask,
             s_steering_fail_count,
             ZB_FAST_STEERING_ATTEMPTS);
    status_led_set_failure(false);
    status_led_set_zigbee_pairing(true);
    debug_pairing_state(reason != NULL ? reason : "steering_start", ESP_OK);
    FATAL_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING));
}

static void publish_event(input_event_type_t type, uint8_t input_id, int32_t value)
{
    const input_event_t event = {
        .type = type,
        .input_id = input_id,
        .value = value,
        .tick = xTaskGetTickCount(),
    };

    if (!event_bus_publish(&event)) {
        ESP_LOGW(TAG, "Zigbee událost zahozena, fronta je plná");
    }
}

static void debug_pairing_state(const char *event, esp_err_t status)
{
    const char *status_name = esp_err_to_name(status);
    ESP_LOGI(TAG,
             "Pairing event=%s joined=%s factory_new=%s steering=%s channel_mask=0x%08lx current_channel=%u status=%s",
             event != NULL ? event : "state",
             s_joined ? "true" : "false",
             s_factory_new ? "true" : "false",
             s_steering_active ? "true" : "false",
             (unsigned long)s_current_channel_mask,
             s_current_channel,
             status_name);
    debug_console_publish_zigbee_pairing(event,
                                         s_joined,
                                         s_factory_new,
                                         s_steering_active,
                                         s_current_channel_mask,
                                         s_current_channel,
                                         status_name);
}

static void pairing_debug_timer_callback(void *arg)
{
    (void)arg;

    if (!s_joined) {
        debug_pairing_state("wait", ESP_OK);
    }
}

static void schedule_pairing_debug(void)
{
    if (!s_joined && s_pairing_debug_timer != NULL) {
        (void)esp_timer_stop(s_pairing_debug_timer);
        (void)esp_timer_start_periodic(s_pairing_debug_timer,
                                       ZB_PAIRING_DEBUG_INTERVAL_MS * 1000ULL);
    }
}

static void stop_pairing_debug(void)
{
    if (s_pairing_debug_timer != NULL) {
        (void)esp_timer_stop(s_pairing_debug_timer);
    }
}

static void restart_pairing_debug(void)
{
    stop_pairing_debug();
    schedule_pairing_debug();
}

static void sync_attr_cache(const device_state_t *state)
{
    if (state == NULL) {
        return;
    }

    s_attr_switches = state->switches & DEVICE_SWITCH_MASK;
    for (uint8_t i = 0; i < DEVICE_SWITCH_COUNT; ++i) {
        s_attr_switch_state[i] = ((s_attr_switches >> i) & 1U) != 0;
    }
    s_attr_brightness = state->brightness;
    s_attr_white_temperature = state->white_temperature;
    s_attr_rs232_enabled = state->rs232_enabled;
    s_attr_ota_request = false;
}

static esp_err_t add_remote_attr(esp_zb_attribute_list_t *cluster, uint16_t attr_id,
                                 uint8_t attr_type, void *value)
{
    return esp_zb_custom_cluster_add_custom_attr(
        cluster,
        attr_id,
        attr_type,
        ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE | ESP_ZB_ZCL_ATTR_ACCESS_REPORTING,
        value);
}

static void add_remote_cluster(esp_zb_cluster_list_t *clusters, const device_state_t *state)
{
    sync_attr_cache(state);

    esp_zb_attribute_list_t *remote_cluster = esp_zb_zcl_attr_list_create(ZB_REMOTE_CLUSTER_ID);
    FATAL_ERROR_IF(remote_cluster == NULL, "esp_zb_zcl_attr_list_create remote");

    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCHES,
                                      ESP_ZB_ZCL_ATTR_TYPE_8BITMAP, &s_attr_switches));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH1,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[0]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH2,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[1]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH3,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[2]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH4,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[3]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH5,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[4]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_SWITCH6,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_switch_state[5]));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_BRIGHTNESS,
                                      ESP_ZB_ZCL_ATTR_TYPE_U8, &s_attr_brightness));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_WHITE_TEMPERATURE,
                                      ESP_ZB_ZCL_ATTR_TYPE_U16, &s_attr_white_temperature));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_RS232_ENABLED,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_rs232_enabled));
    FATAL_ERROR_CHECK(add_remote_attr(remote_cluster, ZB_REMOTE_ATTR_OTA_REQUEST,
                                      ESP_ZB_ZCL_ATTR_TYPE_BOOL, &s_attr_ota_request));
    FATAL_ERROR_CHECK(zigbee_ota_cluster_add_attrs(remote_cluster));

    FATAL_ERROR_CHECK(esp_zb_cluster_list_add_custom_cluster(
        clusters,
        remote_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));
}


static void add_endpoint(esp_zb_ep_list_t *ep_list, uint8_t endpoint, uint16_t device_id,
                         esp_zb_cluster_list_t *clusters)
{
    const esp_zb_endpoint_config_t endpoint_config = {
        .endpoint = endpoint,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = device_id,
        .app_device_version = 0,
    };
    FATAL_ERROR_CHECK(esp_zb_ep_list_add_ep(ep_list, clusters, endpoint_config));
}

static void add_remote_endpoint(esp_zb_ep_list_t *ep_list, const device_state_t *state)
{
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_identify_cluster_cfg_t identify_cfg = {
        .identify_time = ESP_ZB_ZCL_IDENTIFY_IDENTIFY_TIME_DEFAULT_VALUE,
    };

    esp_zb_cluster_list_t *clusters = esp_zb_zcl_cluster_list_create();
    FATAL_ERROR_IF(clusters == NULL, "esp_zb_zcl_cluster_list_create remote");

    esp_zb_attribute_list_t *basic_cluster = esp_zb_basic_cluster_create(&basic_cfg);
    FATAL_ERROR_IF(basic_cluster == NULL, "esp_zb_basic_cluster_create");
    FATAL_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
        s_basic_manufacturer));
    FATAL_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
        s_basic_model));
    FATAL_ERROR_CHECK(esp_zb_basic_cluster_add_attr(
        basic_cluster,
        ESP_ZB_ZCL_ATTR_BASIC_SW_BUILD_ID,
        s_basic_sw_build));
    FATAL_ERROR_CHECK(esp_zb_cluster_list_add_basic_cluster(
        clusters,
        basic_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    esp_zb_attribute_list_t *identify_cluster = esp_zb_identify_cluster_create(&identify_cfg);
    FATAL_ERROR_IF(identify_cluster == NULL, "esp_zb_identify_cluster_create");
    FATAL_ERROR_CHECK(esp_zb_cluster_list_add_identify_cluster(
        clusters,
        identify_cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE));

    add_remote_cluster(clusters, state);
    add_endpoint(ep_list, ZB_REMOTE_ENDPOINT, ESP_ZB_HA_CUSTOM_ATTR_DEVICE_ID, clusters);
}

static void set_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr, void *value)
{
    if (!s_started) {
        return;
    }

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(
        endpoint,
        cluster,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        attr,
        value,
        false);
    esp_zb_lock_release();

    if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "Set attr ep=%u cluster=0x%04x attr=0x%04x status=0x%x",
                 endpoint, cluster, attr, status);
    }
}

static void report_attr(uint8_t endpoint, uint16_t cluster, uint16_t attr)
{
    if (!s_started || !s_joined) {
        return;
    }

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .src_endpoint = endpoint,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = cluster,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .attributeID = attr,
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Report attr ep=%u cluster=0x%04x attr=0x%04x failed: %s",
                 endpoint, cluster, attr, esp_err_to_name(err));
    }
}

static void configure_attr_reporting(uint8_t endpoint, uint16_t cluster, uint16_t attr, uint16_t delta)
{
    esp_zb_zcl_reporting_info_t info = {
        .direction = ESP_ZB_ZCL_REPORT_DIRECTION_SEND,
        .ep = endpoint,
        .cluster_id = cluster,
        .cluster_role = ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        .attr_id = attr,
        .u.send_info = {
            .min_interval = ZB_REPORT_MIN_INTERVAL_S,
            .max_interval = ZB_REPORT_MAX_INTERVAL_S,
            .delta.u16 = delta,
            .def_min_interval = ZB_REPORT_MIN_INTERVAL_S,
            .def_max_interval = ZB_REPORT_MAX_INTERVAL_S,
        },
    };
    esp_err_t err = esp_zb_zcl_update_reporting_info(&info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Reporting config ep=%u cluster=0x%04x attr=0x%04x failed: %s",
                 endpoint, cluster, attr, esp_err_to_name(err));
    }
}

static void configure_reporting(void)
{
    configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_SWITCHES, 1);
    for (uint16_t attr = ZB_REMOTE_ATTR_SWITCH1; attr <= ZB_REMOTE_ATTR_SWITCH6; ++attr) {
        configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, attr, 1);
    }
    configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_BRIGHTNESS, 1);
    configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_WHITE_TEMPERATURE, 1);
    configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_RS232_ENABLED, 1);
    configure_attr_reporting(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_OTA_REQUEST, 1);
}

static void publish_state_to_zigbee(const device_state_t *state, bool report)
{
    sync_attr_cache(state);

    set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_SWITCHES, (void *)&s_attr_switches);
    for (uint16_t attr = ZB_REMOTE_ATTR_SWITCH1; attr <= ZB_REMOTE_ATTR_SWITCH6; ++attr) {
        set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, attr, (void *)&s_attr_switch_state[attr - ZB_REMOTE_ATTR_SWITCH1]);
    }
    set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_BRIGHTNESS, (void *)&s_attr_brightness);
    set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_WHITE_TEMPERATURE, (void *)&s_attr_white_temperature);
    set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_RS232_ENABLED, (void *)&s_attr_rs232_enabled);
    set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_OTA_REQUEST, (void *)&s_attr_ota_request);

    if (report) {
        status_led_indicate_ha_publish();
        report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_SWITCHES);
        for (uint16_t attr = ZB_REMOTE_ATTR_SWITCH1; attr <= ZB_REMOTE_ATTR_SWITCH6; ++attr) {
            report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, attr);
        }
        report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_BRIGHTNESS);
        report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_WHITE_TEMPERATURE);
        report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_RS232_ENABLED);
        report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_OTA_REQUEST);
    }
}

static void handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS || message->attribute.data.value == NULL) {
        return;
    }

    if (zigbee_ota_cluster_handle_set_attr(message)) {
        status_led_indicate_ha_command();
        return;
    }

    const uint8_t endpoint = message->info.dst_endpoint;
    const uint16_t cluster = message->info.cluster;
    const uint16_t attr = message->attribute.id;

    if (endpoint == ZB_REMOTE_ENDPOINT && cluster == ZB_REMOTE_CLUSTER_ID) {
        if (attr == ZB_REMOTE_ATTR_SWITCHES) {
            const uint8_t switches = *(uint8_t *)message->attribute.data.value;
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_SWITCH_SET, 0, switches & DEVICE_SWITCH_MASK);
        } else if (attr >= ZB_REMOTE_ATTR_SWITCH1 && attr <= ZB_REMOTE_ATTR_SWITCH6) {
            const bool on = *(bool *)message->attribute.data.value;
            const uint8_t switch_id = (uint8_t)(attr - ZB_REMOTE_ATTR_SWITCH1 + 1U);
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_SWITCH_SET, switch_id, on ? 1 : 0);
        } else if (attr == ZB_REMOTE_ATTR_BRIGHTNESS) {
            const uint8_t brightness = *(uint8_t *)message->attribute.data.value;
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET, endpoint, brightness);
        } else if (attr == ZB_REMOTE_ATTR_WHITE_TEMPERATURE) {
            const uint16_t kelvin = *(uint16_t *)message->attribute.data.value;
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, endpoint, kelvin);
        } else if (attr == ZB_REMOTE_ATTR_RS232_ENABLED) {
            const bool enabled = *(bool *)message->attribute.data.value;
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_RS232_SET, endpoint, enabled ? 1 : 0);
        } else if (attr == ZB_REMOTE_ATTR_OTA_REQUEST) {
            const bool requested = *(bool *)message->attribute.data.value;
            if (requested) {
                status_led_indicate_ha_command();
                publish_event(INPUT_EVENT_OTA_REQUEST, endpoint, 1);
                s_attr_ota_request = false;
                set_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_OTA_REQUEST, (void *)&s_attr_ota_request);
                report_attr(ZB_REMOTE_ENDPOINT, ZB_REMOTE_CLUSTER_ID, ZB_REMOTE_ATTR_OTA_REQUEST);
            }
        }
    } else if (cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF && attr == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        const bool on = *(bool *)message->attribute.data.value;
        if (endpoint == ZB_LIGHT_ENDPOINT) {
            status_led_indicate_ha_command();
            publish_event(INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET, endpoint, on ? 1 : 0);
        }
    } else if (endpoint == ZB_LIGHT_ENDPOINT &&
               cluster == ESP_ZB_ZCL_CLUSTER_ID_LEVEL_CONTROL &&
               attr == ESP_ZB_ZCL_ATTR_LEVEL_CONTROL_CURRENT_LEVEL_ID) {
        const uint8_t level = *(uint8_t *)message->attribute.data.value;
        status_led_indicate_ha_command();
        publish_event(INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET, endpoint, level_to_percent(level));
    } else if (endpoint == ZB_LIGHT_ENDPOINT &&
               cluster == ESP_ZB_ZCL_CLUSTER_ID_COLOR_CONTROL &&
               attr == ESP_ZB_ZCL_ATTR_COLOR_CONTROL_COLOR_TEMPERATURE_ID) {
        const uint16_t mired = *(uint16_t *)message->attribute.data.value;
        status_led_indicate_ha_command();
        publish_event(INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET, endpoint, mired_to_kelvin(mired));
    }
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t callback_id, const void *message)
{
    if (callback_id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        handle_set_attr((const esp_zb_zcl_set_attr_value_message_t *)message);
    }
    return ESP_OK;
}

static void bdb_start_top_level_commissioning_cb(uint8_t mode_mask)
{
    (void)mode_mask;
    start_network_steering_now("retry_start");
}

static void manual_repair_cb(uint8_t unused)
{
    (void)unused;

    status_led_set_failure(false);
    status_led_set_zigbee_joined(false);
    status_led_set_zigbee_pairing(true);
    power_manager_prevent_light_sleep(true);

    if (esp_zb_bdb_is_factory_new()) {
        ESP_LOGW(TAG, "manual re-pair: device is factory-new, start network steering now");
        s_factory_new = true;
        s_steering_fail_count = 0;
        apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "manual fast");
        start_network_steering_now("manual_steering_start");
        return;
    }

    ESP_LOGW(TAG, "manual re-pair: factory reset Zigbee storage and reboot");
    esp_zb_factory_reset();
}

static const char *zigbee_signal_name(uint32_t signal)
{
    switch (signal) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP: return "SKIP_STARTUP";
        case ESP_ZB_ZDO_SIGNAL_PRODUCTION_CONFIG_READY: return "PRODUCTION_CONFIG_READY";
        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START: return "DEVICE_FIRST_START";
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT: return "DEVICE_REBOOT";
        case ESP_ZB_BDB_SIGNAL_STEERING: return "STEERING";
        case ESP_ZB_BDB_SIGNAL_STEERING_CANCELLED: return "STEERING_CANCELLED";
        case ESP_ZB_ZDO_SIGNAL_LEAVE: return "LEAVE";
        case ESP_ZB_ZDO_SIGNAL_DEVICE_ANNCE: return "DEVICE_ANNCE";
        case ESP_ZB_ZDO_SIGNAL_DEVICE_AUTHORIZED: return "DEVICE_AUTHORIZED";
        default: return "UNKNOWN";
    }
}

static void log_network_info(const char *prefix)
{
    esp_zb_ieee_addr_t extended_pan_id;
    esp_zb_get_extended_pan_id(extended_pan_id);
    ESP_LOGI(TAG,
             "%s short=0x%04hx pan=0x%04hx channel=%u ext_pan=%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x",
             prefix,
             esp_zb_get_short_address(),
             esp_zb_get_pan_id(),
             s_current_channel,
             extended_pan_id[7],
             extended_pan_id[6],
             extended_pan_id[5],
             extended_pan_id[4],
             extended_pan_id[3],
             extended_pan_id[2],
             extended_pan_id[1],
             extended_pan_id[0]);
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *signal_struct)
{
    uint32_t *signal = signal_struct->p_app_signal;
    const esp_err_t status = signal_struct->esp_err_status;

    ESP_LOGI(TAG, "Zigbee signal=%s(0x%lx) status=%s",
             zigbee_signal_name(*signal),
             (unsigned long)*signal,
             esp_err_to_name(status));

    switch (*signal) {
        case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
            ESP_LOGI(TAG, "Zigbee stack started, steering");
            s_started = true;
            s_factory_new = esp_zb_bdb_is_factory_new();
            s_current_channel = esp_zb_get_current_channel();
            debug_pairing_state("stack_started", status);
            FATAL_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION));
            break;

        case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
        case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
            s_factory_new = esp_zb_bdb_is_factory_new();
            s_steering_active = false;
            s_current_channel = esp_zb_get_current_channel();
            debug_pairing_state(*signal == ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START ? "first_start" : "reboot",
                                status);
            if (status == ESP_OK) {
                if (s_factory_new) {
                    ESP_LOGI(TAG, "Zigbee factory-new, start network steering");
                    status_led_set_zigbee_joined(false);
                    power_manager_prevent_light_sleep(true);
                    s_steering_fail_count = 0;
                    apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "factory-new fast");
                    restart_pairing_debug();
                    start_network_steering_now("steering_start");
                } else {
                    s_joined = true;
                    s_steering_active = false;
                    s_current_channel = esp_zb_get_current_channel();
                    status_led_set_failure(false);
                    status_led_set_zigbee_joined(true);
                    power_manager_prevent_light_sleep(false);
                    stop_pairing_debug();
                    log_network_info("Zigbee joined");
                    if (zigbee_channel_is_valid(s_current_channel)) {
                        storage_save_zigbee_last_channel(s_current_channel);
                        s_fast_pair_channel = s_current_channel;
                    }
                    debug_pairing_state("joined_reboot", ESP_OK);
                    configure_reporting();
                    publish_state_to_zigbee(&s_last_published, true);
                }
            } else {
                ESP_LOGE(TAG, "Zigbee init failed: %s", esp_err_to_name(status));
            }
            break;

        case ESP_ZB_BDB_SIGNAL_STEERING:
            if (status == ESP_OK) {
                s_joined = true;
                s_factory_new = false;
                s_steering_active = false;
                s_steering_fail_count = 0;
                s_current_channel = esp_zb_get_current_channel();
                status_led_set_failure(false);
                status_led_set_zigbee_joined(true);
                power_manager_prevent_light_sleep(false);
                stop_pairing_debug();
                log_network_info("Zigbee steering complete");
                if (zigbee_channel_is_valid(s_current_channel)) {
                    storage_save_zigbee_last_channel(s_current_channel);
                    s_fast_pair_channel = s_current_channel;
                }
                debug_pairing_state("steering_complete", status);
                configure_reporting();
                publish_state_to_zigbee(&s_last_published, true);
            } else {
                s_joined = false;
                s_steering_active = false;
                s_steering_fail_count++;
                s_current_channel = esp_zb_get_current_channel();
                if (s_steering_fail_count >= ZB_FAST_STEERING_ATTEMPTS &&
                    s_current_channel_mask == zigbee_channel_mask(s_fast_pair_channel)) {
                    apply_steering_channel_mask(zigbee_configured_scan_mask(), "fallback scan");
                }
                status_led_set_failure(s_current_channel_mask != zigbee_configured_scan_mask());
                status_led_set_zigbee_pairing(true);
                status_led_set_zigbee_joined(false);
                power_manager_prevent_light_sleep(true);
                ESP_LOGW(TAG, "Zigbee steering failed: %s, retry fail_count=%u mask=0x%08lx",
                         esp_err_to_name(status),
                         s_steering_fail_count,
                         (unsigned long)s_current_channel_mask);
                debug_pairing_state("steering_failed", status);
                restart_pairing_debug();
                esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_top_level_commissioning_cb,
                                       ESP_ZB_BDB_MODE_NETWORK_STEERING,
                                       1000);
            }
            break;

        default:
            ESP_LOGD(TAG, "Zigbee signal 0x%lx status=%s", (unsigned long)*signal, esp_err_to_name(status));
            break;
    }
}

static void zigbee_task(void *arg)
{
    (void)arg;

    esp_zb_cfg_t nwk_cfg = ESP_ZB_REMOTE_CONFIG();
    ESP_LOGI(TAG, "Zigbee init begin");
    esp_zb_init(&nwk_cfg);
    ESP_LOGI(TAG, "Zigbee init done");

    const device_state_t *state = state_get();
    if (state != NULL) {
        s_last_published = *state;
    }

#if ZB_PAIRING_MINIMAL_ENDPOINTS
    esp_zb_on_off_light_cfg_t light_cfg = ESP_ZB_DEFAULT_ON_OFF_LIGHT_CONFIG();
    light_cfg.basic_cfg.power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE;
    light_cfg.on_off_cfg.on_off = false;
    esp_zb_ep_list_t *ep_list = esp_zb_on_off_light_ep_create(ZB_REMOTE_ENDPOINT, &light_cfg);
    FATAL_ERROR_IF(ep_list == NULL, "esp_zb_on_off_light_ep_create diagnostic");
#else
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();
    FATAL_ERROR_IF(ep_list == NULL, "esp_zb_ep_list_create");
#endif
#if ZB_PAIRING_MINIMAL_ENDPOINTS
#elif ZB_PAIRING_SWITCH_ENDPOINTS_ONLY
    for (uint8_t ep = ZB_SWITCH_FIRST_ENDPOINT; ep <= ZB_SWITCH_LAST_ENDPOINT; ++ep) {
        add_switch_endpoint(ep_list, ep, ((s_last_published.switches >> (ep - 1U)) & 1U) != 0);
    }
#else
    add_remote_endpoint(ep_list, &s_last_published);
#endif

    ESP_LOGI(TAG, "Zigbee register endpoints begin");
    FATAL_ERROR_CHECK(esp_zb_device_register(ep_list));
    ESP_LOGI(TAG, "Zigbee register endpoints done");
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_rx_on_when_idle(true);
    ESP_LOGI(TAG, "rx_on_when_idle=%s", esp_zb_get_rx_on_when_idle() ? "true" : "false");
    load_fast_pair_channel();
    apply_steering_channel_mask(zigbee_channel_mask(s_fast_pair_channel), "initial fast");

    ESP_LOGI(TAG,
             "Espressif Zigbee SDK %s, end-device HA profile=0x%04x fast_channel=%u mask=0x%08lx fallback_mask=0x%08lx known_pan=0x%04x known_ext_pan=bd:8e:22:7f:6e:c2:6a:30 keep_alive_ms=%u",
             esp_zb_get_version_string(),
             ESP_ZB_AF_HA_PROFILE_ID,
             s_fast_pair_channel,
             (unsigned long)s_current_channel_mask,
             (unsigned long)zigbee_configured_scan_mask(),
             ZB_TARGET_PAN_ID,
             ZB_ED_KEEP_ALIVE_MS);
    debug_pairing_state("before_start", ESP_OK);
    FATAL_ERROR_CHECK(esp_zb_start(false));
    ESP_LOGI(TAG, "esp_zb_start returned OK");
    publish_state_to_zigbee(&s_last_published, false);
    esp_zb_stack_main_loop();
}

void zigbee_init(void)
{
    const esp_zb_platform_config_t config = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    FATAL_ERROR_CHECK(esp_zb_platform_config((esp_zb_platform_config_t *)&config));

    status_led_set_zigbee_joined(false);
    power_manager_prevent_light_sleep(true);

    if (s_pairing_debug_timer == NULL) {
        const esp_timer_create_args_t pairing_timer_args = {
            .callback = pairing_debug_timer_callback,
            .name = "zb_pair_dbg",
        };
        FATAL_ERROR_CHECK(esp_timer_create(&pairing_timer_args, &s_pairing_debug_timer));
    }

    BaseType_t created = xTaskCreate(zigbee_task, "zigbee_main", 8192, NULL, 5, NULL);
    if (created != pdPASS) {
        fatal_error_restart(TAG, "Nelze vytvořit zigbee_main task", ESP_ERR_NO_MEM);
    }
}

void zigbee_request_repair(void)
{
    if (!s_started) {
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
    power_manager_prevent_light_sleep(true);
    esp_zb_scheduler_alarm((esp_zb_callback_t)manual_repair_cb, 0, 0);
}

void zigbee_publish_state(const device_state_t *state)
{
    if (state == NULL) {
        return;
    }

    s_last_published = *state;
    publish_state_to_zigbee(state, true);
}

void zigbee_handle_rs232_enabled_from_ha(bool enabled)
{
    publish_event(INPUT_EVENT_ZIGBEE_RS232_SET, ZB_RS232_ENDPOINT, enabled ? 1 : 0);
}

bool zigbee_handle_start_ota_from_ha(void)
{
    publish_event(INPUT_EVENT_OTA_REQUEST, ZB_OTA_ENDPOINT, 1);
    return true;
}

void zigbee_publish_communication_error(bool communication_error, const void *diagnostics)
{
    (void)diagnostics;

    ESP_LOGW(TAG, "Zigbee communication_error=%s", communication_error ? "true" : "false");
}
