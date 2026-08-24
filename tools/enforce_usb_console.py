#!/usr/bin/env python3
"""Force the local ESP-IDF sdkconfig console to built-in USB Serial/JTAG.

sdkconfig.defaults only supplies defaults; an existing generated sdkconfig can
keep an older UART console selection. This script makes the project's intended
console choice explicit before reconfigure/build.
"""
from __future__ import annotations

from pathlib import Path

path = Path("sdkconfig")
if not path.exists():
    print("sdkconfig not present; sdkconfig.defaults will select USB Serial/JTAG")
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
    "CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y",
    "# CONFIG_ESP_CONSOLE_UART_DEFAULT is not set",
    "# CONFIG_ESP_CONSOLE_UART_CUSTOM is not set",
    "# CONFIG_ESP_CONSOLE_USB_CDC is not set",
    "# CONFIG_ESP_CONSOLE_NONE is not set",
    "CONFIG_ESP_CONSOLE_SECONDARY_NONE=y",
    "# CONFIG_ESP_CONSOLE_SECONDARY_USB_SERIAL_JTAG is not set",
])
path.write_text("\n".join(kept) + "\n", encoding="utf-8")
print("sdkconfig console forced to built-in USB Serial/JTAG")
