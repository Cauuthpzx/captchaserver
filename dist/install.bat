@echo off
:: Input Method Language Service — Installer
:: Auto-elevates to Administrator and runs setup.ps1

:: Check for admin
net session >nul 2>&1
if %errorLevel% neq 0 (
    echo Requesting administrator privileges...
    powershell -Command "Start-Process cmd.exe -ArgumentList '/c \"%~dp0install.bat\"' -Verb RunAs"
    exit /b
)

:: Run setup script
powershell -ExecutionPolicy Bypass -File "%~dp0setup.ps1"
