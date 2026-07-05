"""
DawnCord - Companion App

Connects to Discord as your user account and serves data to the
Vita thin client over local network.

Usage:
    python main.py            normal start (login on first run)
    python main.py --relogin  force re-login (clear saved token)

For the windowed version (same engine), see gui.py.
"""

import asyncio
import logging
import sys

import discord

from auth import get_token, clear_token, invalidate_and_relogin
from discord_bridge import DiscordBridge
from paths import BASE_DIR
from vita_server import VitaServer

# Everything also goes to dawncord.log (next to this file, or next to the
# .exe) so crashes can be reported even after the terminal is gone.
LOG_FILE = BASE_DIR / "dawncord.log"
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(name)s] %(levelname)s: %(message)s",
    handlers=[
        logging.StreamHandler(),
        logging.FileHandler(LOG_FILE, encoding="utf-8"),
    ],
)
log = logging.getLogger("dawncord")

VITA_PORT = 9100


async def serve(token: str) -> str | None:
    """
    Run the companion with the given token.
    Returns a fresh token if the current one was rejected (caller retries),
    or None on a clean shutdown.
    """
    bridge = DiscordBridge(token)
    server = VitaServer(bridge, port=VITA_PORT)
    bridge.set_vita_server(server)

    await server.start()
    log.info("Vita server ready on port %s", VITA_PORT)
    log.info("Connecting to Discord...")

    try:
        await bridge.start()
    except discord.LoginFailure:
        log.error("Discord rejected the token.")
        # Re-login is interactive (input/browser); do it outside the loop.
        return "RELOGIN"
    return None


def main():
    print()
    print("  DawnCord - Companion")
    print()

    if "--relogin" in sys.argv:
        clear_token()

    # IMPORTANT: token acquisition (input() / Playwright sync API) must happen
    # BEFORE the asyncio loop starts. Playwright's sync API raises if called
    # from inside a running event loop, and input() would block it.
    token = get_token()

    try:
        while True:
            result = asyncio.run(serve(token))
            if result == "RELOGIN":
                token = invalidate_and_relogin()
                log.info("Retrying with new token...")
                continue
            break
    except KeyboardInterrupt:
        log.info("Shutting down.")


if __name__ == "__main__":
    main()
