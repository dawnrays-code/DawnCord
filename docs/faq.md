# Questions people actually ask

## Is this safe? Can I get banned?

There is some risk, and it is only fair to spell it out.

The companion logs into Discord with your user token, which makes it a
self-bot. It works that way because a bot account cannot see your DMs and
servers the way you do. Automating a user account is against Discord's
Terms of Service, full stop.

For this particular use case the risk looks small. The companion behaves
like a normal Discord session: it reads what you would read, and sends
what you type at the pace you type it. There is no scraping and nothing
runs on a timer. Enforcement has historically gone after spam and abuse,
but nobody outside Discord can make promises about any of this, so treat
it as use at your own risk, and use a throwaway account if that bothers
you. It is meant for personal, educational use.

## Do I need a lot of free space?

No, the VPK is about 1 MB.

![hello i am tiny](tiny.png)

## Why do I need a PC at all?

Discord in 2026 needs TLS, a websocket gateway, JPEG and WebP decoding,
Opus, and end-to-end encryption for voice. The Vita has none of that and
276 MB of usable RAM on a good day. The thin-client split is what makes
the whole thing possible, and it is also why the client stays under a
megabyte. The PC does the modern web, the Vita does what it has always
been good at: drawing things fast on a lovely OLED.

## The text is too small / I want my own font

Drop any TTF at `ux0:data/dawncord/font.ttf` and restart the app. The
bundled one is Inter.

## My antivirus deleted the companion

Likely, and it is not detecting anything real. The .exe is a Python
program packed by PyInstaller into a single file, and that packing is
what gets flagged: to run, it unpacks a whole Python runtime into a temp
directory and executes from there, which is exactly what a dropper does,
so the file is judged by its shape rather than its contents. Every build
is also a brand new binary nobody has seen before, and reputation counts
for a lot. Malwarebytes deleted one such build within three seconds of
launch during development: no window, no warning, just a file that
stopped existing.

It is unpredictable rather than constant, so the same .exe can be fine
for you and not for the next person. Three ways out, easiest first:

1. Take `DawnCord-Companion-folder.zip` from the same release. Identical
   program, shipped as a folder instead of one self-extracting file, so
   the behaviour that gets flagged is not there at all. Unzip it
   anywhere and run the .exe inside.
2. Restore the file from your antivirus quarantine and add an exclusion
   for it.
3. Run it from source: no packing whatsoever, and
   [the install guide](install.md) has the commands. This is also how
   Linux and macOS run it, with `./start-companion.sh`.

If you would rather check than trust: releases are built by
[the CI workflow](../.github/workflows/build.yml) on GitHub's own
runners, from the commit each tag points at, and that file shows the
exact command used.

## Voice sounds off

The companion log records both ends of the voice link: what it sent,
and what the console reports back every ten seconds ("Vita link delta"
lines with received, dropped, reordered and concealed packet counts).
A bug report with those lines pasted in says most of what a debugging
session would need. Wi-Fi quality matters: the console tolerates
reordering and a little loss, but a struggling 2.4GHz network is the
first thing to rule out.
