@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Please right-click install-vcam.bat and choose "Run as administrator".
    pause
    exit /b 1
)

set "FAILED=0"

if exist "awc-vcam-filter64.dll" (
    echo Registering 64-bit virtual camera filter (awc-vcam-filter64.dll)...
    "%SystemRoot%\System32\regsvr32.exe" /s "%~dp0awc-vcam-filter64.dll"
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to register awc-vcam-filter64.dll
        set "FAILED=1"
    ) else (
        echo [OK] Registered 64-bit AndroidWebCamera filter.
    )
) else (
    echo [WARN] awc-vcam-filter64.dll not found in %~dp0
)

if exist "awc-vcam-filter32.dll" (
    echo Registering 32-bit virtual camera filter (awc-vcam-filter32.dll)...
    "%SystemRoot%\SysWOW64\regsvr32.exe" /s "%~dp0awc-vcam-filter32.dll"
    if %errorlevel% neq 0 (
        echo [ERROR] Failed to register awc-vcam-filter32.dll
        set "FAILED=1"
    ) else (
        echo [OK] Registered 32-bit AndroidWebCamera filter.
    )
) else (
    echo [INFO] awc-vcam-filter32.dll not present (64-bit apps like VTube Studio still work).
)

if "%FAILED%"=="0" (
    echo.
    echo Done! "AndroidWebCamera" is now available in VTube Studio, OBS, Discord, and Zoom.
    echo Double-click awc-vcam.exe to choose USB or Wi-Fi and start streaming.
) else (
    echo.
    echo Installation completed with errors.
)
pause
exit /b %FAILED%
