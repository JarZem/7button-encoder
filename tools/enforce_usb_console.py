#!/usr/bin/env python3
"""Keep the project debug console on UART and mirror it to built-in USB Serial/JTAG.

This matches sdkconfig.defaults. It deliberately does not switch USB Serial/JTAG
into the only primary console, because the project uses both development paths.
"""
from __future__ import annotations

from pathlib import Path

path = Path("sdkconfig")
if not path.exists():
    print("sdkconfig not present; sdkconfig.defaults will select UART + USB Serial/JTAG mirror")
    raise SystemExit(0)

prefixes = (
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=",
    "CONFIG_ESP_CONSOLE_UART_DEFAULT=",
    "CONFIG_ESP_CONSOLE_UART_CUSTOM=",
    "CONFIG_ESP_CONSOLE_USB_CDC=",
    "CONFIG_ESP_CONSOLE_NONE=",
    "CONFIG_ESP_CONSOLE_SECONDARY_NONE=",
    "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=",
    "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set",
    "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set",
    "# CONFIG_ESP_CONSOLE_UART_CUSTOM is not set",
    "# CONFIG_ESP_CONSOLE_USB_CDC is not set",
    "# CONFIG_ESP_CONSOLE_NONE is not set",
    "# CONFIG_ESP_CONSOLE_SECONDARY_NONE is not set",
    "# CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is not set",
)

lines = path.read_text(encoding="utf-8").splitlines()
kept = [line for line in lines if not any(line.startswith(prefix) for prefix in prefixes)]
kept.extend([
    "CONFIG_ESP_CONSOLE_UART_DEFAULT=y",
    "# CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG is not set",
    "# CONFIG_ESP_CONSOLE_UART_CUSTOM is not set",
    "# CONFIG_ESP_CONSOLE_USB_CDC is not set",
    "# CONFIG_ESP_CONSOLE_NONE is not set",
    "# CONFIG_ESP_CONSOLE_SECONDARY_NONE is not set",
    "CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG=y",
])
path.write_text("\n".join(kept) + "\n", encoding="utf-8")
print("sdkconfig console restored: UART primary + USB Serial/JTAG secondary mirror")
