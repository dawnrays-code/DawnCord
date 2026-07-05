"""
DawnCord Companion, windowed edition: double-click, log in once, read the
pairing code off the window, launch DawnCord on the Vita.

Same engine as main.py (which remains the console way to run it). This file
only adds the window: a token prompt on first run, the LAN IP and pairing
code in big type, and a live log pane. CI freezes it into
DawnCord-Companion.exe with PyInstaller. The exe logs in by token paste;
browser login stays a from-source extra (Playwright doesn't freeze well).
"""

import asyncio
import json
import logging
import queue
import secrets
import socket
import threading
import tkinter as tk
from tkinter import scrolledtext

import discord

import auth
import vita_server as vita_server_mod
from discord_bridge import DiscordBridge
from paths import BASE_DIR
from vita_server import VitaServer

VITA_PORT = 9100

BG = "#313338"
PANEL = "#2b2d31"
TEXT = "#dcdde2"
DIM = "#949ba4"
ACCENT = "#5865f2"
GREEN = "#23a55a"
RED = "#f23f43"

log = logging.getLogger("dawncord")

TOKEN_HELP = """\
DawnCord logs in as you, so it needs your Discord token:

  1. open Discord in your browser (discord.com/app) and log in
  2. press F12 (DevTools) and open the Network tab
  3. type "api" in the filter bar and click any request
  4. under Headers, find "authorization"
  5. copy its value and paste it below

The token is saved next to this app and never leaves your PC
(it is only ever sent to Discord itself)."""


def lan_ip() -> str:
    """Best-effort LAN address (UDP connect() sends no packets)."""
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 80))
        return s.getsockname()[0]
    except OSError:
        return "unknown"
    finally:
        s.close()


def ensure_pair_code() -> str:
    """Pair code from config.json, generated and saved on first run so the
    companion is never accidentally left open to the whole LAN."""
    path = BASE_DIR / "config.json"
    try:
        cfg = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError):
        cfg = {}
    code = str(cfg.get("pair_code") or "").strip()
    if not code:
        code = f"{secrets.randbelow(1_000_000):06d}"
        cfg["pair_code"] = code
        path.write_text(json.dumps(cfg, indent=2))
    return code


class QueueHandler(logging.Handler):
    """Log records cross from the asyncio thread to tkinter via a queue:
    tkinter widgets must only be touched from the main thread."""

    def __init__(self, q: queue.Queue):
        super().__init__()
        self.q = q

    def emit(self, record):
        self.q.put(self.format(record))


