#include "power_manager.h"

#include "esp_log.h"
#include "fatal_error.h"
#include "sdkconfig.h"

#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

static const char *TAG = "power";

#if CONFIG_PM_ENABLE
static esp_pm_lock_handle_t s_no_light_sleep_lock;
static bool s_no_light_sleep_locked;
#endif

void power_manager_init(void)
{
#if CONFIG_PM_ENABLE
    const esp_pm_config_t pm_config = {
        .max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ,
        .min_freq_mhz = 40,
        .light_sleep_enable = false,
    };

    FATAL_ERROR_CHECK(esp_pm_configure(&pm_config));
    FATAL_ERROR_CHECK(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP,
                                         0,
                                         "zigbee_pairing",
                                         &s_no_light_sleep_lock));
    power_manager_prevent_light_sleep(true);
    ESP_LOGI(TAG, "Power management: DFS %d-%d MHz, light sleep %s",
             pm_config.min_freq_mhz,
             pm_config.max_freq_mhz,
             pm_config.light_sleep_enable ? "zapnutý" : "vypnutý");
#else
    ESP_LOGW(TAG, "Power management není povolený v sdkconfig");
#endif
}

void power_manager_prevent_light_sleep(bool prevent)
{
#if CONFIG_PM_ENABLE
    if (s_no_light_sleep_lock == NULL || s_no_light_sleep_locked == prevent) {
        return;
    }

    esp_err_t err = prevent
                        ? esp_pm_lock_acquire(s_no_light_sleep_lock)
                        : esp_pm_lock_release(s_no_light_sleep_lock);
    if (err == ESP_OK) {
        s_no_light_sleep_locked = prevent;
        ESP_LOGI(TAG, "Light sleep %s", prevent ? "dočasně blokován" : "povolen");
    } else {
        ESP_LOGW(TAG, "Nelze změnit light sleep lock: %s", esp_err_to_name(err));
    }
#else
    (void)prevent;
#endif
}
