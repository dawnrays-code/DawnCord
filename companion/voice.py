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

import array
import asyncio
import audioop
import logging
import socket
import struct
import sys
import time

# Windows timers tick at 15.6ms by default, so the pump's 4ms sleep was
# really a 15.6ms one and audio left in small bursts. A 1ms tick makes
# the pacing real; process-global, harmless for a dedicated companion.
if sys.platform == "win32":
    try:
        import ctypes
        ctypes.windll.winmm.timeBeginPeriod(1)
    except Exception:
        pass

import davey            # Discord's DAVE/MLS crypto (Rust binding, via PyPI)
import discord
from discord.ext import voice_recv

log = logging.getLogger("dawncord.voice")

# Discord sends an RTCP sender report about once a second and voice_recv
# logs every one of them at INFO because it has no handler for them. They
# are reports, not errors, and they drown the log.
logging.getLogger("discord.ext.voice_recv.reader").setLevel(logging.WARNING)
# Same story for the voice websocket: it reports every payload carrying a
# field it does not know ("WS payload has extra keys: {'seq': N}"), which
# Discord now includes on every message.
logging.getLogger("discord.ext.voice_recv.gateway").setLevel(logging.WARNING)

# Discord voice sits around -20 dBFS and the Vita's speakers are small, so
# the console ends up quiet even at full volume. A fixed +6 dB lands well
# clear of clipping for speech; override with "voice_gain" in config.json.
VOICE_GAIN = 2.0

VOICE_UDP_PORT = 9102          # where the Vita listens for PCM
SAMPLE_RATE = 48000            # discord voice is always 48 kHz
FRAME_MS = 20                  # voice_recv delivers 20 ms frames
MONO_FRAME_BYTES = SAMPLE_RATE * 2 * FRAME_MS // 1000  # 1920 bytes
PACKET_BYTES = 960             # PCM per packet (480 samples, 10 ms)
# Each UDP packet is [4-byte big-endian sequence][PCM]. UDP can reorder or
# lose packets in transit, which the console played as scrambled audio
# ("1-3-2-4" on the hardware test); the sequence lets the Vita drop stale
# ones and keep the stream in order.
SEQ_HEADER = struct.Struct("!I")
SAMPLES_PER_FRAME = MONO_FRAME_BYTES // 2   # 960


def _configured_gain() -> float:
    """Playback gain, overridable without touching the code."""
    try:
        import json
        from paths import BASE_DIR
        cfg = json.loads((BASE_DIR / "config.json").read_text())
        g = float(cfg.get("voice_gain", VOICE_GAIN))
        return max(0.5, min(g, 6.0))
    except Exception:
        return VOICE_GAIN


def _configured_spacing() -> float:
    """Milliseconds between the two 10ms packets of a frame. They used to
    leave back-to-back, microseconds apart, and with ANY independent
    per-packet jitter that makes their arrival order a coin flip; the
    console keeps only strictly-newer sequences, so half the swapped
    pairs, a quarter of all packets, got discarded despite arriving.
    Spacing the pair denies the network the chance to invert it.
    "voice_pace_ms": 0 in config.json restores the old behaviour."""
    try:
        import json
        from paths import BASE_DIR
        cfg = json.loads((BASE_DIR / "config.json").read_text())
        return max(0.0, min(float(cfg.get("voice_pace_ms", 8.0)), 15.0))
    except Exception:
        return 8.0


