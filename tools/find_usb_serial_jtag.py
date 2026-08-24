#!/usr/bin/env python3
"""Print the COM port of the ESP32-C6 built-in USB Serial/JTAG controller.

Espressif USB Serial/JTAG uses USB VID 0x303A, PID 0x1001.
The script prints only the selected port on stdout so batch files can capture it.
Diagnostics go to stderr.
"""
from __future__ import annotations

import sys

try:
    from serial.tools import list_ports
except ImportError as exc:
    print("pyserial is not available in the active ESP-IDF Python environment", file=sys.stderr)
    raise SystemExit(2) from exc

ESPRESSIF_VID = 0x303A
USB_SERIAL_JTAG_PID = 0x1001

matches = [
    port for port in list_ports.comports()
    if port.vid == ESPRESSIF_VID and port.pid == USB_SERIAL_JTAG_PID
]

if not matches:
    print(
        "ESP32-C6 built-in USB Serial/JTAG port not found "
        "(expected VID=303A PID=1001).",
        file=sys.stderr,
    )
    print("Detected serial ports:", file=sys.stderr)
    for port in list_ports.comports():
        vid = f"{port.vid:04X}" if port.vid is not None else "----"
        pid = f"{port.pid:04X}" if port.pid is not None else "----"
        print(f"  {port.device}: VID={vid} PID={pid} {port.description}", file=sys.stderr)
    raise SystemExit(1)

if len(matches) > 1:
    print("More than one Espressif USB Serial/JTAG device found:", file=sys.stderr)
    for port in matches:
        print(f"  {port.device}: {port.description} serial={port.serial_number or '-'}", file=sys.stderr)
    print("Connect only the ESP32-C6 being flashed, or specify the port manually.", file=sys.stderr)
    raise SystemExit(3)

print(matches[0].device)