class CompanionGui:
    def __init__(self, root: tk.Tk):
        self.root = root
        root.title("DawnCord Companion")
        root.configure(bg=BG)
        root.geometry("720x520")
        root.minsize(560, 420)

        head = tk.Frame(root, bg=PANEL)
        head.pack(fill="x")
        tk.Label(head, text="DawnCord Companion", bg=PANEL, fg="white",
                 font=("Segoe UI", 16, "bold")).pack(anchor="w", padx=16, pady=(12, 0))
        self.status = tk.Label(head, text="Starting...", bg=PANEL, fg=DIM,
                               font=("Segoe UI", 10))
        self.status.pack(anchor="w", padx=16, pady=(0, 10))

        row = tk.Frame(root, bg=BG)
        row.pack(fill="x", padx=16, pady=(12, 4))
        self._big_value(row, "PC address", lan_ip())
        self.code_label = self._big_value(row, "Pairing code", "......")

        tk.Label(root, text="Launch DawnCord on the Vita: it finds this PC on "
                            "its own and asks for the pairing code once.",
                 bg=BG, fg=DIM, font=("Segoe UI", 9)).pack(anchor="w", padx=18)

        self.logbox = scrolledtext.ScrolledText(
            root, bg=PANEL, fg=TEXT, insertbackground=TEXT, relief="flat",
            font=("Consolas", 9), state="disabled", wrap="word")
        self.logbox.pack(fill="both", expand=True, padx=16, pady=(8, 16))

        self.log_q: queue.Queue = queue.Queue()
        handler = QueueHandler(self.log_q)
        fmt = logging.Formatter("%(asctime)s [%(name)s] %(levelname)s: %(message)s",
                                "%H:%M:%S")
        handler.setFormatter(fmt)
        file_handler = logging.FileHandler(BASE_DIR / "dawncord.log", encoding="utf-8")
        file_handler.setFormatter(fmt)
        logging.basicConfig(level=logging.INFO, handlers=[handler, file_handler])

        self.events: queue.Queue = queue.Queue()
        root.after(150, self._poll)

    def _big_value(self, parent, caption: str, value: str) -> tk.Label:
        box = tk.Frame(parent, bg=PANEL)
        box.pack(side="left", padx=(0, 12))
        tk.Label(box, text=caption, bg=PANEL, fg=DIM,
                 font=("Segoe UI", 9)).pack(anchor="w", padx=12, pady=(8, 0))
        lbl = tk.Label(box, text=value, bg=PANEL, fg="white",
                       font=("Consolas", 20, "bold"))
        lbl.pack(anchor="w", padx=12, pady=(0, 8))
        return lbl

    # ---- lifecycle ----

    def start(self):
        code = ensure_pair_code()
        vita_server_mod.PAIR_CODE = code   # module global read at handshake time
        self.code_label.config(text=code)

        token = auth.load_saved_token()
        if not token:
            token = self._prompt_token()
            if not token:
                self.root.destroy()
                return
            auth.save_token(token)
        self._set_status("Connecting to Discord...", DIM)
        self._launch(token)

    def _launch(self, token: str):
        threading.Thread(target=self._thread_main, args=(token,),
                         daemon=True).start()

    def _thread_main(self, token: str):
        try:
            asyncio.run(self._serve(token))
        except Exception:
            log.exception("Companion stopped unexpectedly")
            self.events.put("crashed")

    async def _serve(self, token: str):
        bridge = DiscordBridge(token)
        server = VitaServer(bridge, port=VITA_PORT)
        bridge.set_vita_server(server)
        try:
            await server.start()
        except OSError:
            # The single most common failure in the field: an older
            # companion window is still open somewhere holding the port,
            # and the Vita keeps talking to THAT one.
            log.error("Port %s is already in use: another companion is "
                      "still running. Close it (check the taskbar or Task "
                      "Manager) and reopen this app.", VITA_PORT)
            self.events.put("crashed")
            return
        log.info("Waiting for the Vita on port %s", VITA_PORT)
        try:
            await bridge.start()
        except discord.LoginFailure:
            log.error("Discord rejected the token.")
            self.events.put("relogin")

    # ---- token dialog ----

    def _prompt_token(self) -> str | None:
        dlg = tk.Toplevel(self.root)
        dlg.title("Log in to Discord")
        dlg.configure(bg=BG)
        dlg.transient(self.root)
        dlg.grab_set()

        tk.Label(dlg, text=TOKEN_HELP, bg=BG, fg=TEXT, justify="left",
                 font=("Segoe UI", 10)).pack(padx=16, pady=(14, 6), anchor="w")
        entry = tk.Entry(dlg, width=64, bg=PANEL, fg=TEXT, show="*",
                         insertbackground=TEXT, relief="flat")
        entry.pack(padx=16, pady=6, ipady=5, fill="x")
        entry.focus_set()

        result: list = [None]

        def ok(*_):
            result[0] = entry.get().strip() or None
            dlg.destroy()

        tk.Button(dlg, text="Save and connect", command=ok, bg=ACCENT,
                  fg="white", relief="flat", font=("Segoe UI", 10, "bold"),
                  activebackground=ACCENT, activeforeground="white"
                  ).pack(pady=(6, 14), ipadx=12, ipady=3)
        entry.bind("<Return>", ok)
        self.root.wait_window(dlg)
        return result[0]

    # ---- main-thread pump ----

    def _set_status(self, text: str, color: str):
        self.status.config(text=text, fg=color)

    def _append(self, line: str):
        self.logbox.config(state="normal")
        self.logbox.insert("end", line + "\n")
        self.logbox.see("end")
        self.logbox.config(state="disabled")

    def _poll(self):
        try:
            while True:
                line = self.log_q.get_nowait()
                self._append(line)
                # Cheap but effective status line: piggyback on known logs.
                if "Discord connected as" in line:
                    self._set_status("Connected to Discord. Launch DawnCord "
                                     "on the Vita.", GREEN)
                elif "Vita connected" in line:
                    self._set_status("Vita connected!", GREEN)
        except queue.Empty:
            pass

        try:
            while True:
                ev = self.events.get_nowait()
                if ev == "relogin":
                    auth.clear_token()
                    self._set_status("Token rejected: log in again.", RED)
                    token = self._prompt_token()
                    if token:
                        auth.save_token(token)
                        self._set_status("Connecting to Discord...", DIM)
                        self._launch(token)
                elif ev == "crashed":
                    self._set_status("Companion stopped: see the log below.", RED)
        except queue.Empty:
            pass

        self.root.after(150, self._poll)


def main():
    root = tk.Tk()
    app = CompanionGui(root)
    root.after(50, app.start)
    root.mainloop()


if __name__ == "__main__":
    main()
