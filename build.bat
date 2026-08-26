@echo off
setlocal EnableExtensions EnableDelayedExpansion
cd /d "%~dp0"
set "PROJECT_DIR=%CD%"
set "MODE=build"

if /I "%~1"=="-f" set "MODE=flash"
if /I "%~1"=="-m" set "MODE=monitor"
if not "%~1"=="" if /I not "%~1"=="-f" if /I not "%~1"=="-m" goto :usage
if not "%~2"=="" goto :usage

call :ensure_idf || goto :error

if /I "%MODE%"=="monitor" goto :monitor_only

echo [GIT] Kontroluji vzdaleny repozitar...
git fetch --recurse-submodules=no
if errorlevel 1 goto :error

for /f "delims=" %%U in ('git rev-parse --abbrev-ref --symbolic-full-name "@{u}" 2^>nul') do set "UPSTREAM=%%U"
if not defined UPSTREAM (
    echo CHYBA: aktualni Git branch nema nastaveny upstream.
    goto :error
)

set "REMOTE_COUNT=0"
for /f "delims=" %%C in ('git rev-list --count HEAD..@{u}') do set "REMOTE_COUNT=%%C"
if "%REMOTE_COUNT%"=="0" (
    echo [GIT] Neni nic noveho na %UPSTREAM%. Koncim bez buildu.
    exit /b 0
)

echo [GIT] Nalezeno %REMOTE_COUNT% novych commitu. Stahuji zmeny...
git pull --ff-only
if errorlevel 1 goto :error

echo [GIT] Synchronizuji submoduly...
git submodule sync --recursive
if errorlevel 1 goto :error
git submodule update --init --recursive
if errorlevel 1 goto :error

echo [IDF] Nastavuji USB konzoli...
python tools\enforce_usb_console.py
if errorlevel 1 goto :error

if not exist "sdkconfig" (
    echo [IDF] Nastavuji target esp32c6...
    call idf.py set-target esp32c6
    if errorlevel 1 goto :error
)

echo [IDF] Reconfigure...
call idf.py reconfigure
if errorlevel 1 goto :error

echo [BUILD] Sestavuji firmware...
call idf.py build
if errorlevel 1 goto :error

if /I "%MODE%"=="flash" goto :flash

echo.
echo HOTOVO - Git update a build probehly OK.
exit /b 0

:flash
call :find_port || goto :error
echo [FLASH] Flashuji na %ESP_PORT% a spoustim monitoring...
call idf.py -p %ESP_PORT% flash monitor
if errorlevel 1 goto :error
exit /b 0

:monitor_only
call :find_port || goto :error
echo [MONITOR] Spoustim pouze monitoring na %ESP_PORT%...
call idf.py -p %ESP_PORT% monitor
if errorlevel 1 goto :error
exit /b 0

:ensure_idf
where idf.py >nul 2>nul
if not errorlevel 1 (
    echo [IDF] idf.py je dostupny.
    exit /b 0
)

echo [IDF] idf.py neni v aktualnim PATH. Hledam ESP-IDF export.bat...
set "IDF_EXPORT="

if defined IDF_PATH if exist "%IDF_PATH%\export.bat" set "IDF_EXPORT=%IDF_PATH%\export.bat"

if not defined IDF_EXPORT call :search_export "%PROJECT_DIR%\.."
if not defined IDF_EXPORT call :search_export "%PROJECT_DIR%\..\.."
if not defined IDF_EXPORT call :search_export "%USERPROFILE%\esp"
if not defined IDF_EXPORT call :search_export "C:\Espressif"
if not defined IDF_EXPORT call :search_export "D:\Espressif"

if not defined IDF_EXPORT (
    echo.
    echo ESP-IDF export.bat nebyl automaticky nalezen.
    set /p "IDF_EXPORT=Zadej plnou cestu k export.bat: "
)

if not defined IDF_EXPORT exit /b 1
if not exist "%IDF_EXPORT%" (
    echo CHYBA: soubor neexistuje: %IDF_EXPORT%
    exit /b 1
)

echo [IDF] Aktivace: %IDF_EXPORT%
call "%IDF_EXPORT%"
if errorlevel 1 exit /b 1

where idf.py >nul 2>nul
if errorlevel 1 (
    echo CHYBA: export.bat probehl, ale idf.py stale neni dostupny.
    exit /b 1
)
exit /b 0

:search_export
set "SEARCH_ROOT=%~1"
if not exist "%SEARCH_ROOT%" exit /b 0
for /f "usebackq delims=" %%E in (`powershell -NoProfile -Command "$r=[IO.Path]::GetFullPath('%SEARCH_ROOT%'); Get-ChildItem -Path $r -Filter export.bat -File -Recurse -ErrorAction SilentlyContinue ^| Where-Object {$_.FullName -match '\\esp-idf[^\\]*\\export\.bat$'} ^| Select-Object -First 1 -ExpandProperty FullName"`) do set "IDF_EXPORT=%%E"
exit /b 0

:find_port
set "ESP_PORT="
for /f "usebackq delims=" %%P in (`python tools\find_usb_serial_jtag.py`) do set "ESP_PORT=%%P"
if not defined ESP_PORT (
    echo CHYBA: ESP32-C6 built-in USB Serial/JTAG COM port nebyl nalezen.
    exit /b 1
)
echo [PORT] Pouzivam %ESP_PORT%
exit /b 0

:usage
echo Pouziti:
echo   %~nx0       zkontroluje Git; jen pri novych commitech provede pull + submoduly + build
echo   %~nx0 -f    stejne jako vyse, po uspesnem buildu flash + monitor
echo   %~nx0 -m    pouze monitor, bez Git kontroly a bez buildu
exit /b 2

:error
echo.
echo ========================================
echo CHYBA - operace byla zastavena
echo ========================================
exit /b 1
