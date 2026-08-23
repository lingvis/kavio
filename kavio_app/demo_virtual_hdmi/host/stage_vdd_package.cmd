@echo off
setlocal EnableExtensions
set "SCRIPT_DIR=%~dp0"
set "SOURCE_DIR=%SCRIPT_DIR%vdd_package"
set "HOST_OUTPUT_DIR=%SCRIPT_DIR%..\output\host"
set "OUTPUT_DIR=%HOST_OUTPUT_DIR%\vdd"
set "TEMP_DIR=%HOST_OUTPUT_DIR%\vdd.staging"

set "MISSING=0"
for %%F in (MttVDD.inf MttVDD.dll mttvdd.cat vdd_settings.xml VDD_LICENSE.txt VDD_SHA256.txt) do (
  if not exist "%SOURCE_DIR%\%%F" (
    echo Missing VDD file: %SOURCE_DIR%\%%F
    set "MISSING=1"
  )
)
if "%MISSING%"=="1" (
  echo.
  echo VDD package is incomplete. Run:
  echo   host\fetch_vdd_package.cmd
  echo and rebuild the Host project.
  exit /b 1
)

if not exist "%HOST_OUTPUT_DIR%" mkdir "%HOST_OUTPUT_DIR%"
if errorlevel 1 exit /b 1
if exist "%TEMP_DIR%" rmdir /S /Q "%TEMP_DIR%"
mkdir "%TEMP_DIR%"
if errorlevel 1 exit /b 1

for %%F in (MttVDD.inf MttVDD.dll mttvdd.cat vdd_settings.xml VDD_LICENSE.txt VDD_SHA256.txt README.txt) do (
  if exist "%SOURCE_DIR%\%%F" copy /Y "%SOURCE_DIR%\%%F" "%TEMP_DIR%\%%F" >nul || exit /b 1
)

if exist "%OUTPUT_DIR%" rmdir /S /Q "%OUTPUT_DIR%"
if errorlevel 1 exit /b 1
move /Y "%TEMP_DIR%" "%OUTPUT_DIR%" >nul
if errorlevel 1 exit /b 1

echo Staged VirtualDrivers VDD package: %OUTPUT_DIR%
exit /b 0
