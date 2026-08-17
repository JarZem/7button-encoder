#include "fatal_error.h"

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status_led.h"

void fatal_error_restart(const char *tag, const char *message, esp_err_t err)
{
    ESP_LOGE(tag != NULL ? tag : "fatal", "Fatal: %s failed: %s",
             message != NULL ? message : "operation",
             esp_err_to_name(err));

    status_led_fatal();

    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        vTaskDelay(pdMS_TO_TICKS(500));
    } else {
        esp_rom_delay_us(500000);
    }

    esp_restart();
}
