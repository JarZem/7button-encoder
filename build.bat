@echo off

git pull
if errorlevel 1 exit /b %errorlevel%

idf.py build
if errorlevel 1 exit /b %errorlevel%

idf.py -p COM3 flash monitor
