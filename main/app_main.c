#include "sdkconfig.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static int usb_log_vprintf(const char *format, va_list args)
{
    char buffer[768];
    int len = vsnprintf(buffer, sizeof(buffer), format, args);
    if (len <= 0) {
        return len;
    }

    size_t write_len = (size_t)len;
    if (write_len >= sizeof(buffer)) {
        write_len = sizeof(buffer) - 1;
    }

    (void)usb_serial_jtag_write_bytes(buffer, write_len, pdMS_TO_TICKS(100));
    return len;
}

static void init_builtin_usb_console(void)
{
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t config = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        config.tx_buffer_size = 2048;
        config.rx_buffer_size = 256;
        ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&config));
    }

    usb_serial_jtag_vfs_use_driver();
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    esp_log_set_vprintf(usb_log_vprintf);

    static const char marker[] = "USB_SERIAL_JTAG_CONSOLE_READY\r\n";
    (void)usb_serial_jtag_write_bytes(marker, sizeof(marker) - 1, pdMS_TO_TICKS(100));
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(250));
}

#if CONFIG_APP_ZIGBEE_MINIMAL_TEST

#include "event_bus.h"
#include "fw_identity.h"
#include "input.h"
#include "minimal_app_state.h"
#include "status_led.h"
#include "storage.h"
#include "zigbee_minimal.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "remote_control";

static void log_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "Firmware: %s product=%s hw=%s channel=%s secure_version=%d",
             app->version, FW_PRODUCT, HW_REVISION, FW_CHANNEL, FW_SECURE_VERSION);
    ESP_LOGI(TAG, "Running partition: %s, offset 0x%lx, size %lu B",
             running->label,
             (unsigned long)running->address,
             (unsigned long)running->size);
}

static const char *event_name(input_event_type_t type)
{
    switch (type) {
        case INPUT_EVENT_BUTTON_PRESSED: return "BUTTON_PRESSED";
        case INPUT_EVENT_BUTTON_RELEASED: return "BUTTON_RELEASED";
        case INPUT_EVENT_ENCODER_CW: return "ENCODER_CW";
        case INPUT_EVENT_ENCODER_CCW: return "ENCODER_CCW";
        case INPUT_EVENT_ZIGBEE_REPAIR_REQUEST: return "ZB_REPAIR_REQUEST";
        case INPUT_EVENT_ZIGBEE_SWITCH_SET: return "ZB_SWITCH_SET";
        case INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET: return "ZB_LIGHT_ONOFF_SET";
        case INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET: return "ZB_BRIGHTNESS_SET";
        case INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET: return "ZB_WHITE_TEMP_SET";
        case INPUT_EVENT_ZIGBEE_RS232_SET: return "ZB_RS232_SET";
        default: return "UNKNOWN";
    }
}

static void app_event_task(void *arg)
{
    (void)arg;
    input_event_t event;

    while (true) {
        if (event_bus_receive(&event, portMAX_DELAY)) {
            ESP_LOGI(TAG, "%s id=%u value=%ld tick=%lu",
                     event_name(event.type),
                     event.input_id,
                     (long)event.value,
                     (unsigned long)event.tick);
            if (event.type == INPUT_EVENT_ZIGBEE_REPAIR_REQUEST) {
                zigbee_minimal_request_repair();
                continue;
            }
            minimal_app_state_handle_input_event(&event);
        }
    }
}

void app_main(void)
{
    init_builtin_usb_console();

    status_led_init();
    status_led_indicate_boot();

    ESP_LOGW(TAG, "STABLE ZIGBEE BUILD: inputs and delayed NVS enabled, RS232 transport disabled");

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    log_partition_info();
    event_bus_init();
    storage_init();
    minimal_app_state_init();
    input_init();
    BaseType_t created = xTaskCreate(app_event_task, "app_event", 4096, NULL, 8, NULL);
    ESP_ERROR_CHECK(created == pdPASS ? ESP_OK : ESP_FAIL);
    zigbee_minimal_init();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

#else

#include "event_bus.h"
#include "fw_identity.h"
#include "input.h"
#include "state.h"
#include "storage.h"
#include "uart_transport.h"
#include "zigbee.h"
#include "debug_console.h"
#include "power_manager.h"
#include "status_led.h"
#include "fatal_error.h"

#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "remote_control";

static const char *event_name(input_event_type_t type)
{
    switch (type) {
        case INPUT_EVENT_BUTTON_PRESSED:  return "BUTTON_PRESSED";
        case INPUT_EVENT_BUTTON_RELEASED: return "BUTTON_RELEASED";
        case INPUT_EVENT_ENCODER_CW:       return "ENCODER_CW";
        case INPUT_EVENT_ENCODER_CCW:      return "ENCODER_CCW";
        case INPUT_EVENT_ZIGBEE_REPAIR_REQUEST: return "ZB_REPAIR_REQUEST";
        case INPUT_EVENT_ZIGBEE_SWITCH_SET: return "ZB_SWITCH_SET";
        case INPUT_EVENT_ZIGBEE_LIGHT_ONOFF_SET: return "ZB_LIGHT_ONOFF_SET";
        case INPUT_EVENT_ZIGBEE_BRIGHTNESS_SET: return "ZB_BRIGHTNESS_SET";
        case INPUT_EVENT_ZIGBEE_WHITE_TEMP_SET: return "ZB_WHITE_TEMP_SET";
        case INPUT_EVENT_ZIGBEE_RS232_SET: return "ZB_RS232_SET";
        default:                           return "UNKNOWN";
    }
}

static void log_partition_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_app_desc_t *app = esp_app_get_description();

    ESP_LOGI(TAG, "Firmware: %s product=%s hw=%s channel=%s secure_version=%d",
             app->version, FW_PRODUCT, HW_REVISION, FW_CHANNEL, FW_SECURE_VERSION);
    ESP_LOGI(TAG, "Běžící partition: %s, offset 0x%lx, velikost %lu B",
             running->label,
             (unsigned long)running->address,
             (unsigned long)running->size);
}

void app_main(void)
{
    init_builtin_usb_console();

    status_led_init();
    status_led_indicate_boot();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        FATAL_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    FATAL_ERROR_CHECK(err);

    log_partition_info();
    power_manager_init();
    event_bus_init();
    storage_init();
    debug_console_init();
    uart_transport_init();
    state_init();
    zigbee_init();
    input_init();

    ESP_LOGI(TAG, "Remote control spuštěn");

    input_event_t event;
    while (true) {
        if (event_bus_receive(&event, portMAX_DELAY)) {
            ESP_LOGI(TAG, "%s id=%u value=%ld tick=%lu",
                     event_name(event.type),
                     event.input_id,
                     (long)event.value,
                     (unsigned long)event.tick);

            if (event.type == INPUT_EVENT_ZIGBEE_REPAIR_REQUEST) {
                zigbee_request_repair();
            } else {
                state_handle_input_event(&event);
            }
        }
    }
}

#endif
