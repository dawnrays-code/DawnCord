"""
DawnCord binary protocol.

Wire format:
  [2 bytes, big-endian] message type
  [4 bytes, big-endian] payload length
  [N bytes]             JSON payload (UTF-8)
"""

import asyncio
import json
import struct
from enum import IntEnum

HEADER_FMT = "!HI"
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# Sent in HANDSHAKE_ACK. Bump when the companion learns a request the
# client depends on: the client warns on screen instead of degrading
# weirdly against a stale companion process (it happened: an old
# companion answering a scroll-back request with the newest window made
# the chat look like it looped).
PROTOCOL_VERSION = 2


class MsgType(IntEnum):
    # Vita -> Companion
    HANDSHAKE = 0x0001
    SEND_MESSAGE = 0x0020
    REQUEST_GUILDS = 0x0021
    REQUEST_CHANNELS = 0x0022
    REQUEST_MESSAGES = 0x0023
    SET_CHANNEL = 0x0024
    REQUEST_IMAGE = 0x0025
    REQUEST_MEMBERS = 0x0026
    JOIN_VOICE = 0x0027       # {channel_id}: relay this voice channel over UDP
    LEAVE_VOICE = 0x0028      # {}: stop the relay

    # Companion -> Vita
    HANDSHAKE_ACK = 0x0002
    GUILD_LIST = 0x0010
    CHANNEL_LIST = 0x0011
    MESSAGE_LIST = 0x0012
    MESSAGE_NEW = 0x0013
    MESSAGE_SENT_ACK = 0x0014
    # {"key": <echoed>, "data": <base64 jpeg>} or data=null if unavailable.
    # base64-in-JSON keeps the whole protocol single-format; the companion
    # resizes hard enough that payloads stay well under MAX_PAYLOAD.
    IMAGE_DATA = 0x0015
    # {"channel_id": <echoed>, "members": [{"name", "status"}]} sorted
    # online-first, capped so the reply stays in one frame.
    MEMBER_LIST = 0x0016
    # {"channel_id":..., "name":...}: someone started typing in the active
    # channel. Fire-and-forget push, the client just times it out.
    TYPING = 0x0017
    # {"active": bool, "channel_id":...}: result of a JOIN/LEAVE_VOICE.
    VOICE_STATE = 0x0018
    ERROR = 0x00FF


MAX_PAYLOAD = 64 * 1024


def encode(msg_type: MsgType, payload: dict) -> bytes:
    body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
    header = struct.pack(HEADER_FMT, int(msg_type), len(body))
    return header + body


async def read_message(reader):
    """
    Read one framed message. Returns (msg_type, payload_dict), or None on EOF.
    Unknown type codes are returned as a plain int (compares equal to any
    matching MsgType) so a bad frame can't crash the reader.
    """
    try:
        header = await reader.readexactly(HEADER_SIZE)
    except asyncio.IncompleteReadError:
        return None

    raw_type, length = struct.unpack(HEADER_FMT, header)
    if length > MAX_PAYLOAD:
        raise ValueError(f"payload too large: {length}")

    body = await reader.readexactly(length)
    try:
        msg_type = MsgType(raw_type)
    except ValueError:
        msg_type = raw_type

    return msg_type, json.loads(body)
