@echo off
rem DawnCord companion launcher: double-click and go.
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo Python not found. Install it from https://python.org and retry.
    pause
    exit /b 1
)

python -c "import discord" >nul 2>nul
if errorlevel 1 (
    echo First run: installing dependencies...
    python -m pip install -r companion\requirements.txt
)

rem Windowed companion (pass "console" as first arg for the old behavior).
if "%~1"=="console" (
    python companion\main.py
) else (
    python companion\gui.py
)
echo.
echo Companion stopped. See companion\dawncord.log for details.
pause
