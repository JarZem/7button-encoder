#pragma once

#include <stdbool.h>

void status_led_init(void);
void status_led_indicate_boot(void);
void status_led_set_zigbee_joined(bool joined);
void status_led_set_zigbee_pairing(bool pairing);
void status_led_set_failure(bool failed);
void status_led_indicate_local_activity(void);
void status_led_indicate_ha_command(void);
void status_led_indicate_ha_publish(void);
void status_led_indicate_provision_step(void);
void status_led_fatal(void);
