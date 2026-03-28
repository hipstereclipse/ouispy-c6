@echo off
:: ╔═══════════════════════════════════════════╗
:: ║         OUI-SPY C6 Firmware Flasher       ║
:: ║   Double-click to flash your board!       ║
:: ╚═══════════════════════════════════════════╝
title OUI-Spy C6 Flasher

:: Check for Python
where python >nul 2>nul
if %ERRORLEVEL% neq 0 (
    echo.
    echo   [ERROR] Python not found!
    echo.
    echo   Install Python 3.8+ from:
    echo     https://www.python.org/downloads/
    echo.
    echo   IMPORTANT: Check "Add Python to PATH" during install.
    echo.
    pause
    exit /b 1
)

:: Run the interactive wizard
python "%~dp0flash.py" %*
