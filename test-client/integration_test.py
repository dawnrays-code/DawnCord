"""
End-to-end integration test: VitaServer + a fake Discord bridge.

Verifies, without touching real Discord:
  - handshake required as first frame, pairing accept/reject
  - guild/channel/message request routing
  - all snowflake IDs travel as strings (cJSON on the Vita parses numbers
    as double and would corrupt 64-bit snowflakes)
  - live push (MESSAGE_NEW) interleaved with a pending request
  - send + ack, bad-request error handling

Usage:
    python integration_test.py
"""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "companion"))

import vita_server as vita_server_mod  # noqa: E402
from protocol import MsgType, encode, read_message  # noqa: E402
from vita_server import VitaServer  # noqa: E402

TEST_PORT = 9155

GUILD_ID = "1181234567890123456"   # deliberately > 2^53
CHANNEL_ID = "1181234567890123457"


class FakeBridge:
    """Mimics DiscordBridge's public surface with canned data."""

    def __init__(self):
        self.server = None
        self.active_channel = None
        self.sent = []

    def set_vita_server(self, server):
        self.server = server

    async def wait_until_ready(self):
        pass

    def get_guilds(self):
        return [
            {"id": "0", "name": "Direct Messages", "icon": None},
            {"id": GUILD_ID, "name": "Test Guild", "icon": None},
        ]

    def get_me(self):
        return {"name": "tester", "avatar": None}

    def get_channels(self, guild_id):
        if str(guild_id) == GUILD_ID:
            return [{"id": CHANNEL_ID, "name": "generale", "type": "text"}]
        return []

    async def get_messages(self, channel_id, limit, before=None):
        if before:
            # Scroll-back chunk: everything older than message `before`.
            return [
                {"id": "8", "author": "older", "author_id": "9",
                 "content": "from the past", "timestamp": "", "attachments": []},
            ]
        # Push a live message while this request is in flight: the client's
        # single reader must route both frames correctly.
        await self.server.broadcast(MsgType.MESSAGE_NEW, {
            "channel_id": CHANNEL_ID,
            "message": {"id": "1", "author": "pusher", "author_id": "2",
                        "content": "pushed mid-request", "timestamp": "", "attachments": []},
        })
        return [
            {"id": "10", "author": "alice", "author_id": "11",
             "content": "ciao", "timestamp": "", "attachments": []},
        ]

    async def send_message(self, channel_id, content):
        self.sent.append((channel_id, content))
        return True

    def get_members(self, channel_id):
        if str(channel_id) == CHANNEL_ID:
            return [{"name": "alice", "status": "online"},
                    {"name": "bob", "status": "offline"}]
        return []

    async def get_image(self, url, size=64):
        # Real bridge fetches from Discord's CDN and shrinks with Pillow;
        # here a tiny generated JPEG proves the round-trip.
        if url == "https://cdn.discordapp.com/avatars/1/x.png":
            from io import BytesIO

            from PIL import Image

            buf = BytesIO()
            Image.new("RGB", (size, size), (88, 101, 242)).save(buf, "JPEG")
            return buf.getvalue()
        return None

    def set_active_channel(self, channel_id):
        self.active_channel = channel_id


class Client:
    """Single-reader test client, same socket discipline as the real Vita."""

    def __init__(self, port):
        self.port = port
        self.pushes = []
        self._waiters = {}

    async def connect(self, pair_payload):
        self.reader, self.writer = await asyncio.open_connection("127.0.0.1", self.port)
        self.writer.write(encode(MsgType.HANDSHAKE, pair_payload))
        await self.writer.drain()
        first = await read_message(self.reader)
        if first and first[0] == MsgType.HANDSHAKE_ACK:
            self.server_version = first[1].get("version")
            self._task = asyncio.create_task(self._read_loop())
            return True
        self.writer.close()
        return first  # (ERROR, payload) or None, for rejection tests

    async def _read_loop(self):
        while True:
            msg = await read_message(self.reader)
            if msg is None:
                break
            msg_type, payload = msg
            if msg_type == MsgType.MESSAGE_NEW:
                self.pushes.append(payload)
            else:
                fut = self._waiters.pop(int(msg_type), None)
                if fut and not fut.done():
                    fut.set_result(payload)

    async def request(self, send_type, expect_type, payload):
        fut = asyncio.get_event_loop().create_future()
        self._waiters[int(expect_type)] = fut
        self.writer.write(encode(send_type, payload))
        await self.writer.drain()
        return await asyncio.wait_for(fut, timeout=5)

    def close(self):
        self._task.cancel()
        self.writer.close()


def check(cond, label):
    status = "ok" if cond else "FAIL"
    print(f"  [{status}] {label}")
    if not cond:
        raise AssertionError(label)


