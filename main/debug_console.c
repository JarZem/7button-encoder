#include "debug_console.h"

#include <stdbool.h>
#include <stdio.h>
#include "driver/usb_serial_jtag.h"

static bool debug_console_is_active(void)
{
    return usb_serial_jtag_is_connected();
}

void debug_console_init(void)
{
    if (debug_console_is_active()) {
        printf("DEBUG usb_serial_jtag=connected\n");
    }
}

void debug_console_publish_state(const char *reason, const device_state_t *state)
{
    if (state == NULL || !debug_console_is_active()) {
        return;
    }

    printf("STATE reason=%s switches=0x%02x switch1=%u switch2=%u switch3=%u "
           "switch4=%u switch5=%u switch6=%u brightness=%u white_temperature=%u "
           "rs232=%s\n",
           reason != NULL ? reason : "change",
           state->switches & DEVICE_SWITCH_MASK,
           (state->switches >> 0) & 1U,
           (state->switches >> 1) & 1U,
           (state->switches >> 2) & 1U,
           (state->switches >> 3) & 1U,
           (state->switches >> 4) & 1U,
           (state->switches >> 5) & 1U,
           state->brightness,
           state->white_temperature,
           state->rs232_enabled ? "on" : "off");
}

void debug_console_publish_ha_change(const device_state_t *state)
{
    if (state == NULL || !debug_console_is_active()) {
        return;
    }

    printf("HA_CHANGE switches=0x%02x brightness=%u white_temperature=%u rs232=%s\n",
           state->switches & DEVICE_SWITCH_MASK,
           state->brightness,
           state->white_temperature,
           state->rs232_enabled ? "on" : "off");
}

void debug_console_publish_ota_window(bool active, uint32_t timeout_seconds)
{
    if (!debug_console_is_active()) {
        return;
    }

    printf("OTA_WINDOW active=%s timeout_seconds=%lu\n",
           active ? "true" : "false",
           (unsigned long)timeout_seconds);
}

void debug_console_publish_zigbee_pairing(const char *event,
                                          bool joined,
                                          bool factory_new,
                                          bool steering,
                                          uint32_t channel_mask,
                                          uint8_t current_channel,
                                          const char *status)
{
    if (!debug_console_is_active()) {
        return;
    }

    printf("ZIGBEE_PAIRING event=%s joined=%s factory_new=%s steering=%s "
           "channel_mask=0x%08lx current_channel=%u status=%s\n",
           event != NULL ? event : "state",
           joined ? "true" : "false",
           factory_new ? "true" : "false",
           steering ? "true" : "false",
           (unsigned long)channel_mask,
           current_channel,
           status != NULL ? status : "unknown");
}
