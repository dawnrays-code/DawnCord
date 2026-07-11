# DawnCord

A Discord thin client for the PlayStation Vita, with a PC companion app doing
all the heavy lifting.

> **Status: alpha, but works.** It runs on actual hardware against a real
> Discord account. Expect rough edges anyway.

## What is this?

This is yet another client for Discord for the PS Vita. It features a modern,
Discord-familiar UI (channels, chat and member list side by side), live
messages, avatars, real-time user status, plug and play pairing with your PC,
and a roadmap that points straight at voice chat support.

It takes inspiration from [VitaCord](https://github.com/devingDev/VitaCord),
the original port from many extremely talented and respected individuals in
the community, which unfortunately dates back to when Discord had just come
out (2016 era), and for that reason is missing many features that were
introduced later on.

A personal note: while I've been working in IT for a decade as a sysadmin,
unfortunately I don't have much experience in coding. For that reason, this
project was primarily vibe-coded with Claude (Fable 5), and I can't guarantee
the solid codebase you'd expect from a senior engineer/developer. Also, it
cannot replicate the joy of figuring out why the code wouldn't run or compile
at 4 AM. It's something you only get by spending time and effort on your
creation, a personal joy that AI coding does not convey. Well, I still have
extensive knowledge in clients, networks and what can feasibly be achieved by
such tools, which helped me direct the workflow and the specifics. But yeah,
the point above still stands.

Anyway, technical and existential AI considerations aside, it came out to be
a fun and decently working project, which can bring a cool app we all use
every day to the PS Vita, and for that I'm really thankful.

Also, I got my hands on a Vita for the first time only around 2 months ago,
since I could not really afford it when I was younger. And, thanks to
incredible tools like Moonlight-vita by xyzz, it's pretty much a PlayStation
Portal with an OLED screen. Porting over Discord only seemed right.

I hope you can try it out and let me know if it works.

## Do I need a lot of free space to install the app?

No! The .vpk is only ~200 kilobytes.

![hello i am 200kilobyte i am tiny](docs/tiny.jpg)

## Is this safe? Can I get banned?

Well... yes, there is some risk, and it's only fair to spell it out.

The companion logs in with your user token (a so-called self-bot), because a
bot account can't see your DMs and servers the way you do. Automating a user
account is against Discord's Terms of Service, full stop.

In practice the risk looks small for this use case: the companion behaves
like a regular Discord session (it reads what you'd read and sends what you
type, at human pace, with no automation and no scraping), and enforcement
historically targets spam and abuse. But nobody outside Discord can promise
anything. Treat it as use at your own risk, and if that bothers you, use a
throwaway account. This project is for personal, educational use.

## Features

Working today:

- [x] Server list and DMs, with icons
- [x] Channels in the same order as the official sidebar, grouped by category
- [x] Three-column layout: channels, chat, member list with presence dots
- [x] Live message push (no polling), avatars, Discord-style grouping
- [x] Sending messages with the native on-screen keyboard
- [x] Embeds rendered as Discord-style boxes (color bar, title, description)
- [x] Image previews in chat: thumbnails inline, tap one to expand it
      Discord-style over the dimmed chat
- [x] Typing indicator, round avatars, your profile in the corner
- [x] Plug and play: the Vita finds the PC by itself, a pairing code keeps
      the rest of your LAN out
- [x] Companion GUI for Windows (.exe, no Python needed)
- [x] Crisp TTF text everywhere (Inter bundled); drop your own at
      `ux0:data/dawncord/font.ttf` to override it
- [x] Voice channels in the sidebar with who's connected; joining them
      (listen-only) is in, still experimental

On the way, roughly in order:

- [ ] Talking back in voice with the Vita's built-in mic
- [ ] Update check (the client tells you when you're behind)
- [ ] Animated emoji, stickers (transcoded by the companion)
- [ ] Emoji in channel names and messages, served as tiny images
- [ ] Video and YouTube playback: even mentioning it feels weird, so let's
      call it fantasy tier for now.

## Installation

Two files from the [releases page](../../releases), and you're in.
You need a Vita with HENkaku/h-encore and a PC on the same network.

1. **Vita**: install `DawnCord.vpk` with VitaShell.
2. **PC**: run `DawnCord-Companion.exe`. First run: paste your Discord token
   (guide below), a pairing code appears on screen. Leave the window open.
3. **Launch DawnCord on the Vita**: it finds the PC by itself and asks for
   that code, once. Everything is remembered from then on.

To update later: install the new VPK over the old one, replace the exe. Your
config survives.

<details>
<summary><b>How to get your Discord token</b></summary>

1. Open Discord in your browser (discord.com/app) and log in
2. Press F12 for DevTools, go to the Network tab
3. Type `api` in the filter bar and click any request
4. Under Headers, find `authorization` and copy its value

The token is stored next to the companion and only ever sent to Discord.
Treat it like a password: anyone who has it IS you.
</details>

<details>
<summary><b>Don't trust the exe? Run the companion from source</b></summary>

Same window, your own Python:

```
pip install -r companion/requirements.txt
python companion/gui.py     # windowed
python companion/main.py    # console, --relogin clears the saved token
```

With Playwright installed, a browser-login flow replaces the token paste.
</details>

<details>
<summary><b>Config details (pair code, Vita-side file)</b></summary>

- The pairing code lives in `config.json` next to the companion exe (or next
  to `gui.py` when run from source); the GUI generates one on first run. The
  `DAWNCORD_PAIR_CODE` env var overrides it.
- The Vita writes `ux0:data/dawncord/config.txt` (`host=`, `port=`, `code=`).
  Delete it to redo the first-boot setup; if the PC's IP changes, the app
  rediscovers it on its own.
</details>

### Controls

Inside a server the screen splits Discord-style: channels on the left, chat
in the middle, member list (with presence dots) on the right. UP/DOWN acts on
whichever column has the focus; the chat is the only one that scrolls
messages.

| Button    | Action                               |
|-----------|--------------------------------------|
| D-pad / analog sticks | move selection, scroll the focused column |
| Left/Right or L/R | move focus: channels, chat, members |
| Cross     | open server / channel                |
| Circle    | back to the server list              |
| Triangle  | refresh messages and members         |
| Start     | write a message (on-screen keyboard) |
| Select    | quit                                 |

## Under the hood

```
+-----------------+         LAN (TCP 9100)        +------------------+
|    PS Vita      | <---------------------------> |  PC Companion    |
|  (thin client)  |   binary frames + JSON        |  (Python)        |
|                 |                               |                  |
|  vita2d UI      |                               |  discord.py-self |
|  input, render  |                               |  full Discord    |
+-----------------+                               +---------+--------+
                                                            |
                                                            v
                                                        Discord
```

The Vita never talks to Discord directly. The proxy design keeps all the
modern-web pain (TLS, gateway, image decoding) on the PC, where it's easy,
and it means features like voice can later be routed through the PC too. The
consequence: the companion must be running whenever you use the client.

Protocol, in three rules: the first client frame must be `HANDSHAKE` (with
the pairing code if set); requests are fire-and-forget and replies are routed
by message type from a single reader, because the server can push at any
moment; all snowflake IDs travel as JSON strings, since they'd lose precision
through a double-based JSON parser like cJSON.

## Development

CI builds everything on every push: the VPK (official VitaSDK Docker image),
the Windows companion exe (PyInstaller), and runs the test suites. Releases
also carry `DawnCord-debug-elf.zip`; if the app ever crashes on you,
`python tools/crash_symbols.py <psp2core dump> <debug ELF>` turns the dump
into plain function names you can paste in an issue.

Building the VPK locally needs [VitaSDK](https://vitasdk.org) (Linux/macOS):

```
cd vita-client
cmake -S . -B build && cmake --build build   # -> build/DawnCord.vpk
```

Testing without a Vita:

```
python test-client/integration_test.py   # companion vs fake Discord, no token
python test-client/test_client.py <ip>   # interactive fake Vita, real companion

cd vita-client                            # C parsing layer, plain gcc
gcc -Wall -Iinclude -Isrc/cjson test/state_test.c src/state.c src/b64.c \
    src/cjson/cJSON.c -o state_test && ./state_test
```

## End notes

Built by **dawnrays** & **Claude** (Anthropic's Fable 5).

- [VitaSDK](https://vitasdk.org) + [vita2d](https://github.com/xerpi/libvita2d)
- [cJSON](https://github.com/DaveGamble/cJSON) (vendored, MIT)
- [discord.py-self](https://github.com/dolfies/discord.py-self)
- [Playwright](https://playwright.dev) (optional, login only)

Thanks to devingDev for the original
[VitaCord](https://github.com/devingDev/VitaCord), proving there's an active
interest in the community.

## License

MIT, see [LICENSE](LICENSE). Vendored cJSON keeps its own MIT notice.
