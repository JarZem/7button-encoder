@echo off
setlocal

cd /d "%~dp0"

 echo [1/5] Git pull...
git pull
if errorlevel 1 goto :error

 echo [2/5] Updating submodules...
git submodule update --init --recursive
if errorlevel 1 goto :error

 echo [3/5] ESP-IDF fullclean...
call idf.py fullclean
if errorlevel 1 goto :error

 echo [4/5] Building firmware...
call idf.py build
if errorlevel 1 goto :error

 echo [5/5] Flashing firmware...
call idf.py flash
if errorlevel 1 goto :error

 echo.
echo ========================================
echo HOTOVO - update, build i flash probehly OK
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