async def main():
    # A pair code from the local env/config.json would break the open
    # handshake checks; pairing is tested explicitly below.
    vita_server_mod.PAIR_CODE = None

    bridge = FakeBridge()
    server = VitaServer(bridge, host="127.0.0.1", port=TEST_PORT)
    bridge.set_vita_server(server)
    await server.start()

    print("core flow:")
    c = Client(TEST_PORT)
    check(await c.connect({}) is True, "handshake accepted")
    check(c.server_version == 2, "handshake carries the protocol version")

    guild_reply = await c.request(MsgType.REQUEST_GUILDS, MsgType.GUILD_LIST, {})
    guilds = guild_reply["guilds"]
    check(guilds[1]["id"] == GUILD_ID, "guild list, IDs are strings")
    check(guild_reply["me"]["name"] == "tester", "logged-in user shipped with guilds")

    chans = await c.request(MsgType.REQUEST_CHANNELS, MsgType.CHANNEL_LIST,
                            {"guild_id": GUILD_ID})
    check(chans["guild_id"] == GUILD_ID, "channel list echoes guild_id as string")
    check(chans["channels"][0]["id"] == CHANNEL_ID, "channel IDs are strings")

    msgs = await c.request(MsgType.REQUEST_MESSAGES, MsgType.MESSAGE_LIST,
                           {"channel_id": CHANNEL_ID, "limit": 20})
    check(msgs["channel_id"] == CHANNEL_ID, "message list echoes channel_id as string")
    check(msgs["messages"][0]["author"] == "alice", "message payload intact")
    check("before" not in msgs, "fresh list carries no before echo")

    older = await c.request(MsgType.REQUEST_MESSAGES, MsgType.MESSAGE_LIST,
                            {"channel_id": CHANNEL_ID, "limit": 20, "before": "10"})
    check(older.get("before") == "10" and older["messages"][0]["author"] == "older",
          "scroll-back chunk echoes before and returns older history")
    await asyncio.sleep(0.1)
    check(len(c.pushes) == 1 and c.pushes[0]["message"]["author"] == "pusher",
          "push routed correctly during pending request")

    ack = await c.request(MsgType.SEND_MESSAGE, MsgType.MESSAGE_SENT_ACK,
                          {"channel_id": CHANNEL_ID, "content": "hello"})
    check(ack["success"] and bridge.sent == [(int(CHANNEL_ID), "hello")],
          "send message + ack")

    membs = await c.request(MsgType.REQUEST_MEMBERS, MsgType.MEMBER_LIST,
                            {"channel_id": CHANNEL_ID})
    check(membs["channel_id"] == CHANNEL_ID, "member list echoes channel_id as string")
    check(membs["members"][0] == {"name": "alice", "status": "online"},
          "member payload intact")

    err = await c.request(MsgType.REQUEST_CHANNELS, MsgType.ERROR,
                          {"guild_id": "not-a-number"})
    check("bad request" in err["error"], "bad request returns ERROR, socket survives")

    import base64
    img = await c.request(MsgType.REQUEST_IMAGE, MsgType.IMAGE_DATA,
                          {"url": "https://cdn.discordapp.com/avatars/1/x.png",
                           "key": "avatar-1", "size": 32})
    jpeg = base64.b64decode(img["data"])
    check(img["key"] == "avatar-1" and jpeg[:2] == b"\xff\xd8",
          "image round-trip: key echoed, base64 decodes to JPEG")
    img = await c.request(MsgType.REQUEST_IMAGE, MsgType.IMAGE_DATA,
                          {"url": "https://evil.example.com/x.png", "key": "bad"})
    check(img["data"] is None, "unavailable image returns data=null")
    c.close()

    print("handshake enforcement:")
    r2, w2 = await asyncio.open_connection("127.0.0.1", TEST_PORT)
    w2.write(encode(MsgType.REQUEST_GUILDS, {}))
    await w2.drain()
    first = await read_message(r2)
    check(first is not None and first[0] == MsgType.ERROR,
          "non-handshake first frame rejected")
    w2.close()

    print("pairing:")
    vita_server_mod.PAIR_CODE = "1234"
    try:
        bad = Client(TEST_PORT)
        res = await bad.connect({"code": "wrong"})
        check(res is not True, "wrong pairing code rejected")
        good = Client(TEST_PORT)
        check(await good.connect({"code": "1234"}) is True, "correct pairing code accepted")
        good.close()
    finally:
        vita_server_mod.PAIR_CODE = None

    server._server.close()
    await server._server.wait_closed()
    print("\nAll checks passed.")


if __name__ == "__main__":
    asyncio.run(main())
