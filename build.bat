@echo off
setlocal

git pull
if errorlevel 1 exit /b %errorlevel%

python tools\enforce_usb_console.py
if errorlevel 1 exit /b %errorlevel%

idf.py reconfigure
if errorlevel 1 exit /b %errorlevel%

for /f "usebackq delims=" %%P in (`python tools\find_usb_serial_jtag.py`) do set "ESP_PORT=%%P"
if not defined ESP_PORT (
    echo ESP32-C6 built-in USB Serial/JTAG COM port was not found.
    exit /b 1
)

echo Using ESP32-C6 built-in USB Serial/JTAG on %ESP_PORT%

idf.py build
if errorlevel 1 exit /b %errorlevel%

idf.py -p %ESP_PORT% flash monitor
