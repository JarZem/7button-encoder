#include "zigbee_ota_control.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_zigbee_attribute.h"
#include "esp_zigbee_cluster.h"
#include "esp_zigbee_core.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "zigbee_ota_cluster.h"
#include "ota_secure_session.h"
#include "ota_service.h"
#include "zcl/esp_zigbee_zcl_command.h"

static const char *TAG = "zigbee_ota_control";

#define ZB_OTA_CONTROL_DEVICE_ID 0xff02
#define OTA_STATUS_MONITOR_MS 250

static bool s_enable_ota = false;
static uint8_t s_status = ZIGBEE_OTA_STATUS_IDLE;
static bool s_endpoint_registered;
static bool s_monitor_started;

static bool network_ready(void)
{
    if (esp_zb_bdb_is_factory_new()) return false;
    const uint16_t short_addr = esp_zb_get_short_address();
    return short_addr != 0x0000 && short_addr != 0xfffe && short_addr != 0xffff;
}

static void report_attr(uint16_t cluster_id, uint16_t attr_id)
{
    if (!s_endpoint_registered || !network_ready()) return;

    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd = {
            .dst_addr_u.addr_short = 0x0000,
            .dst_endpoint = 1,
            .src_endpoint = ZIGBEE_OTA_CONTROL_ENDPOINT,
        },
        .address_mode = ESP_ZB_APS_ADDR_MODE_16_ENDP_PRESENT,
        .clusterID = cluster_id,
        .manuf_specific = 1,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
        .dis_default_resp = 1,
        .manuf_code = ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE,
        .attributeID = attr_id,
    };

    esp_zb_lock_acquire(portMAX_DELAY);
    esp_err_t err = esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "report failed cluster=0x%04x attr=0x%04x manuf=0x%04x: %s",
                 cluster_id, attr_id, ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE, esp_err_to_name(err));
    }
}

static void set_manufacturer_attr_locked(uint16_t cluster_id, uint16_t attr_id, void *value)
{
    if (!s_endpoint_registered) return;
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_status_t st = esp_zb_zcl_set_manufacturer_attribute_val(
        ZIGBEE_OTA_CONTROL_ENDPOINT,
        cluster_id,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE,
        attr_id,
        value,
        false);
    esp_zb_lock_release();
    if (st != ESP_ZB_ZCL_STATUS_SUCCESS) {
        ESP_LOGW(TAG, "set manufacturer attr failed cluster=0x%04x attr=0x%04x manuf=0x%04x status=0x%x",
                 cluster_id, attr_id, ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE, st);
    }
}

void zigbee_ota_control_set_status(zigbee_ota_status_t status)
{
    if (s_status == (uint8_t)status) return;
    s_status = (uint8_t)status;
    set_manufacturer_attr_locked(ZIGBEE_OTA_STATUS_CLUSTER_ID, ZIGBEE_OTA_STATUS_ATTR_ID, &s_status);
    ESP_LOGI(TAG, "OTA Status=%u", (unsigned)s_status);
    report_attr(ZIGBEE_OTA_STATUS_CLUSTER_ID, ZIGBEE_OTA_STATUS_ATTR_ID);
}

static void ota_status_monitor_task(void *arg)
{
    (void)arg;
    ota_state_t previous = OTA_STATE_IDLE;
    while (true) {
        const ota_state_t state = ota_service_get_state();
        if (state != previous) {
            switch (state) {
                case OTA_STATE_CONNECTING_WIFI:
                case OTA_STATE_DOWNLOADING:
                    if (zigbee_ota_control_is_enabled() &&
                        s_status != ZIGBEE_OTA_STATUS_FW_UPDATE_STARTED) {
                        zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_FW_UPDATE_STARTED);
                    }
                    break;
                case OTA_STATE_SUCCESS:
                    if (zigbee_ota_control_is_enabled()) {
                        zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_FW_UPDATE_COMPLETE);
                    }
                    break;
                case OTA_STATE_FAILED:
                    if (zigbee_ota_control_is_enabled()) {
                        const esp_err_t err = ota_service_get_last_error();
                        if (err == ESP_ERR_INVALID_VERSION) {
                            zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_FW_SKIPPED);
                        } else if (err == ESP_ERR_INVALID_CRC) {
                            zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_FW_VERIFY_ERROR);
                        } else {
                            zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_FW_UPDATE_ERROR);
                        }
                    }
                    break;
                default:
                    break;
            }
            previous = state;
        }
        vTaskDelay(pdMS_TO_TICKS(OTA_STATUS_MONITOR_MS));
    }
}

static void apply_enable_task(void *arg)
{
    const bool enabled = (uintptr_t)arg != 0;
    vTaskDelay(pdMS_TO_TICKS(10));
    if (enabled != s_enable_ota) {
        vTaskDelete(NULL);
        return;
    }
    if (enabled) {
        zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_PROVISIONING_STARTED);
        zigbee_ota_schedule_hello(0);
    } else {
        ota_secure_session_reset_for_retry();
        zigbee_ota_control_set_status(ZIGBEE_OTA_STATUS_IDLE);
    }
    vTaskDelete(NULL);
}

