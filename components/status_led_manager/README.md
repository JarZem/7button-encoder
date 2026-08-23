# status_led_manager

Portable ESP-IDF status LED manager for one WS2812-compatible LED using the `led_strip` RMT backend.

The component is intentionally independent of Zigbee, OTA, Home Assistant and project state. Only this component is allowed to call `led_strip_set_pixel()` / `led_strip_refresh()`.

## Design

- Two internal `esp_timer` timers are used.
  - heartbeat timer: requests the periodic OK blink (default 10 s),
  - scheduler timer: services priority states and the short-pulse FIFO (default 50 ms).
- Short indications are stored as a color ID plus duration. The FIFO has 16 entries.
- When the FIFO is full, the oldest queued indication is discarded.
- One scheduler-tick OFF gap is inserted between queued pulses, so adjacent identical colors remain visible as separate blinks.
- Persistent states use eight priority slots. The highest active priority wins.
- Persistent states are START/STOP operations, not repeatedly queued pulses: one `set_mode(..., true, ...)` starts blinking and one `set_mode(..., false, ...)` stops it.
- An optional external-state callback can expose a long-running project state without repeatedly writing to the FIFO. The current project uses this for OTA Wi-Fi connect/download/verify phases.
- `status_led_manager_block()` / `status_led_manager_unblock()` prevent all WS2812/RMT transmissions during RF-critical sections. Calls may be nested. Timers and the queue continue to advance while blocked.
- Hardware output is serialized by one mutex and a color is transmitted only when the requested RGB value changes.

## Color IDs

Color IDs are one byte in the API; IDs 0..63 are reserved for the color table (`STATUS_LED_COLOR_TABLE_SIZE = 64`). Currently defined colors are:

`OFF`, `GREEN`, `YELLOW`, `PURPLE`, `BLUE`, `CYAN`, `ORANGE`, `WHITE`, `RED`, `MAGENTA`.

The remaining IDs are reserved for future project-independent colors. Control information is deliberately not packed into the high bits of the color ID. Long-running START/STOP behavior is represented explicitly by `status_led_manager_set_mode()`, which is easier to validate and cannot be mistaken for a color.

## Communication convention

The manager itself does not know Zigbee or OTA semantics. Projects should hook activity at the common transport boundary:

- successful application RX into ESP -> short `YELLOW` pulse,
- successful application TX from ESP -> short `CYAN` pulse.

This automatically covers normal Home Assistant commands, OTA endpoint 10/11 traffic, provisioning frames, CHECK frames and their application responses without adding LED calls to protocol logic.

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

/* Short event. */
status_led_manager_enqueue(STATUS_LED_COLOR_YELLOW, 200);

/* Long-running state: START once, STOP once. */
status_led_manager_set_mode(0, true, STATUS_LED_COLOR_RED, 200, 100);
status_led_manager_set_mode(0, false, STATUS_LED_COLOR_RED, 200, 100);
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
- 20 queued RX/TX/event pulses
- heartbeat is lowest priority

Queued pulses do not need an explicit priority: all active persistent states suppress them until the state clears.
