@echo off
rem DawnCord companion launcher: double-click and go.
cd /d "%~dp0"

where python >nul 2>nul
if errorlevel 1 (
    echo Python not found. Install it from https://python.org and retry.
    pause
    exit /b 1
)

rem discord-ext-voice-recv depends on stock discord.py, which owns the same
rem "discord" package directory as discord.py-self; whichever pip installs
rem last wins. So the check is not "is discord importable" but "is it the
rem right one": stock discord.py refuses to build a Client without intents.
python -c "import discord; discord.Client()" >nul 2>nul
if errorlevel 1 (
    echo First run: installing dependencies...
    python -m pip install -r companion\requirements.txt
    python -m pip install --force-reinstall --no-deps "discord.py-self>=2.0"
    python -c "import discord; discord.Client()" >nul 2>nul
    if errorlevel 1 (
        echo.
        echo Dependency install failed: the wrong Discord library is installed.
        echo Try: python -m pip install --force-reinstall --no-deps "discord.py-self>=2.0"
        pause
        exit /b 1
    )
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
