"""
TCP server that accepts connections from the Vita client
and bridges them to Discord via the companion's Discord client.

Framing note:
  The client sends fire-and-forget requests. All server output is typed
  (GUILD_LIST, MESSAGE_LIST, MESSAGE_NEW, ...). The client is expected to
  route replies by message type from a single read loop, NOT to assume the
  next frame is the answer to its last request. This keeps request/response
  and server-push on one socket without desync.
"""

import asyncio
import base64
import json
import logging
import os
import secrets
import time

from paths import BASE_DIR
from protocol import (MAX_PAYLOAD, PROTOCOL_VERSION, MsgType, encode,
                      read_message)

log = logging.getLogger("dawncord.server")


def _load_pair_code() -> str:
    """
    The pairing code a client must present in its HANDSHAKE before the
    server will serve anything. The port is reachable by any host on the
    LAN and a valid session can read your DMs and send as you, so this is
    never optional and never None: if no code exists yet, one is generated
    and saved. Running the companion unpaired used to be possible by
    starting it from main.py rather than the GUI, which left the account
    open to anything on the network.

    Sources, in order: env DAWNCORD_PAIR_CODE, then "pair_code" in
    config.json next to the companion (gitignored), then a fresh one.
    """
    env = os.environ.get("DAWNCORD_PAIR_CODE", "").strip()
    if env:
        return env

    path = BASE_DIR / "config.json"
    existed = path.exists()
    readable = True
    try:
        cfg = json.loads(path.read_text())
        if not isinstance(cfg, dict):
            cfg, readable = {}, False
    except FileNotFoundError:
        cfg = {}
    except (OSError, json.JSONDecodeError):
        cfg, readable = {}, False

    raw = cfg.get("pair_code")
    code = raw.strip() if isinstance(raw, str) else ""
    if code:
        return code

    code = f"{secrets.randbelow(1_000_000):06d}"
    if existed and not readable:
        # The file is there but unreadable or not JSON. Rewriting it would
        # silently discard whatever else the user put in it (voice_gain,
        # voice_pace_ms), so leave it alone and say so.
        log.warning("%s could not be read, so it was left untouched and a "
                    "temporary pairing code was generated: %s. Fix or "
                    "delete the file to keep a stable code.", path, code)
        return code

    cfg["pair_code"] = code
    try:
        path.write_text(json.dumps(cfg, indent=2))
    except OSError:
        log.warning("Could not save the pairing code to %s: it will be a "
                    "different one every start until that is fixed.", path)
    return code


PAIR_CODE = _load_pair_code()

# Wrong pairing codes tolerated from one address before it is refused for
# the rest of the session.
MAX_BAD_CODES = 10

# LAN auto-discovery: the Vita broadcasts this magic on UDP DISCOVERY_PORT
# and we answer with where the TCP server lives. First-boot setup on the
# console uses it so nobody has to type IP addresses.
DISCOVERY_PORT = 9101
DISCOVERY_MAGIC = b"DAWNCORD_DISCOVER"


class _DiscoveryResponder(asyncio.DatagramProtocol):
    def __init__(self, tcp_port: int):
        self.tcp_port = tcp_port
        self.transport: asyncio.DatagramTransport | None = None

    def connection_made(self, transport):
        self.transport = transport

    def datagram_received(self, data: bytes, addr):
        if not data.startswith(DISCOVERY_MAGIC) or self.transport is None:
            return
        reply = json.dumps({
            "dawncord": True,
            "port": self.tcp_port,
            "needs_code": bool(PAIR_CODE),
        }).encode()
        self.transport.sendto(reply, addr)
        log.info("Discovery probe from %s, replied", addr[0])


