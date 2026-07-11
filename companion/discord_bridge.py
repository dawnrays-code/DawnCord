"""
Discord bridge: wraps discord.py-self and exposes a simplified interface
for the Vita server to query guilds, channels, messages, and send messages.

Uses discord.py-self (user tokens) so the companion logs in AS the user
and sees everything they see: all servers, all DMs, all channels.
"""

import asyncio
import logging
import re
import unicodedata
from datetime import timezone
from io import BytesIO
from urllib.parse import urlparse

import aiohttp
import discord
from PIL import Image

from protocol import MsgType

log = logging.getLogger("dawncord.discord")

# Only fetch images from known media CDNs: the image endpoint is driven
# by whatever URL a LAN client sends, so an allowlist keeps the companion
# from being used as a proxy into arbitrary (or internal) hosts. Tenor,
# Giphy, Imgur and YouTube thumbnails cover what people actually paste.
# Suffix entries (leading dot) match any subdomain: Tenor and Klipy serve
# GIF stills from numbered/rotating hosts (media1.tenor.com, ...), so an
# exact host would miss most of them.
_IMAGE_HOSTS = ("cdn.discordapp.com", ".discordapp.net", ".discordapp.com",
                ".tenor.com", ".giphy.com", ".klipy.com",
                "i.imgur.com", "i.ytimg.com", "img.youtube.com")

# The wire protocol caps payloads at 64 KiB; base64 adds ~33%, so the
# re-encoded JPEG must stay comfortably below that.
_IMAGE_MAX_JPEG = 45_000
_IMAGE_CACHE_MAX = 128

# Characters the Vita's PGF font can't draw (emoji, decorative symbols,
# private-use glyphs, zero-width junk). Popular servers love these in
# channel names; on the Vita they render as boxes/underscores.
_UNRENDERABLE = re.compile(
    "[🀀-🿿"      # emoji, symbols & pictographs
    "←-⯿"               # arrows, box drawing, dingbats
    "⸀-⹿"               # supplemental punctuation
    "-"               # private use area
    "︀-️"               # variation selectors
    "​-‏ -‮⁠-⁯"  # zero-width / format chars
    "]+"
)


# Combining marks (zalgo decorations and stray accents left over after
# normalization): PGF draws them as smudges over the previous glyph.
_COMBINING = re.compile("[̀-ͯ⃐-⃿︠-︯]+")


def _clean_text(text: str | None, limit: int) -> str:
    """Multiline-safe variant of _clean_name for embed descriptions:
    strips what PGF can't draw but keeps the line breaks."""
    cleaned = unicodedata.normalize("NFKC", text or "")
    cleaned = _UNRENDERABLE.sub("", cleaned)
    cleaned = _COMBINING.sub("", cleaned)
    return cleaned[:limit].strip()


def _clean_name(name: str | None) -> str:
    # NFKC first: it folds the "fancy font" tricks people write nicks with
    # (fullwidth ＡＢＣ, math bold 𝐀𝐁𝐂, circled...) back into plain letters
    # the Vita's PGF font can actually draw, instead of dropping the whole
    # name to boxes and underscores.
    cleaned = unicodedata.normalize("NFKC", name or "")
    cleaned = _UNRENDERABLE.sub("", cleaned)
    cleaned = _COMBINING.sub("", cleaned)
    cleaned = re.sub(r"\s+", " ", cleaned).strip(" -_|.·•")
    return cleaned or (name or "?")


