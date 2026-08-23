@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "TAG=%~1"
if "%TAG%"=="" set "TAG=25.7.23"
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%SCRIPT_DIR%fetch_vdd_package.ps1" -Tag "%TAG%"
exit /b %errorlevel%
