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

from paths import BASE_DIR
from protocol import PROTOCOL_VERSION, MsgType, encode, read_message

log = logging.getLogger("dawncord.server")


def _load_pair_code() -> str | None:
    """
    Optional pairing code: a client must present it in its HANDSHAKE before
    the server will serve anything. The port is reachable by any host on the
    LAN, and a valid session can read your DMs and send as you, so setting
    one is strongly recommended for anything but a trusted, isolated network.

    Sources, in order: env DAWNCORD_PAIR_CODE, then "pair_code" in
    companion/config.json (gitignored), e.g. {"pair_code": "1234"}.
    """
    code = os.environ.get("DAWNCORD_PAIR_CODE")
    if code:
        return code
    try:
        cfg = json.loads((BASE_DIR / "config.json").read_text())
    except (OSError, json.JSONDecodeError):
        return None
    code = cfg.get("pair_code")
    return str(code) if code else None


PAIR_CODE = _load_pair_code()

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
    def __init__(self, discord_bridge, host: str = "0.0.0.0", port: int = 9100):
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
        if not PAIR_CODE:
            log.warning(
                "No pairing code set — any host on the LAN that connects "
                "can use your Discord session. Set DAWNCORD_PAIR_CODE or "
                'add {"pair_code": "..."} to companion/config.json.'
            )

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
        finally:
            self._drop(writer)
            writer.close()
            try:
                await writer.wait_closed()
            except (ConnectionError, OSError):
                pass

    async def _do_handshake(self, reader, writer, addr) -> bool:
        """First frame must be HANDSHAKE. Validate pairing code if configured."""
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

        if PAIR_CODE and payload.get("code") != PAIR_CODE:
            log.warning("Bad pairing code from %s", addr)
            writer.write(encode(MsgType.ERROR, {"error": "invalid pairing code"}))
            await writer.drain()
            return False

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
                writer.write(encode(MsgType.MESSAGE_LIST, reply))

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
