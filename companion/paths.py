"""Where the companion keeps its files (.token, config.json, dawncord.log).

Run from source, that's simply the companion/ folder. Frozen into an .exe
(PyInstaller onefile) __file__ points inside a throwaway unpack directory
that changes on every start, so the files go next to the .exe instead,
where they survive restarts.
"""
import sys
from pathlib import Path

if getattr(sys, "frozen", False):
    BASE_DIR = Path(sys.executable).resolve().parent
else:
    BASE_DIR = Path(__file__).resolve().parent
