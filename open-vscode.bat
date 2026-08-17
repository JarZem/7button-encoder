@echo off
setlocal

set "PROJECT_DIR=%~dp0"
set "IDF_EXPORT=D:\Espressif\esp-idf\export.bat"

cd /d "%PROJECT_DIR%"

if not exist "%IDF_EXPORT%" (
    echo ESP-IDF export.bat nebyl nalezen: %IDF_EXPORT%
    echo Uprav cestu IDF_EXPORT v tomto souboru.
    pause
    exit /b 1
)

call "%IDF_EXPORT%"
if errorlevel 1 (
    echo Nepodarilo se aktivovat ESP-IDF prostredi.
    pause
    exit /b 1
)

if not exist "sdkconfig" (
    idf.py set-target esp32c6
    if errorlevel 1 (
        echo Nepodarilo se nastavit ESP-IDF target.
        pause
        exit /b 1
    )
)

idf.py reconfigure
if errorlevel 1 (
    echo idf.py reconfigure selhalo. VS Code spustim i tak, ale IntelliSense nemusi byt aktualni.
    pause
)

where code >nul 2>nul
if errorlevel 1 (
    echo Prikaz code neni v PATH. Otevri projekt ručně z teto slozky:
    echo %PROJECT_DIR%
    pause
    exit /b 1
)

code "%PROJECT_DIR%"
