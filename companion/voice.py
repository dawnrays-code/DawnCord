"""
Voice relay (listen-only, tentative first cut).

The companion joins a Discord voice channel as the user, receives everyone's
audio already decoded to PCM by discord-ext-voice-recv, downmixes it to mono
and streams it as raw 48 kHz signed-16 PCM over UDP to the Vita, which just
plays it. No Opus, no crypto, no framing reaches the console: all of that
stays on the PC, same philosophy as the rest of the app.

Unknowns that only real use will settle: whether a user account (not a bot)
actually receives others' audio through voice_recv, and how the raw stream
holds up on hardware. Kept entirely optional so text chat is never affected.
"""

import asyncio
import audioop
import logging
import socket

import discord
from discord.ext import voice_recv

log = logging.getLogger("dawncord.voice")

VOICE_UDP_PORT = 9102          # where the Vita listens for PCM
SAMPLE_RATE = 48000            # discord voice is always 48 kHz
FRAME_MS = 20                  # voice_recv delivers 20 ms frames
MONO_FRAME_BYTES = SAMPLE_RATE * 2 * FRAME_MS // 1000  # 1920 bytes
PACKET_BYTES = 960             # split frames so UDP stays under the MTU


class _MonoMixSink(voice_recv.AudioSink):
    """Collects per-speaker PCM frames; the relay's pump mixes and sends
    them on Discord's 20 ms clock.

    Decoding happens HERE, per packet and per speaker, with our own Opus
    decoders. The library's built-in decode path kills its whole packet
    router thread at the first packet it dislikes (hardware log: repeated
    "OpusError: corrupted stream" and then silence forever). Doing it
    ourselves means a bad packet costs one frame, not the stream."""

    def __init__(self):
        super().__init__()
        self.frames: dict[int, bytes] = {}   # source -> latest 20ms mono frame
        self._decoders: dict[int, discord.opus.Decoder] = {}
        self.rx = 0        # opus packets seen
        self.ok = 0        # decoded fine
        self.bad = 0       # dropped (decode error)

    def wants_opus(self) -> bool:
        return True   # raw Opus: we decode, tolerantly

    def write(self, user, data: voice_recv.VoiceData):
        pkt = data.opus
        if not pkt:
            return
        src = getattr(data, "source", None) or id(user)
        self.rx += 1
        try:
            dec = self._decoders.get(src)
            if dec is None:
                dec = self._decoders[src] = discord.opus.Decoder()
            pcm = dec.decode(pkt, fec=False)   # 48 kHz stereo s16
        except Exception:
            self.bad += 1
            if self.bad in (1, 10, 100) or self.bad % 1000 == 0:
                log.warning("Opus decode drops so far: %d of %d packets",
                            self.bad, self.rx)
            return
        self.ok += 1
        # 48 kHz stereo s16 -> mono s16 (average the two channels)
        self.frames[src] = audioop.tomono(pcm, 2, 0.5, 0.5)

    def cleanup(self):
        self.frames.clear()
        self._decoders.clear()


class VoiceRelay:
    def __init__(self):
        self._vc: voice_recv.VoiceRecvClient | None = None
        self._sink: _MonoMixSink | None = None
        self._pump: asyncio.Task | None = None
        self._sock: socket.socket | None = None
        self._target: tuple[str, int] | None = None

    @property
    def active(self) -> bool:
        return self._vc is not None

    async def join(self, channel: discord.VoiceChannel, vita_ip: str):
        await self.leave()   # one channel at a time

        if not discord.opus.is_loaded():
            discord.opus._load_default()   # bundled libopus, needed to decode

        self._vc = await channel.connect(cls=voice_recv.VoiceRecvClient,
                                         self_mute=True, self_deaf=False)
        self._sink = _MonoMixSink()
        self._vc.listen(self._sink)

        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._target = (vita_ip, VOICE_UDP_PORT)
        self._pump = asyncio.create_task(self._run())
        log.info("Joined voice %s, streaming to %s:%d",
                 channel.name, vita_ip, VOICE_UDP_PORT)

    async def _run(self):
        silence = b"\x00" * MONO_FRAME_BYTES
        try:
            while True:
                await asyncio.sleep(FRAME_MS / 1000)
                if not self._sink:
                    continue
                frames = list(self._sink.frames.values())
                self._sink.frames.clear()
                if not frames:
                    continue
                # Speakers can deliver different frame sizes (packet loss,
                # varying Opus frame durations): pad everyone to the full
                # 20 ms before mixing, or audioop.add refuses the sum.
                frames = [(f + silence)[:MONO_FRAME_BYTES] for f in frames]
                mixed = frames[0]
                for f in frames[1:]:
                    mixed = audioop.add(mixed, f, 2)   # sum with clipping
                for off in range(0, len(mixed), PACKET_BYTES):
                    self._sock.sendto(mixed[off:off + PACKET_BYTES], self._target)
        except asyncio.CancelledError:
            pass
        except Exception:
            log.exception("Voice pump stopped")

    async def leave(self):
        if self._sink and self._sink.rx:
            log.info("Voice session stats: %d opus packets, %d decoded, "
                     "%d dropped", self._sink.rx, self._sink.ok,
                     self._sink.bad)
        if self._pump:
            self._pump.cancel()
            self._pump = None
        if self._vc:
            try:
                await self._vc.disconnect()
            except Exception:
                log.warning("Voice disconnect was not clean", exc_info=True)
            self._vc = None
        if self._sock:
            self._sock.close()
            self._sock = None
        self._sink = None