class DiscordBridge:
    def __init__(self, token: str):
        # discord.py-self doesn't use intents for user accounts
        self._client = discord.Client()
        self._token = token
        self._vita_server = None
        self._active_channel_id: int | None = None
        self._ready = asyncio.Event()
        self._http: aiohttp.ClientSession | None = None
        self._img_cache: dict[tuple[str, int], bytes | None] = {}
        self._voice = None   # lazily created VoiceRelay

        self._client.event(self.on_ready)
        self._client.event(self.on_message)
        self._client.event(self.on_typing)

    def set_vita_server(self, server):
        self._vita_server = server

    async def start(self):
        # discord.py-self auto-detects user token, no bot= parameter needed
        await self._client.start(self._token)

    async def wait_until_ready(self):
        await self._ready.wait()

    async def on_ready(self):
        log.info("Discord connected as %s", self._client.user)
        log.info("Servers: %d | DMs available: yes", len(self._client.guilds))
        self._ready.set()

    async def on_typing(self, channel, user, when):
        if user == self._client.user:
            return
        if self._vita_server and channel.id == self._active_channel_id:
            await self._vita_server.broadcast(MsgType.TYPING, {
                "channel_id": str(channel.id),
                "name": _clean_name(getattr(user, "display_name", str(user))),
            })

    async def on_message(self, message: discord.Message):
        if message.author == self._client.user:
            return
        if self._vita_server and message.channel.id == self._active_channel_id:
            await self._vita_server.broadcast(MsgType.MESSAGE_NEW, {
                "channel_id": str(message.channel.id),
                "message": self._serialize_message(message),
            })

    # Snowflake IDs are serialized as strings everywhere: they exceed the
    # 53-bit exact-integer range of doubles, and cJSON on the Vita parses
    # JSON numbers as double, which would silently corrupt them.

    def get_guilds(self) -> list[dict]:
        guilds = [
            {"id": str(g.id), "name": _clean_name(g.name),
             "icon": str(g.icon.url) if g.icon else None}
            for g in self._client.guilds
        ]
        guilds.insert(0, {"id": "0", "name": "Direct Messages", "icon": None})
        return guilds

    def get_me(self) -> dict | None:
        """Who the companion is logged in as, for the client's profile box."""
        user = self._client.user
        if not user:
            return None
        return {"name": _clean_name(user.display_name or user.name),
                "avatar": self._avatar_url(user)}

    def get_channels(self, guild_id: int) -> list[dict]:
        if guild_id == 0:
            return [
                {"id": str(dm.id), "name": _clean_name(self._dm_name(dm)),
                 "type": "dm", "icon": self._dm_icon(dm)}
                for dm in self._client.private_channels
            ]
        guild = self._client.get_guild(guild_id)
        if not guild:
            return []

        def _readable(c) -> bool:
            # Skip channels this account can't read: they'd just be 403s
            # waiting to happen when opened from the Vita.
            try:
                return not guild.me or c.permissions_for(guild.me).read_messages
            except Exception:
                return True  # permission resolution is best-effort

        # by_category() yields (category, channels) in the same order as the
        # official Discord sidebar. Category rows go out with a "cat:" id so
        # the client renders them as non-selectable headers. Voice channels
        # go out with type "voice" so the client can render and join them.
        channels = []
        for category, chans in guild.by_category():
            texts = [c for c in chans
                     if isinstance(c, discord.TextChannel) and _readable(c)]
            voices = [c for c in chans
                      if isinstance(c, discord.VoiceChannel) and _readable(c)]
            if not texts and not voices:
                continue
            if category is not None:
                channels.append({"id": f"cat:{category.id}",
                                 "name": _clean_name(category.name),
                                 "type": "category"})
            channels.extend({"id": str(c.id), "name": _clean_name(c.name),
                             "type": "text"} for c in texts)
            for c in voices:
                channels.append({"id": str(c.id), "name": _clean_name(c.name),
                                 "type": "voice"})
                # Who's connected, as a display-only "vu:" row right under
                # the channel (the client skips it while navigating).
                # Refreshed when the channel list is (re)fetched: TRIANGLE.
                folks = [_clean_name(m.display_name) for m in c.members[:3]]
                if folks:
                    extra = len(c.members) - len(folks)
                    label = ", ".join(folks) + (f" +{extra}" if extra > 0 else "")
                    channels.append({"id": f"vu:{c.id}", "name": label,
                                     "type": "voiceusers"})
        return channels

    async def get_messages(self, channel_id: int, limit: int = 50,
                           before_id: int | None = None) -> list[dict]:
        """Newest window by default; with before_id, the chunk of history
        right before that message (for scroll-back paging)."""
        channel = self._client.get_channel(channel_id)
        if not channel:
            return []
        kwargs: dict = {"limit": limit}
        if before_id:
            kwargs["before"] = discord.Object(id=before_id)
        messages = []
        try:
            async for msg in channel.history(**kwargs):
                messages.append(self._serialize_message(msg))
        except discord.HTTPException as e:
            # 403 on restricted history and friends: empty list, not a crash.
            log.warning("Cannot read history of %s: %s", channel_id, e)
            return []
        messages.reverse()
        return messages

    async def send_message(self, channel_id: int, content: str) -> bool:
        channel = self._client.get_channel(channel_id)
        if not channel:
            return False
        try:
            await channel.send(content)
            return True
        except discord.HTTPException:
            log.exception("Failed to send message to %s", channel_id)
            return False

    def set_active_channel(self, channel_id: int):
        self._active_channel_id = channel_id

    # --- voice (listen-only, optional) ---

    async def join_voice(self, channel_id: int, vita_ip: str) -> bool:
        channel = self._client.get_channel(channel_id)
        if not isinstance(channel, discord.VoiceChannel):
            log.warning("JOIN_VOICE for %s: not a voice channel", channel_id)
            return False
        # Your account can only be in one voice channel at a time: if you're
        # already connected from the desktop Discord app, that session gets
        # moved here. DawnCord becomes your voice presence, it isn't a second
        # one. Worth knowing when a join behaves unexpectedly.
        if self._client.voice_clients:
            log.info("JOIN_VOICE: account already had a voice session; "
                     "moving it to %s", channel.name)
        log.info("JOIN_VOICE '%s' -> streaming to %s", channel.name, vita_ip)
        from voice import VoiceRelay
        if self._voice is None:
            self._voice = VoiceRelay()
        try:
            await self._voice.join(channel, vita_ip)
            return True
        except Exception:
            log.exception("Failed to join voice %s", channel_id)
            return False

    async def leave_voice(self):
        if self._voice is not None:
            await self._voice.leave()

    # Order for the member rail: online people first, then away/busy,
    # offline (or unknown, common on big guilds where presences aren't
    # cached) at the bottom, alphabetical inside each bucket.
    _STATUS_RANK = {"online": 0, "idle": 1, "dnd": 2}

    def get_members(self, channel_id: int, limit: int = 40) -> list[dict]:
        """
        Members who can see the channel, from the local cache (no gateway
        round-trip: on huge guilds the cache is partial and that's fine,
        the rail is a peek, not a census).
        """
        channel = self._client.get_channel(channel_id)
        if channel is None:
            return []

        users: list[dict] = []
        if isinstance(channel, (discord.DMChannel, discord.GroupChannel)):
            people = list(getattr(channel, "recipients", None)
                          or ([channel.recipient] if getattr(channel, "recipient", None) else []))
            if self._client.user:
                people.append(self._client.user)
            users = [{"name": _clean_name(getattr(u, "display_name", str(u))),
                      "status": "online" if u == self._client.user else "offline"}
                     for u in people]
        else:
            for m in getattr(channel, "members", []):
                status = str(getattr(m, "status", "offline"))
                users.append({"name": _clean_name(m.display_name),
                              "status": status if status in ("online", "idle", "dnd")
                              else "offline"})

        users.sort(key=lambda u: (self._STATUS_RANK.get(u["status"], 3),
                                  u["name"].casefold()))
        return users[:limit]

    async def get_image(self, url: str, size: int = 64) -> bytes | None:
        """
        Fetch an image from Discord's CDN, shrink it to a size x size
        thumbnail JPEG small enough for one protocol frame. None on any
        failure (bad host, 404, not an image...). Results are cached.
        """
        size = max(16, min(int(size), 512))  # 512: expanded attachment view
        cache_key = (url, size)
        if cache_key in self._img_cache:
            return self._img_cache[cache_key]

        host = urlparse(url).hostname or ""
        suffixes = tuple(h for h in _IMAGE_HOSTS if h.startswith("."))
        if not (host in _IMAGE_HOSTS or host.endswith(suffixes)):
            log.warning("Refusing image fetch from unknown host: %s", host)
            return None

        data = None
        try:
            if self._http is None or self._http.closed:
                self._http = aiohttp.ClientSession()
            async with self._http.get(
                url, timeout=aiohttp.ClientTimeout(total=10)
            ) as resp:
                if resp.status == 200:
                    # resp.read() collects the WHOLE body. The previous
                    # content.read(n) returns only the first buffered chunk,
                    # which truncated every real-world CDN download (PIL then
                    # failed with "image file is truncated").
                    if (resp.content_length or 0) <= 8 * 1024 * 1024:
                        raw = await resp.read()
                        if len(raw) <= 8 * 1024 * 1024:
                            data = self._shrink_image(raw, size)
                    else:
                        log.warning("Image too large, skipping: %s", url)
        except (aiohttp.ClientError, asyncio.TimeoutError, OSError) as e:
            log.warning("Image fetch failed for %s: %s", url, e)

        if len(self._img_cache) >= _IMAGE_CACHE_MAX:
            self._img_cache.pop(next(iter(self._img_cache)))
        self._img_cache[cache_key] = data
        return data

    @staticmethod
    def _shrink_image(raw: bytes, size: int) -> bytes | None:
        try:
            img = Image.open(BytesIO(raw))
            img = img.convert("RGB")
            img.thumbnail((size, size))
            for quality in (85, 70, 55, 40):
                buf = BytesIO()
                img.save(buf, "JPEG", quality=quality)
                if buf.tell() <= _IMAGE_MAX_JPEG:
                    return buf.getvalue()
            return None
        except Exception as e:
            log.warning("Image decode failed: %s", e)
            return None

    @staticmethod
    def _dm_name(channel) -> str:
        if isinstance(channel, discord.DMChannel) and channel.recipient:
            return channel.recipient.display_name
        elif isinstance(channel, discord.GroupChannel):
            return channel.name or ", ".join(
                u.display_name for u in channel.recipients[:3]
            )
        return "Unknown"

    @staticmethod
    def _dm_icon(channel) -> str | None:
        try:
            if isinstance(channel, discord.DMChannel) and channel.recipient:
                return str(channel.recipient.display_avatar)
            if isinstance(channel, discord.GroupChannel) and channel.icon:
                return str(channel.icon)
        except Exception:
            pass
        return None

    @staticmethod
    def _avatar_url(user) -> str | None:
        try:
            return str(user.display_avatar)
        except Exception:
            return None

    @staticmethod
    def _serialize_embeds(msg: discord.Message) -> list[dict]:
        """Structured embeds for the client's Discord-style boxes (color
        bar + title + description). The embed's image travels separately
        as the message "image"."""
        out = []
        for e in msg.embeds[:2]:
            title = _clean_text(str(e.title) if e.title else "", 90)
            desc = _clean_text(str(e.description) if e.description else "", 220)
            if not title and not desc:
                continue
            out.append({
                "title": title,
                "description": desc,
                "color": e.colour.value if e.colour else 0,
            })
        return out

    @staticmethod
    def _first_image(msg: discord.Message) -> str | None:
        """URL of the first renderable image: attachment, sticker, or embed
        image/thumbnail (covers Tenor GIFs and YouTube previews). Animated
        sources come through as a static first frame: Pillow decodes frame
        zero of GIF/APNG on the companion, the Vita never knows."""
        for a in msg.attachments:
            if (a.content_type or "").startswith("image/"):
                return a.url
        for s in getattr(msg, "stickers", []):
            fmt = getattr(getattr(s, "format", None), "name", "")
            if fmt in ("png", "apng", "gif"):   # lottie is json, undrawable
                return str(s.url)
        for e in msg.embeds:
            if e.image and e.image.url:
                return str(e.image.url)
            if e.thumbnail and e.thumbnail.url:
                return str(e.thumbnail.url)
        return None

    @staticmethod
    def _has_video(msg: discord.Message) -> bool:
        """True for video embeds (YouTube & co.): the client overlays a
        play triangle on the thumbnail so nobody expects it to play."""
        for e in msg.embeds:
            if e.type == "video" or e.video:
                return True
        return False

    _CUSTOM_EMOJI = re.compile(r"<a?(:\w+:)\d+>")
    _URL_TOKEN = re.compile(r"^https?://\S+$")

    @classmethod
    def _tidy_content(cls, content: str, has_media: bool) -> str:
        """Custom emoji codes become their readable :name:. And when the
        message is nothing but media URLs that we're already rendering,
        the raw links disappear (author's request: no URL under a
        rendered image, even with several of them)."""
        content = cls._CUSTOM_EMOJI.sub(r"\1", content)
        if has_media:
            tokens = content.split()
            if tokens and all(cls._URL_TOKEN.match(t) for t in tokens):
                return ""
        return content

    @staticmethod
    def _serialize_message(msg: discord.Message) -> dict:
        # system_content covers joins/pins/boosts; equals content for
        # normal messages.
        content = msg.system_content or msg.content or ""
        image = DiscordBridge._first_image(msg)
        embeds = DiscordBridge._serialize_embeds(msg)
        return {
            "id": str(msg.id),
            "author": _clean_name(msg.author.display_name),
            "author_id": str(msg.author.id),
            "avatar": DiscordBridge._avatar_url(msg.author),
            "content": DiscordBridge._tidy_content(content,
                                                   bool(image or embeds)),
            "timestamp": msg.created_at.replace(tzinfo=timezone.utc).isoformat(),
            "image": image,
            "video": DiscordBridge._has_video(msg),
            "embeds": embeds,
            "attachments": [
                {"url": a.url, "filename": a.filename}
                for a in msg.attachments
            ],
        }