esp_err_t zigbee_ota_control_add_endpoint(esp_zb_ep_list_t *ep_list)
{
    ESP_RETURN_ON_FALSE(ep_list != NULL, ESP_ERR_INVALID_ARG, TAG, "ep_list is NULL");

    s_enable_ota = false;
    s_status = ZIGBEE_OTA_STATUS_IDLE;

    esp_zb_cluster_list_t *cluster_list = esp_zb_zcl_cluster_list_create();
    ESP_RETURN_ON_FALSE(cluster_list != NULL, ESP_ERR_NO_MEM, TAG, "cluster list allocation failed");

    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DC_SOURCE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    ESP_RETURN_ON_FALSE(basic != NULL, ESP_ERR_NO_MEM, TAG, "Basic cluster allocation failed");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_basic_cluster(cluster_list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
                        TAG, "Basic cluster add failed");

    esp_zb_attribute_list_t *enable_cluster = esp_zb_zcl_attr_list_create(ZIGBEE_OTA_ENABLE_CLUSTER_ID);
    ESP_RETURN_ON_FALSE(enable_cluster != NULL, ESP_ERR_NO_MEM, TAG, "Enable OTA cluster allocation failed");
    const uint8_t enable_access = ESP_ZB_ZCL_ATTR_ACCESS_READ_WRITE |
                                  ESP_ZB_ZCL_ATTR_ACCESS_REPORTING |
                                  ESP_ZB_ZCL_ATTR_MANUF_SPEC;
    ESP_RETURN_ON_ERROR(esp_zb_cluster_add_manufacturer_attr(
                            enable_cluster,
                            ZIGBEE_OTA_ENABLE_CLUSTER_ID,
                            ZIGBEE_OTA_ENABLE_ATTR_ID,
                            ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE,
                            ESP_ZB_ZCL_ATTR_TYPE_BOOL,
                            enable_access,
                            &s_enable_ota),
                        TAG, "Enable OTA attr add failed");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_custom_cluster(cluster_list, enable_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
                        TAG, "Enable OTA cluster add failed");

    esp_zb_attribute_list_t *status_cluster = esp_zb_zcl_attr_list_create(ZIGBEE_OTA_STATUS_CLUSTER_ID);
    ESP_RETURN_ON_FALSE(status_cluster != NULL, ESP_ERR_NO_MEM, TAG, "OTA Status cluster allocation failed");
    const uint8_t status_access = ESP_ZB_ZCL_ATTR_ACCESS_READ_ONLY |
                                  ESP_ZB_ZCL_ATTR_ACCESS_REPORTING |
                                  ESP_ZB_ZCL_ATTR_MANUF_SPEC;
    ESP_RETURN_ON_ERROR(esp_zb_cluster_add_manufacturer_attr(
                            status_cluster,
                            ZIGBEE_OTA_STATUS_CLUSTER_ID,
                            ZIGBEE_OTA_STATUS_ATTR_ID,
                            ZIGBEE_OTA_CONTROL_MANUFACTURER_CODE,
                            ESP_ZB_ZCL_ATTR_TYPE_U8,
                            status_access,
                            &s_status),
                        TAG, "OTA Status attr add failed");
    ESP_RETURN_ON_ERROR(esp_zb_cluster_list_add_custom_cluster(cluster_list, status_cluster, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE),
                        TAG, "OTA Status cluster add failed");

    const esp_zb_endpoint_config_t cfg = {
        .endpoint = ZIGBEE_OTA_CONTROL_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ZB_OTA_CONTROL_DEVICE_ID,
        .app_device_version = 0,
    };
    esp_err_t err = esp_zb_ep_list_add_ep(ep_list, cluster_list, cfg);
    if (err == ESP_OK) {
        s_endpoint_registered = true;
        ESP_LOGI(TAG, "endpoint=%u EnableOTA cluster=0x%04x bool default=0; Status cluster=0x%04x u8 default=0",
                 ZIGBEE_OTA_CONTROL_ENDPOINT, ZIGBEE_OTA_ENABLE_CLUSTER_ID, ZIGBEE_OTA_STATUS_CLUSTER_ID);
        if (!s_monitor_started) {
            s_monitor_started = xTaskCreate(ota_status_monitor_task, "ota_status", 3072, NULL, 4, NULL) == pdPASS;
            if (!s_monitor_started) ESP_LOGW(TAG, "OTA status monitor task creation failed");
        }
    }
    return err;
}

bool zigbee_ota_control_handle_set_attr(const esp_zb_zcl_set_attr_value_message_t *message)
{
    if (message == NULL || message->info.status != ESP_ZB_ZCL_STATUS_SUCCESS ||
        message->info.dst_endpoint != ZIGBEE_OTA_CONTROL_ENDPOINT ||
        message->info.cluster != ZIGBEE_OTA_ENABLE_CLUSTER_ID ||
        message->attribute.id != ZIGBEE_OTA_ENABLE_ATTR_ID) {
        return false;
    }

    if (message->attribute.data.type != ESP_ZB_ZCL_ATTR_TYPE_BOOL || message->attribute.data.value == NULL) {
        ESP_LOGW(TAG, "Enable OTA rejected: invalid type/value");
        return true;
    }

    /* The ZCL stack has already applied the manufacturer-specific write before this callback.
     * The registered attribute backing storage is s_enable_ota, so do not write it a second
     * time through the non-manufacturer API. */
    const bool enabled = *(bool *)message->attribute.data.value;
    s_enable_ota = enabled;

    ESP_LOGI(TAG, "Enable OTA=%u", s_enable_ota ? 1U : 0U);
    if (xTaskCreate(apply_enable_task, "ota_enable", 3072, (void *)(uintptr_t)(enabled ? 1U : 0U), 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "Enable OTA apply task creation failed");
    }
    return true;
}

bool zigbee_ota_control_is_enabled(void)
{
    return s_enable_ota;
}

uint8_t zigbee_ota_control_get_status(void)
{
    return s_status;
}
