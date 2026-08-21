#include "jarzem_secure_ota.h"
#include "status_led.h"

void jarzem_ota_hook_rx_from_ha(void)
{
    status_led_indicate_ha_command();
}

void jarzem_ota_hook_provision_step(void)
{
    status_led_indicate_provision_step();
}