class _MonoMixSink(voice_recv.AudioSink):
    """Collects per-speaker PCM frames; the relay's pump mixes and sends
    them on Discord's 20 ms clock.

    Two things happen here that the library does not do for us.

    1. DAVE DECRYPTION. Since March 2026 Discord end-to-end encrypts voice
       (the DAVE protocol, MLS group keys). discord.py-self negotiates the
       MLS group and holds the session, but only ever uses it to ENCRYPT
       what we send: nothing in the stack decrypts what others say, so the
       payload reaching a receiver is ciphertext. Feeding that to an Opus
       decoder is what produced the "corrupted stream" errors and the
       garbled 1-bit-walkie-talkie audio on hardware. We hold the same
       session, so we decrypt here, per packet and per speaker.

    2. OPUS DECODING, tolerantly. The library's own decode path kills its
       whole packet-router thread at the first packet it dislikes; doing it
       ourselves means a bad packet costs one frame, not the stream."""

    # Discord's "I stopped talking" marker: never encrypted, never decoded.
    SILENCE_FRAME = b"\xf8\xff\xfe"

    def __init__(self):
        super().__init__()
        # Per-speaker accumulation buffers. The first cut kept only the
        # LATEST 20ms frame per speaker and a jittery 20ms asyncio timer
        # threw the rest away: a third of the samples never reached the
        # Vita, which is exactly the garbled walkie-talkie the hardware
        # test reported. Nothing gets dropped anymore.
        self.bufs: dict[int, bytearray] = {}
        self._decoders: dict[int, discord.opus.Decoder] = {}
        self.talkers: dict[str, float] = {}   # display name -> last audio at
        self._seq: dict[object, int] = {}     # speaker -> last RTP sequence
        self.rx = 0        # opus packets seen
        self.ok = 0        # decoded fine
        self.bad = 0       # dropped (decode error)
        self.undec = 0     # dropped (could not decrypt)
        self.late = 0      # arrived after we had moved on
        self.lost = 0      # never arrived, concealed by the decoder
        self.healed = 0    # undecryptable, concealed instead of dropped
        self.unmapped = 0  # speaker not yet identified by the library

    def talkers_since(self, cutoff: float) -> list[str]:
        """Names that produced audio more recently than cutoff. Snapshot:
        write() inserts from the router thread while the pump reads."""
        return [n for n, t in list(self.talkers.items()) if t >= cutoff]

    def _decrypt(self, user, pkt: bytes) -> bytes | None:
        """Ciphertext -> Opus. None means 'drop this packet' (the caller
        conceals the gap). Mirrors the library's own can_encrypt test:
        the channel is plaintext only when dave_protocol_version is 0 or
        no session exists. A session that exists but is not ready yet
        means E2EE is NEGOTIATING: those packets are ciphertext, and
        passing them through decodes into full-scale garbage, which is
        what the full-scale blast at every join was."""
        try:
            vc = self.voice_client
        except AttributeError:
            return pkt             # sink not attached yet
        conn = getattr(vc, "_connection", None) if vc else None
        version = getattr(conn, "dave_protocol_version", 0) if conn else 0
        sess = getattr(conn, "dave_session", None) if conn else None
        if not version or sess is None:
            return pkt              # genuinely plaintext (v0) channel
        if not getattr(sess, "ready", False):
            self.undec += 1
            return None             # negotiating: ciphertext, no key yet

        uid = getattr(user, "id", None)
        try:
            return sess.decrypt(uid, davey.MediaType.audio, pkt)
        except Exception:
            # During a protocol transition Discord briefly sends cleartext
            # and flags the sender as passthrough-allowed.
            try:
                if sess.can_passthrough(uid):
                    return pkt
            except Exception:
                pass
            self.undec += 1
            if self.undec in (1, 10, 100) or self.undec % 1000 == 0:
                log.warning("DAVE decrypt drops so far: %d of %d packets",
                            self.undec, self.rx)
            return None

    def wants_opus(self) -> bool:
        return True   # raw Opus: we decode, tolerantly

    def write(self, user, data: voice_recv.VoiceData):
        pkt = data.opus
        if not pkt:
            return

        # RFC 3550 padding. voice_recv leaves it attached (rtp.py has a
        # TODO for it) and davey then fails the whole decrypt: this was
        # discord.js's DAVE bug too (PR #11449). Strip it first, so a
        # padded silence frame is still recognised as silence below.
        rtppkt = getattr(data, "packet", None)
        if getattr(rtppkt, "padding", False) and len(pkt) > 1:
            n = pkt[-1]
            if 0 < n < len(pkt):
                pkt = pkt[:-n]

        # A speaker voice_recv has not mapped to a user yet cannot be
        # decrypted (no key id), and lumping those packets under one
        # shared decoder corrupts it for everyone. Skip; the mapping
        # arrives within a moment via the speaking event.
        if user is None:
            self.unmapped += 1
            return

        src = getattr(data, "source", None) or id(user)
        seq = getattr(rtppkt, "sequence", None)
        is_silence = pkt == self.SILENCE_FRAME

        # RTP order, tracked for EVERY frame including silence markers.
        # Silence frames consume sequence numbers on the wire; skipping
        # them untracked made every utterance resume look like a 5-packet
        # loss, and the concealment for it manufactured quiet holes out
        # of perfectly normal pauses.
        conceal = 0
        if seq is not None:
            prev = self._seq.get(src)
            if prev is not None:
                delta = (seq - prev) & 0xFFFF
                if delta == 0 or delta > 0x8000:
                    if not is_silence:
                        self.late += 1
                    return                      # duplicate or too late
                if not is_silence:
                    conceal = min(delta - 1, 4)  # longer gaps: restart
            self._seq[src] = seq
        if is_silence:
            return
        self.rx += 1

        dec = self._decoders.get(src)
        if dec is None:
            dec = self._decoders[src] = discord.opus.Decoder()

        pkt = self._decrypt(user, pkt)
        if not pkt:
            # Undecryptable (counted in _decrypt). A bare return here left
            # a 20ms hole with hard edges, one crackle per drop; asking
            # the decoder to conceal the frame keeps the waveform
            # continuous instead.
            try:
                self.healed += 1
                buf = self.bufs.setdefault(src, bytearray())
                buf.extend(audioop.tomono(dec.decode(None, fec=False),
                                          2, 0.5, 0.5))
            except Exception:
                pass
            return

        try:
            buf = self.bufs.setdefault(src, bytearray())
            for _ in range(conceal):
                self.lost += 1
                buf.extend(audioop.tomono(dec.decode(None, fec=False),
                                          2, 0.5, 0.5))
            pcm = dec.decode(pkt, fec=False)   # 48 kHz stereo s16
        except Exception:
            self.bad += 1
            if self.bad in (1, 10, 100) or self.bad % 1000 == 0:
                log.warning("Opus decode drops so far: %d of %d packets",
                            self.bad, self.rx)
            return
        self.ok += 1
        name = getattr(user, "display_name", None) or getattr(user, "name", None)
        if name:
            self.talkers[name] = time.monotonic()
        # 48 kHz stereo s16 -> mono s16 (average the two channels)
        buf.extend(audioop.tomono(pcm, 2, 0.5, 0.5))
        # Latency guard: if a buffer grows past ~400ms (pump stalled),
        # keep only the freshest 200ms.
        if len(buf) > MONO_FRAME_BYTES * 20:
            del buf[:-MONO_FRAME_BYTES * 10]

    def cleanup(self):
        self.bufs.clear()
        self._decoders.clear()
        self.talkers.clear()
        self._seq.clear()


