# DawnCord

Discord on the PlayStation Vita, for real: servers, DMs, live chat, images
and voice channels, with a PC companion on your LAN doing the heavy lifting.

![DawnCord chat on a PS Vita](docs/screen-chat.png)

## Download

Two files from the [releases page](../../releases), that's the whole setup:

| File | Where it goes |
|------|---------------|
| `DawnCord.vpk` | the Vita, installed with VitaShell |
| `DawnCord-Companion.exe` | the PC, just run it and leave the window open |

On first run the companion asks for your Discord token and shows a pairing
code. Start the app on the Vita, it finds the PC by itself, asks for that
code once and remembers everything. The full walkthrough, token guide
included, is in [docs/install.md](docs/install.md).

Two extras in the same release, for when the plain .exe does not suit:
`DawnCord-Companion-folder.zip` is the same program as a folder, which
antivirus software objects to far less often (see
[the FAQ](docs/faq.md) if yours eats the .exe), and Linux and macOS run
it from source with `./start-companion.sh`.

## What is this?

Yet another Discord client for the PS Vita, with a modern UI and the
features you actually expect in 2026. It takes inspiration from
[VitaCord](https://github.com/devingDev/VitaCord), which dates back to when
Discord had just come out, and tries to be what that project would look
like today. The Vita never talks to Discord directly: everything a modern
web stack demands stays on the PC, and the console gets a clean feed over
the local network.

It was vibe-coded with Claude by a sysadmin who can read code better than
he writes it. The longer story, and what I think about that, is in
[docs/about.md](docs/about.md).

## What it looks like

| Your servers | Inside a server |
|---|---|
| ![server grid](docs/screen-servers.png) | ![channel list](docs/screen-channels.png) |

| Embeds and voice | Chat with images |
|---|---|
| ![embeds and a voice channel](docs/screen-voice.png) | ![chat](docs/screen-chat.png) |

## What works

- Live chat: message push with no polling, avatars, embeds, image
  previews you can tap to expand, typing indicator
- Discord-style layout: channels, chat and member list side by side, the
  focused column growing to take the space
- Voice channels: join and listen, see who is connected and whose dot
  lights up while they talk. Newest feature, still settling.
- Writing with the native on-screen keyboard
- Pairing that sorts itself out on the LAN, and reconnection that does too
- Crisp TTF text everywhere, drawn natively at 60fps

On the way: talking back with the Vita's mic, voice that survives
launching a game (as a taiHEN plugin), an update check, animated emoji.

A note on voice: Discord end-to-end encrypts calls since March 2026, and
the companion is the end that holds the keys. The console just plays what
it is handed. Listening works, talking back does not exist yet. Details in
[docs/tech.md](docs/tech.md).

## Is it safe?

The companion logs in with your user token, which makes it a self-bot, and
automating a user account is against Discord's Terms of Service. For this
use the risk profile looks small, since it behaves like a normal session
doing normal things at human pace, but nobody outside Discord can promise
anything. Use a throwaway account if that bothers you. The honest, longer
answer is in [docs/faq.md](docs/faq.md).

## Controls

| Button | Action |
|--------|--------|
| D-pad / sticks | move selection, scroll the focused column |
| Left/Right or L/R | move focus: channels, chat, members |
| Cross | open server / channel, join voice |
| Circle | back to the server list |
| Square | mute voice playback |
| Triangle | refresh messages and members |
| Start | write a message |
| Select | quit |

## More

- [docs/install.md](docs/install.md): full setup, the token guide,
  running from source, config files
- [docs/tech.md](docs/tech.md): how it works inside, the protocol,
  building and testing
- [docs/faq.md](docs/faq.md): the ban question at length, sizes, fonts,
  why a PC is needed
- [docs/about.md](docs/about.md): the story behind the project, credits

## License

MIT, see [LICENSE](LICENSE). Vendored cJSON keeps its own MIT notice.
