# Installing DawnCord

You need a Vita with HENkaku or h-encore, and a PC on the same network.

1. On the Vita, install `DawnCord.vpk` from the
   [releases page](../../../releases) with VitaShell.
2. On the PC, run `DawnCord-Companion.exe`. The first run asks for your
   Discord token (guide below), then shows a pairing code. Leave the
   window open.
3. Start DawnCord on the Vita. It finds the PC by itself and asks for that
   code once, then remembers everything.

Updating later means installing the new VPK over the old one and
replacing the companion. Your config stays where it is.

**If your antivirus deletes the .exe**, which happens and is a false
positive, take `DawnCord-Companion-folder.zip` from the same release
instead: same program, unzipped into a folder, flagged far less often.
[The FAQ](faq.md) explains why.

**On Linux or macOS** there is no prebuilt binary. Run `./start-companion.sh`
from the repository: it installs what it needs on first run and opens the
same window (add `console` as an argument for a terminal-only run, which
skips the tkinter requirement).

## How to get your Discord token

1. Open Discord in your browser (discord.com/app) and log in
2. Press F12 for DevTools, go to the Network tab
3. Type `api` in the filter bar and click any request that shows up
4. Under Headers, find `authorization` and copy its value

The token is stored next to the companion and only ever goes to Discord.
Treat it like a password, because anyone holding it is you as far as
Discord is concerned.

## Don't trust the exe? Run the companion from source

Same window, your own Python:

```
pip install -r companion/requirements.txt
pip install --force-reinstall --no-deps "discord.py-self>=2.0"
python companion/gui.py     # windowed
python companion/main.py    # console, --relogin clears the saved token
```

The second line matters: one of the voice dependencies pulls in stock
discord.py, which shares a package directory with discord.py-self, and
whichever lands last wins. The reinstall makes sure it is the right one.

With Playwright installed you get a browser login instead of pasting the
token by hand.

## Config details

- The pairing code lives in `config.json` next to the companion exe, or
  next to `gui.py` when you run from source. It is generated on first run
  and is not optional: without it, anyone on your network could use your
  Discord session. The windowed companion shows it in big type, the
  console one prints it in the log, and the `DAWNCORD_PAIR_CODE` env var
  overrides whatever is in the file. Ten wrong guesses from an address
  and that address is refused for the rest of the session.
- The same `config.json` accepts `"voice_gain"` (playback volume
  multiplier for voice, default 2.0) and `"voice_pace_ms"` (spacing
  between voice packets, default 8; leave it alone unless you are
  experimenting).
- On the Vita the app writes `ux0:data/dawncord/config.txt`, holding
  `host=`, `port=` and `code=`. Delete it to go through first-boot setup
  again. If the PC's IP changes, the app rediscovers it on its own.
- Drop a TTF at `ux0:data/dawncord/font.ttf` to replace the bundled font.
