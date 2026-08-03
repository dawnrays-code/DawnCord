# How DawnCord works

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

The Vita never talks to Discord directly. Everything a modern web stack
demands (TLS, the gateway, image decoding) stays on the PC, where the
libraries already exist and CPU is free. Voice works for the same reason
and could not really work any other way, since the encryption Discord now
requires lives on the companion side. The price of the arrangement is that
the companion has to be running whenever you use the client.

## The protocol

The protocol is small. Frames are `[2 bytes type][4 bytes length][JSON]`
over TCP port 9100. The first frame a client sends has to be `HANDSHAKE`
and it has to carry the pairing code: the companion generates one on
first run and refuses anything else, since the port is reachable by every
host on the network and a session can read your DMs. Requests after that
are fire-and-forget, and a single reader routes replies by message type,
since the server can push something at any moment. Snowflake IDs travel
as JSON strings, since a double-based parser like cJSON would quietly
round them off.

The handshake also carries a protocol version. When the console finds a
companion older than it expects it says so on screen, which beats
degrading in ways nobody can diagnose.

## Voice

Discord has been end-to-end encrypting voice since March 2026, through the
DAVE protocol (MLS group keys). discord.py-self negotiates the MLS group
but only ever uses it to encrypt what you send, so the companion holds the
same session and decrypts what other people say, packet by packet, with
[davey](https://github.com/Snazzah/davey). It then decodes the Opus frames
per speaker in RTP order, mixes everyone on a 20ms clock through a
limiter, and streams raw 48 kHz mono PCM over UDP to the Vita's port
9102. Each 20ms frame leaves as two sequence-numbered packets spaced 8ms
apart, because two packets sent together arrive in whichever order the
Wi-Fi retries decide, and that coin flip cost a quarter of the audio.

The console does no crypto and no codec work at all. It holds arriving
packets briefly so late ones can slot back into place, replaces genuinely
missing ones with faded repeats of the last audio rather than splicing
the gap shut, keeps a 120ms cushion, and plays samples. Concealing rather
than splicing is what lets the cushion recover: every discarded packet
used to shorten it permanently, and once it hit bottom the playback
gated the speech about ninety times a second, which is what a robot voice
is made of.

Both ends count what they see, and the console reports its own numbers
back over TCP every ten seconds, so the companion log shows delivery and
reordering separately. That distinction is the whole reason the voice
work converged: nothing was being lost, packets were arriving and being
thrown away.

Microphone capture is in progress. The console will send 16 kHz mono
grains up to port 9103 and the companion will resample, cancel the echo
against what it just sent, and hand Discord 20ms frames; the console
stays a dumb capture device the way it is a dumb playback one.

## Development

Every push goes through CI, which builds the VPK in the official VitaSDK
Docker image and the Windows companion with PyInstaller, then runs every
test suite. Releases also carry `DawnCord-debug-elf.zip`. If the app ever
crashes on you, `python tools/crash_symbols.py <psp2core dump> <debug ELF>`
turns the dump into plain function names you can paste into an issue.

Anything that is protocol logic rather than hardware lives in a module
with no console dependencies and a test that runs on a PC: message
parsing, the config file, voice packet sequencing, the microphone frame
builder. Those tests caught real bugs before hardware ever saw them, and
they are the reason a module like the frame builder can later be linked
into a taiHEN plugin unchanged.

Building the VPK locally needs [VitaSDK](https://vitasdk.org), on Linux or
macOS:

```
cd vita-client
cmake -S . -B build && cmake --build build   # -> build/DawnCord.vpk
```

Testing without a Vita:

```
python test-client/integration_test.py   # companion vs fake Discord, no token
python test-client/test_client.py <ip>   # interactive fake Vita, real companion
```

The console's pure modules, with plain gcc, from `vita-client/`. These
are the same four lines CI runs, so if you add a module add its line:

```
gcc -Wall -Wextra -Iinclude -Isrc/cjson \
    test/state_test.c src/state.c src/b64.c src/cjson/cJSON.c -o state_test && ./state_test
gcc -Wall -Wextra -Iinclude test/config_test.c src/config.c -o config_test && ./config_test
gcc -Wall -Wextra -Iinclude test/vseq_test.c src/voice_seq.c -o vseq_test && ./vseq_test
gcc -Wall -Wextra -Iinclude test/mic_frame_test.c src/mic_frame.c -o mic_frame_test && ./mic_frame_test
```
