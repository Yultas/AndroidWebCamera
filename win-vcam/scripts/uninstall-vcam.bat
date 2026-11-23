@echo off
setlocal
cd /d "%~dp0"

net session >nul 2>&1
if %errorlevel% neq 0 (
    echo [ERROR] Please right-click uninstall-vcam.bat and choose "Run as administrator".
    pause
    exit /b 1
)

if exist "awc-vcam-filter64.dll" (
    echo Unregistering 64-bit virtual camera filter...
    "%SystemRoot%\System32\regsvr32.exe" /u /s "%~dp0awc-vcam-filter64.dll"
)

if exist "awc-vcam-filter32.dll" (
    echo Unregistering 32-bit virtual camera filter...
    "%SystemRoot%\SysWOW64\regsvr32.exe" /u /s "%~dp0awc-vcam-filter32.dll"
)

echo Done! AndroidWebCamera virtual camera filter has been unregistered.
pause
exit /b 0
