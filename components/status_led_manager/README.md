# status_led_manager

Portable ESP-IDF status LED manager for one WS2812-compatible LED using the `led_strip` RMT backend.

The component is intentionally independent of Zigbee, OTA, Home Assistant and project state. Only this component is allowed to call `led_strip_set_pixel()` / `led_strip_refresh()`.

## Design

- Two internal `esp_timer` timers are used.
  - heartbeat timer: requests the periodic OK blink (default 10 s),
  - scheduler timer: services priority states and the short-pulse FIFO (default 50 ms).
- Short indications are stored as a color ID plus duration. The FIFO has 16 entries.
- When the FIFO is full, the oldest queued indication is discarded.
- Persistent states use eight priority slots. The highest active priority wins.
- An optional external-state callback can add a project-specific high-priority state without adding another timer. The current project uses it for OTA download state.
- `status_led_manager_block()` / `status_led_manager_unblock()` prevent all WS2812/RMT transmissions during RF-critical sections. Calls may be nested. Timers and the queue continue to advance while blocked.
- Hardware output is serialized by one mutex and a color is transmitted only when the requested RGB value changes.

## Built-in color IDs

`OFF`, `GREEN`, `YELLOW`, `PURPLE`, `BLUE`, `CYAN`, `ORANGE`, `WHITE`, `RED`, `MAGENTA`.

The RGB table is intentionally inside the component, so project code queues a one-byte color ID instead of raw RGB values.

## Minimal integration

```c
#include "status_led_manager.h"

status_led_manager_config_t cfg = {
    .gpio_num = 8,
    .heartbeat_period_ms = 10000,
    .scheduler_period_ms = 50,
    .heartbeat_pulse_ms = 150,
    .heartbeat_color = STATUS_LED_COLOR_GREEN,
};
ESP_ERROR_CHECK(status_led_manager_init(&cfg));

status_led_manager_enqueue(STATUS_LED_COLOR_YELLOW, 200);
status_led_manager_set_mode(0, true, STATUS_LED_COLOR_RED, 200, 100);
```

## Protecting RF-critical code

```c
status_led_manager_block();
/* Zigbee/RF operation that must not be disturbed by an RMT transfer. */
status_led_manager_unblock();
```

`block()` waits until an already-running LED refresh has completed. Once it returns, the scheduler cannot start another LED transmission until the matching `unblock()`.

## Priorities

Priority is an unsigned byte; larger number wins. Suggested project policy:

- 100 fatal
- 90 OTA firmware transfer/verify
- 70 pairing or hard failure
- 20 queued user/provisioning pulses
- heartbeat is lowest priority

Queued pulses do not need an explicit priority: all active persistent states suppress them until the state clears.
