@echo off
setlocal

cd /d "%~dp0"

set "DO_FLASH=0"
if /I "%~1"=="-f" set "DO_FLASH=1"

if not "%~1"=="" if /I not "%~1"=="-f" (
    echo Usage: %~nx0 [-f]
    echo   without parameter = update + build
    echo   -f                = update + build + flash monitor
    exit /b 2
)

echo [1/3] Git pull...
git pull
if errorlevel 1 goto :error

echo [2/3] Updating submodules...
git submodule update --init --recursive
if errorlevel 1 goto :error

echo [3/3] Building firmware...
call idf.py build
if errorlevel 1 goto :error

if "%DO_FLASH%"=="1" (
    echo.
    echo Flashing firmware and starting monitor...
    call idf.py flash monitor
    if errorlevel 1 goto :error
)

echo.
echo ========================================
if "%DO_FLASH%"=="1" (
    echo HOTOVO - update, build, flash a monitor probehly OK
) else (
    echo HOTOVO - update a build probehly OK
)
echo ========================================
pause
exit /b 0

:error
echo.
echo ========================================
echo CHYBA - operace byla zastavena
echo ========================================
pause
exit /b 1
