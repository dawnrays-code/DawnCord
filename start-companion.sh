#!/usr/bin/env bash
# DawnCord companion launcher for Linux and macOS.
# There is no prebuilt binary for these: the companion is plain Python and
# runs from source, which is also the packaging antivirus software never
# argues with.
#
#   ./start-companion.sh            windowed
#   ./start-companion.sh console    terminal only, no tkinter needed
set -euo pipefail
cd "$(dirname "$0")"

PY=""
for candidate in python3 python; do
    if command -v "$candidate" >/dev/null 2>&1; then
        PY="$candidate"
        break
    fi
done
if [ -z "$PY" ]; then
    echo "Python 3 not found. Install it from your package manager and retry."
    exit 1
fi

# discord-ext-voice-recv depends on stock discord.py, which owns the same
# "discord" package directory as discord.py-self; whichever pip installs
# last wins. So the check is not "is discord importable" but "is it the
# right one": stock discord.py refuses to build a Client without intents.
if ! "$PY" -c "import discord; discord.Client()" >/dev/null 2>&1; then
    echo "First run: installing dependencies..."
    "$PY" -m pip install -r companion/requirements.txt
    "$PY" -m pip install --force-reinstall --no-deps "discord.py-self>=2.0"
    if ! "$PY" -c "import discord; discord.Client()" >/dev/null 2>&1; then
        echo
        echo "Dependency install failed: the wrong Discord library is installed."
        echo 'Try: pip install --force-reinstall --no-deps "discord.py-self>=2.0"'
        exit 1
    fi
fi

if [ "${1:-}" = "console" ]; then
    "$PY" companion/main.py
else
    # The window needs tkinter, which several distributions package
    # separately from Python itself. Say so rather than dying on an
    # import nobody expects to be optional.
    if ! "$PY" -c "import tkinter" >/dev/null 2>&1; then
        echo "tkinter is missing, so the window cannot open."
        echo "  Debian/Ubuntu:  sudo apt install python3-tk"
        echo "  Fedora:         sudo dnf install python3-tkinter"
        echo "  Arch:           sudo pacman -S tk"
        echo "  macOS (brew):   brew install python-tk"
        echo
        echo "Or run it in the terminal instead: ./start-companion.sh console"
        exit 1
    fi
    "$PY" companion/gui.py
fi

echo
echo "Companion stopped. See companion/dawncord.log for details."