class VoiceRelay:
    def __init__(self):
        self._vc: voice_recv.VoiceRecvClient | None = None
        self._sink: _MonoMixSink | None = None
        self._pump: asyncio.Task | None = None
        self._sock: socket.socket | None = None
        self._target: tuple[str, int] | None = None
        self._broadcast = None    # async fn(list[str]) -> pushes who is talking
        self.sent = 0             # datagrams actually sent this session

    def set_broadcaster(self, fn):
        self._broadcast = fn

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
        # Clocked, and the clock is what makes the mix a MIX. The old
        # data-driven loop emitted a frame whenever ANY speaker had one
        # complete, so two concurrent streams whose packets straddled
        # different wakeups left as ALTERNATING solo frames instead of
        # being summed: the quiet speaker's comfort-noise frames were
        # punched whole into the talker's words (the exactly-20ms holes
        # measured on hardware) and the timeline stretched (29.4s
        # of tap out of a 25s call). Now a frame leaves only on a 20ms
        # tick, carrying the sum of every speaker with a full frame at
        # that instant; whoever misses a tick is summed on the next one,
        # never emitted alone.
        seq = 0
        speaking = frozenset()
        next_speaking_check = 0.0
        gain = _configured_gain()
        # Limiter state. A fixed gain saturates: with a loud speaker the
        # x2.0 hard-clipped 14% of speech frames, and hard clipping IS the
        # dirty-highs crackle. The gain now yields per frame: any frame
        # that would cross LIMIT gets exactly the gain that keeps its peak
        # at LIMIT (per-frame attack, no lookahead needed), and recovery
        # back toward the configured gain is slow so it does not pump.
        LIMIT = 30000.0
        live_gain = gain
        next_emit = None            # monotonic deadline, None while idle
        draining = False            # catching up on a backlog
        spacing = _configured_spacing() / 1000.0
        pending = []                # (due_at, datagram), strictly in order
        next_due = 0.0              # keeps queued sends monotonic in seq
        try:
            while True:
                await asyncio.sleep(0.004)
                if not self._sink:
                    continue

                now = time.monotonic()
                # Paced sends whose moment has come, strictly in queue
                # order so sequence numbers never leave inverted.
                while pending and pending[0][0] <= now:
                    self._sock.sendto(pending.pop(0)[1], self._target)
                    self.sent += 1

                # Snapshot: write() may insert a new speaker from the
                # router thread mid-iteration, and a dict-size change here
                # would kill the pump for good.
                bufs = list(self._sink.bufs.values())
                ready = [b for b in bufs if len(b) >= MONO_FRAME_BYTES]

                if not ready:
                    # An empty instant is NOT the end of speech: packets
                    # run a few ms late all the time, and resetting the
                    # clock here re-anchored it on whichever speaker
                    # arrived next, which is the interleave again. Hold
                    # the deadline; only a real pause re-anchors, and the
                    # limiter recovers during it.
                    if next_emit is not None and now - next_emit > 0.25:
                        next_emit = None
                    if next_emit is None:
                        live_gain = min(gain, live_gain + 0.005)
                elif next_emit is None:
                    next_emit = now
                elif now - next_emit > 0.25:
                    next_emit = now             # stalled: drop the debt

                # Up to three frames per wakeup: enough to drain a backlog
                # without bursting the console.
                frames = 0
                while next_emit is not None and now >= next_emit and frames < 3:
                    chunks = []
                    for buf in bufs:
                        if len(buf) < MONO_FRAME_BYTES:
                            continue
                        chunks.append(bytes(buf[:MONO_FRAME_BYTES]))
                        del buf[:MONO_FRAME_BYTES]
                    if not chunks:
                        break       # data a hair late: hold the deadline,
                                    # the next wake sends it on arrival
                    if len(chunks) == 1:
                        mixed = chunks[0]
                    else:
                        # Two voices summed can exceed 16 bits, and
                        # audioop.add saturates BEFORE the limiter ever
                        # sees the frame: that was the distortion heard
                        # exactly when two people talked at once.
                        # Accumulate wide instead, and let the limiter
                        # below scale the true peak back into range.
                        acc = array.array("i", array.array("h", chunks[0]))
                        for c in chunks[1:]:
                            a = array.array("h", c)
                            for i in range(len(acc)):
                                acc[i] += a[i]
                        wide_peak = max(abs(v) for v in acc) or 1
                        f = min(1.0, 32000.0 / wide_peak)
                        mixed = array.array(
                            "h", [int(v * f) for v in acc]).tobytes()
                    peak = audioop.max(mixed, 2) or 1
                    safe = min(gain, LIMIT / peak)
                    live_gain = safe if safe < live_gain \
                        else min(gain, live_gain + 0.02)   # ~1s release
                    if abs(live_gain - 1.0) > 0.001:
                        mixed = audioop.mul(mixed, 2, live_gain)
                    next_due = max(next_due, now)
                    for off in range(0, len(mixed), PACKET_BYTES):
                        dgram = SEQ_HEADER.pack(seq) + \
                            mixed[off:off + PACKET_BYTES]
                        seq = (seq + 1) & 0xFFFFFFFF
                        if spacing <= 0 or (not pending and next_due <= now):
                            self._sock.sendto(dgram, self._target)
                            self.sent += 1
                        else:
                            pending.append((next_due, dgram))
                        next_due += spacing
                    frames += 1
                    # Backlog drain with hysteresis: start catching up past
                    # 120ms queued, keep going until back under 40ms, then
                    # return to the 20ms clock. A single threshold here
                    # flip-flopped around its own edge, and the cadence at
                    # the console became burst-gap-burst.
                    if draining:
                        draining = any(len(b) >= MONO_FRAME_BYTES * 2
                                       for b in bufs)
                    else:
                        draining = any(len(b) >= MONO_FRAME_BYTES * 6
                                       for b in bufs)
                    if draining:
                        next_emit = now
                    else:
                        next_emit += FRAME_MS / 1000.0

                # Who is talking right now, pushed to the console only when
                # the set changes, so the dots light up without a poll.
                now = time.monotonic()
                if self._broadcast and now >= next_speaking_check:
                    next_speaking_check = now + 0.15
                    live = frozenset(self._sink.talkers_since(now - 0.35))
                    if live != speaking:
                        speaking = live
                        await self._broadcast(sorted(live))
        except asyncio.CancelledError:
            pass
        except Exception:
            log.exception("Voice pump stopped")

    async def leave(self):
        if self._sink and self._sink.rx:
            log.info("Voice session stats: %d packets, %d played, "
                     "%d undecryptable (%d healed), %d undecodable, "
                     "%d late, %d concealed, %d unmapped; %d datagrams "
                     "sent to the console", self._sink.rx, self._sink.ok,
                     self._sink.undec, self._sink.healed, self._sink.bad,
                     self._sink.late, self._sink.lost, self._sink.unmapped,
                     self.sent)
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