class VitaServer:
    _vita_stats = (None, 0, 0.0)   # last console report, sent count, time

    def __init__(self, discord_bridge, host: str = "0.0.0.0", port: int = 9100):
        self._bad_codes: dict[str, int] = {}   # peer address -> wrong guesses
        self.discord_bridge = discord_bridge
        self.host = host
        self.port = port
        self._clients: set[asyncio.StreamWriter] = set()
        self._server: asyncio.AbstractServer | None = None

    async def start(self):
        self._server = await asyncio.start_server(
            self._handle_client, self.host, self.port
        )
        addr = self._server.sockets[0].getsockname()
        log.info("Vita server listening on %s:%s", addr[0], addr[1])
        # The console asks for this once and remembers it. The windowed
        # companion shows it in big type; from a terminal the log is the
        # only place it can appear, and without it the user has no way to
        # know the code that was just generated for them.
        log.info("Pairing code: %s   (the Vita asks for it on first run)",
                 PAIR_CODE)

        # Discovery responder (best-effort: the TCP server works without it).
        try:
            loop = asyncio.get_running_loop()
            await loop.create_datagram_endpoint(
                lambda: _DiscoveryResponder(self.port),
                local_addr=("0.0.0.0", DISCOVERY_PORT),
            )
            log.info("Discovery responder on UDP %s", DISCOVERY_PORT)
        except OSError as e:
            log.warning("Discovery responder unavailable (%s); the Vita "
                        "will need the IP typed in at first boot.", e)
        # start_server already accepts connections in the background; just keep
        # a reference so it isn't garbage-collected. No serve_forever() needed.

    async def broadcast(self, msg_type: MsgType, payload: dict):
        data = encode(msg_type, payload)
        for writer in list(self._clients):
            try:
                writer.write(data)
                await writer.drain()
            except (ConnectionError, OSError):
                self._drop(writer)

    def _drop(self, writer: asyncio.StreamWriter):
        self._clients.discard(writer)  # discard: safe if already gone

    async def _handle_client(self, reader: asyncio.StreamReader, writer: asyncio.StreamWriter):
        addr = writer.get_extra_info("peername")
        log.info("Vita connected from %s", addr)

        try:
            if not await self._do_handshake(reader, writer, addr):
                return
            self._clients.add(writer)

            while True:
                msg = await read_message(reader)
                if msg is None:
                    break
                await self._dispatch(msg[0], msg[1], writer)
        except (asyncio.IncompleteReadError, ConnectionError):
            log.info("Vita disconnected from %s", addr)
        except asyncio.CancelledError:
            raise                       # shutdown, not a client fault
        except Exception:
            # Everything here is driven by bytes from the network, so a
            # malformed frame must cost one connection and nothing more:
            # an oversized length, invalid JSON or a payload of the wrong
            # shape used to escape as a traceback from the task.
            log.warning("Dropping %s after a bad frame", addr, exc_info=True)
        finally:
            self._drop(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def _do_handshake(self, reader, writer, addr) -> bool:
        """First frame must be HANDSHAKE, carrying the pairing code."""
        host = addr[0] if addr else "?"
        if self._bad_codes.get(host, 0) >= MAX_BAD_CODES:
            # Ten wrong guesses and this address is done for the session.
            # Without it, concurrent connections defeat the delay below
            # and the six-digit space falls in about an hour on a LAN.
            log.warning("Refusing %s: too many bad pairing codes", host)
            return False
        try:
            first = await asyncio.wait_for(read_message(reader), timeout=10)
        except asyncio.TimeoutError:
            log.warning("Handshake timeout from %s", addr)
            return False
        if first is None:
            return False

        msg_type, payload = first
        if msg_type != MsgType.HANDSHAKE:
            writer.write(encode(MsgType.ERROR, {"error": "expected handshake"}))
            await writer.drain()
            return False

        # A handshake body that is not an object (a bare array or string is
        # perfectly valid JSON) must be a rejection, not an AttributeError.
        code = payload.get("code") if isinstance(payload, dict) else None
        # Compared as bytes: compare_digest raises TypeError on non-ASCII
        # str, so an accented pairing code used to break every handshake,
        # including the correct one.
        if not secrets.compare_digest(str(code or "").encode("utf-8"),
                                      PAIR_CODE.encode("utf-8")):
            log.warning("Bad pairing code from %s", addr)
            host = addr[0] if addr else "?"
            self._bad_codes[host] = self._bad_codes.get(host, 0) + 1
            # The delay is per connection and asyncio runs them in
            # parallel, so a sleep alone does not rate-limit anything: a
            # measured 300 concurrent sockets got 140 guesses a second.
            # What actually bounds it is the lockout below.
            await asyncio.sleep(1)
            writer.write(encode(MsgType.ERROR, {"error": "invalid pairing code"}))
            await writer.drain()
            return False
        self._bad_codes.pop(addr[0] if addr else "?", None)

        writer.write(encode(MsgType.HANDSHAKE_ACK,
                            {"status": "ok", "version": PROTOCOL_VERSION}))
        await writer.drain()
        return True

    async def _dispatch(self, msg_type: MsgType, payload: dict, writer: asyncio.StreamWriter):
        # Data requests need Discord to be logged in and caches populated.
        if msg_type in (
            MsgType.REQUEST_GUILDS,
            MsgType.REQUEST_CHANNELS,
            MsgType.REQUEST_MESSAGES,
            MsgType.REQUEST_MEMBERS,
            MsgType.SEND_MESSAGE,
            MsgType.JOIN_VOICE,
        ):
            await self.discord_bridge.wait_until_ready()

        try:
            if msg_type == MsgType.REQUEST_GUILDS:
                guilds = self.discord_bridge.get_guilds()
                # "me" feeds the profile box on the Vita (bridge is ready
                # here, so the logged-in user is known).
                me = getattr(self.discord_bridge, "get_me", lambda: None)()
                writer.write(encode(MsgType.GUILD_LIST,
                                    {"guilds": guilds, "me": me}))

            elif msg_type == MsgType.REQUEST_CHANNELS:
                # IDs arrive as strings (snowflakes don't fit a double, see
                # discord_bridge) — int() accepts both forms. Echo as string.
                guild_id = int(payload["guild_id"])
                channels = self.discord_bridge.get_channels(guild_id)
                writer.write(encode(MsgType.CHANNEL_LIST,
                                    {"guild_id": str(guild_id), "channels": channels}))

            elif msg_type == MsgType.REQUEST_MESSAGES:
                channel_id = int(payload["channel_id"])
                limit = int(payload.get("limit", 50))
                # Optional scroll-back paging: "before" asks for the chunk
                # of history older than that message id. Echoed back so the
                # client can tell a chunk from a fresh newest-window list.
                before = payload.get("before")
                messages = await self.discord_bridge.get_messages(
                    channel_id, limit, int(before) if before else None)
                reply = {"channel_id": str(channel_id), "messages": messages}
                if before:
                    reply["before"] = str(before)
                # A frame over MAX_PAYLOAD is refused by the console and
                # takes the connection down with it, and a channel of long
                # messages can reach that: non-ASCII escapes to six bytes
                # per character in JSON. Drop from the OLD end until the
                # batch fits, so the newest messages always survive.
                frame = encode(MsgType.MESSAGE_LIST, reply)
                while len(frame) > MAX_PAYLOAD and len(reply["messages"]) > 1:
                    reply["messages"] = reply["messages"][1:]
                    frame = encode(MsgType.MESSAGE_LIST, reply)
                if len(messages) != len(reply["messages"]):
                    # The client infers "no more history" from a short
                    # chunk, so a trimmed reply must say it was trimmed or
                    # scroll-back stops dead in any channel of long or
                    # non-Latin messages.
                    reply["trimmed"] = True
                    log.info("History for %s trimmed to %d messages to fit "
                             "one frame", channel_id, len(reply["messages"]))
                writer.write(frame)

            elif msg_type == MsgType.SET_CHANNEL:
                channel_id = int(payload["channel_id"])
                self.discord_bridge.set_active_channel(channel_id)
                log.info("Active channel set to %s", channel_id)

            elif msg_type == MsgType.SEND_MESSAGE:
                channel_id = int(payload["channel_id"])
                content = payload["content"]
                success = await self.discord_bridge.send_message(channel_id, content)
                writer.write(encode(MsgType.MESSAGE_SENT_ACK, {"success": success}))

            elif msg_type == MsgType.REQUEST_MEMBERS:
                channel_id = int(payload["channel_id"])
                members = self.discord_bridge.get_members(channel_id)
                writer.write(encode(MsgType.MEMBER_LIST,
                                    {"channel_id": str(channel_id), "members": members}))

            elif msg_type == MsgType.JOIN_VOICE:
                channel_id = int(payload["channel_id"])
                vita_ip = writer.get_extra_info("peername")[0]
                ok = await self.discord_bridge.join_voice(channel_id, vita_ip)
                writer.write(encode(MsgType.VOICE_STATE,
                                    {"active": ok,
                                     "channel_id": str(channel_id) if ok else "0"}))

            elif msg_type == MsgType.LEAVE_VOICE:
                await self.discord_bridge.leave_voice()
                writer.write(encode(MsgType.VOICE_STATE,
                                    {"active": False, "channel_id": "0"}))

            elif msg_type == MsgType.VOICE_RX_STATS:
                # The console's counters are cumulative and survive
                # sessions, so what carries meaning is the DELTA between
                # consecutive reports, read against how many datagrams we
                # sent in the same window: rx/sent measures delivery,
                # drop/rx measures reordering, independently.
                now = time.monotonic()
                cur = (payload.get("rx", 0), payload.get("drop", 0),
                       payload.get("under", 0), payload.get("resync", 0),
                       payload.get("trim", 0), payload.get("reorder", 0),
                       payload.get("conceal", 0))
                relay = getattr(self.discord_bridge, "_voice", None)
                sent = getattr(relay, "sent", 0) if relay else 0
                prev, prev_sent, prev_t = self._vita_stats
                if prev is not None and len(prev) == len(cur) and \
                        all(c >= p for c, p in zip(cur, prev)):
                    d = [c - p for c, p in zip(cur, prev)]
                    log.info("Vita link delta: +%d rx (+%d sent), +%d drop, "
                             "+%d underruns, +%d resyncs, +%d trims, "
                             "+%d reordered, +%d concealed in %.1fs",
                             d[0], sent - prev_sent, d[1], d[2], d[3], d[4],
                             d[5], d[6], now - prev_t)
                else:
                    log.info("Vita link stats (session): rx=%d drop=%d "
                             "under=%d resync=%d trim=%d reorder=%d "
                             "conceal=%d", *cur)
                self._vita_stats = (cur, sent, now)

            elif msg_type == MsgType.REQUEST_IMAGE:
                url = str(payload["url"])
                key = str(payload.get("key", url))
                size = int(payload.get("size", 64))
                jpeg = await self.discord_bridge.get_image(url, size)
                writer.write(encode(MsgType.IMAGE_DATA, {
                    "key": key,
                    "data": base64.b64encode(jpeg).decode("ascii") if jpeg else None,
                }))

            else:
                writer.write(encode(MsgType.ERROR,
                                    {"error": f"unhandled type: {msg_type}"}))
        except (KeyError, ValueError, TypeError) as e:
            writer.write(encode(MsgType.ERROR, {"error": f"bad request: {e}"}))
        except Exception as e:
            # Discord can refuse anything (403 on restricted channels, rate
            # limits...). That must never kill the companion: report and go on.
            log.exception("Request %s failed", msg_type)
            writer.write(encode(MsgType.ERROR, {"error": f"companion error: {e}"}))

        await writer.drain()
