"""
Test client that simulates a Vita connecting to the companion app.
Run this to verify the protocol works without Vita hardware.

Usage:
    python test_client.py [host] [port]

Set DAWNCORD_PAIR_CODE to match the companion if pairing is enabled.

Design: ONE background reader owns the socket. Requests are fire-and-forget;
replies are routed back by message type. This mirrors how the real Vita
client must work and avoids two coroutines racing on the same stream.
"""

import asyncio
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent.parent / "companion"))

from protocol import MsgType, encode, read_message  # noqa: E402
from vita_server import _load_pair_code  # noqa: E402

# Same sources as the companion (env var, then companion/config.json),
# so pairing just works when both run on the same machine.
PAIR_CODE = _load_pair_code()


class TestVitaClient:
    def __init__(self, host: str = "127.0.0.1", port: int = 9100):
        self.host = host
        self.port = port
        self.reader = None
        self.writer = None
        self._current_channel = None
        self._waiters: dict[int, asyncio.Future] = {}
        self._reader_task = None

    async def connect(self):
        self.reader, self.writer = await asyncio.open_connection(self.host, self.port)
        # Handshake must be the first frame the server sees.
        self.writer.write(encode(MsgType.HANDSHAKE,
                                 {"code": PAIR_CODE} if PAIR_CODE else {}))
        await self.writer.drain()

        first = await read_message(self.reader)
        if not first or first[0] != MsgType.HANDSHAKE_ACK:
            raise RuntimeError(f"Handshake failed: {first}")
        print(f"Connected! Server version: {first[1].get('version')}")

        # From here on, the background reader owns the socket.
        self._reader_task = asyncio.create_task(self._read_loop())

    async def _read_loop(self):
        try:
            while True:
                msg = await read_message(self.reader)
                if msg is None:
                    break
                msg_type, payload = msg
                if msg_type == MsgType.MESSAGE_NEW:
                    m = payload["message"]
                    print(f"\r  [{m['author']}] {m['content']}\n> ", end="")
                else:
                    fut = self._waiters.pop(int(msg_type), None)
                    if fut and not fut.done():
                        fut.set_result(payload)
        except asyncio.CancelledError:
            pass

    async def request(self, send_type: MsgType, expect_type: MsgType, payload: dict) -> dict:
        """Send a request and wait for the reply of the expected type."""
        fut = asyncio.get_event_loop().create_future()
        self._waiters[int(expect_type)] = fut
        self.writer.write(encode(send_type, payload))
        await self.writer.drain()
        return await asyncio.wait_for(fut, timeout=15)

    def fire(self, send_type: MsgType, payload: dict):
        """Send with no expected reply (e.g. SET_CHANNEL)."""
        self.writer.write(encode(send_type, payload))

    async def interactive(self):
        await self.connect()

        guild_data = await self.request(MsgType.REQUEST_GUILDS, MsgType.GUILD_LIST, {})
        guilds = guild_data["guilds"]
        print("\n--- Servers ---")
        for i, g in enumerate(guilds):
            print(f"  [{i}] {g['name']}")

        idx = int(input("\nSelect server: "))
        guild_id = guilds[idx]["id"]

        chan_data = await self.request(MsgType.REQUEST_CHANNELS, MsgType.CHANNEL_LIST,
                                       {"guild_id": guild_id})
        channels = chan_data["channels"]
        print("\n--- Channels ---")
        for i, c in enumerate(channels):
            print(f"  [{i}] #{c['name']}")

        idx = int(input("\nSelect channel: "))
        self._current_channel = channels[idx]["id"]

        self.fire(MsgType.SET_CHANNEL, {"channel_id": self._current_channel})

        msg_data = await self.request(MsgType.REQUEST_MESSAGES, MsgType.MESSAGE_LIST, {
            "channel_id": self._current_channel,
            "limit": 20,
        })
        print("\n--- Messages ---")
        for m in msg_data["messages"]:
            print(f"  [{m['author']}] {m['content']}")

        print("\nType messages to send (Ctrl+C to quit):")
        try:
            while True:
                text = await asyncio.get_event_loop().run_in_executor(None, input, "> ")
                if text.strip():
                    ack = await self.request(MsgType.SEND_MESSAGE, MsgType.MESSAGE_SENT_ACK, {
                        "channel_id": self._current_channel,
                        "content": text,
                    })
                    if not ack.get("success"):
                        print("  (send failed)")
        except (KeyboardInterrupt, EOFError):
            print("\nDisconnected.")
        finally:
            if self._reader_task:
                self._reader_task.cancel()
            self.writer.close()


async def main():
    host = sys.argv[1] if len(sys.argv) > 1 else "127.0.0.1"
    port = int(sys.argv[2]) if len(sys.argv) > 2 else 9100
    client = TestVitaClient(host, port)
    await client.interactive()


if __name__ == "__main__":
    asyncio.run(main())
