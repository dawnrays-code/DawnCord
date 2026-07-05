"""Generate the LiveArea assets (icon0, bg, startup) for the Vita client.

Run from anywhere; writes into vita-client/sce_sys/. Needs Pillow and an
internet connection (the Clyde comes from Discord's own CDN).

Default bg is the generated "dawn" theme (it's called DawnCord, after
all): night-blurple sky melting into a sunrise, stars, hill silhouettes
and a Clyde watching it. Pass an image as argv[1] to use artwork instead
(e.g. tools/assets/livearea-banner.jpg, Discord's floating islands).

LiveArea layout notes learned on real hardware:
  - the gate ("Avvia") frame sits in the middle of the screen and covers
    roughly the center third of bg.png: keep text out of there;
  - safe spots are the top strip, the left column and the bottom strip,
    BUT the console crops the outer edges of bg.png when it scales it to
    the screen: keep anything that matters ~40px away from every border;
  - text sitting straight on artwork needs a dark pill behind it or it
    disappears (white on light clouds);
  - all PNGs must be 8-bit palette or the LiveArea may render them wrong.
"""
import io
import math
import random
import sys
import urllib.request
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageFont

OUT = Path(__file__).resolve().parent.parent / "vita-client" / "sce_sys"
(OUT / "livearea" / "contents").mkdir(parents=True, exist_ok=True)

PHOTO = Path(sys.argv[1]) if len(sys.argv) > 1 else None

H = {"User-Agent": "Mozilla/5.0 (Windows NT 10.0; Win64; x64) Chrome/126.0"}
ICON_URL = "https://cdn.discordapp.com/embed/avatars/0.png?size=512"

BLURPLE = (88, 101, 242)
DARK = (49, 51, 56)           # #313338, Discord chat background
SUBTLE = (181, 186, 193)
FAINT = (148, 155, 164)

raw = urllib.request.urlopen(urllib.request.Request(ICON_URL, headers=H), timeout=30).read()
src = Image.open(io.BytesIO(raw)).convert("RGBA")
print("clyde scaricato:", src.size)


def rounded(img: Image.Image, radius: int) -> Image.Image:
    mask = Image.new("L", img.size, 0)
    d = ImageDraw.Draw(mask)
    d.rounded_rectangle([0, 0, img.size[0] - 1, img.size[1] - 1], radius=radius, fill=255)
    out = img.copy()
    out.putalpha(mask)
    return out


def clyde_glyph(size: int, alpha: int) -> Image.Image:
    """The source asset is a filled blurple square: extract just the white
    Clyde shape so it can be used as a watermark without the box."""
    img = src.resize((size, size), Image.LANCZOS)
    glyph = Image.new("RGBA", img.size, (0, 0, 0, 0))
    px, gx = img.load(), glyph.load()
    for y in range(img.size[1]):
        for x in range(img.size[0]):
            r, g, b, a = px[x, y]
            if a > 0 and r > 180 and g > 180 and b > 200:
                gx[x, y] = (255, 255, 255, alpha)
    return glyph


def to_palette(img: Image.Image) -> Image.Image:
    return img.convert("RGBA").quantize(colors=256, method=Image.FASTOCTREE)


def to_palette_bg(img: Image.Image) -> Image.Image:
    """The bg is one huge smooth gradient: without help, 256 colors show as
    ugly bands on the OLED ("low bit" look). A whisper of noise breaks the
    flat runs and Floyd-Steinberg dithering does the rest. Opaque only
    (the bg has no transparency to preserve)."""
    flat = img.convert("RGB")
    grain = Image.effect_noise(flat.size, 12).point(lambda v: abs(v - 128) // 14)
    flat = ImageChops.add(flat, Image.merge("RGB", (grain, grain, grain)))
    return flat.quantize(colors=256, method=Image.MEDIANCUT,
                         dither=Image.Dither.FLOYDSTEINBERG)


def font(size: int, bold: bool = True):
    for name in (("segoeuib.ttf" if bold else "segoeui.ttf"),
                 ("arialbd.ttf" if bold else "arial.ttf")):
        try:
            return ImageFont.truetype(rf"C:\Windows\Fonts\{name}", size)
        except OSError:
            continue
    return ImageFont.load_default()


# --- icon0.png: 128x128, rounded bubble ---
icon = rounded(src.resize((128, 128), Image.LANCZOS), 26)
to_palette(icon).save(OUT / "icon0.png")
print("icon0.png ok")

# --- bg.png: 840x500, text clear of the central gate ---
bg = Image.new("RGBA", (840, 500), DARK + (255,))
d = ImageDraw.Draw(bg)

if PHOTO and PHOTO.exists():
    # The banner is much wider than 840x500: shrinking it to width and
    # cropping would amputate the floating islands at the sides, which are
    # the whole point. Instead the full strip sits at the bottom and the
    # empty sky above it is extended by stretching one of its own rows, so
    # nothing is lost and the center stays clear for the gate.
    photo = Image.open(PHOTO).convert("RGB")
    strip_h = round(photo.height * 840 / photo.width)
    strip = photo.resize((840, strip_h), Image.LANCZOS)
    top_h = 500 - strip_h

    # Blur kills the vertical streaks that stretching a single row leaves
    # where clouds or light rays crossed it.
    sky = strip.crop((0, 2, 840, 3)).resize((840, top_h + 24))
    sky = sky.filter(ImageFilter.GaussianBlur(24))
    bg.paste(sky, (0, 0))
    bg.paste(strip, (0, top_h))

    # Soft dark scrim behind the header text, fading out by y=185.
    scrim = Image.new("RGBA", (840, 185), (0, 0, 0, 0))
    for y in range(185):
        alpha = int(130 * (1 - y / 185))
        ImageDraw.Draw(scrim).line([(0, y), (839, y)], fill=(20, 18, 40, alpha))
    bg.alpha_composite(scrim, (0, 0))
else:
    # The dawn. Night-blurple fading into sunrise, all generated.
    stops = [(0.00, (14, 10, 42)),
             (0.42, (43, 32, 100)),
             (0.68, (86, 74, 190)),
             (0.84, (176, 108, 160)),
             (1.00, (248, 166, 106))]
    # Sun glow baked into the gradient math (a composited blurred layer
    # leaves a visible contour where its alpha tail hits zero; a smooth
    # exponential falloff added per pixel does not).
    gx, gy = 420.0, 466.0
    px = bg.load()
    for y in range(500):
        t = y / 499.0
        col = stops[-1][1]
        for (t0, c0), (t1, c1) in zip(stops, stops[1:]):
            if t0 <= t <= t1:
                k = (t - t0) / (t1 - t0)
                col = tuple(c0[i] + (c1[i] - c0[i]) * k for i in range(3))
                break
        dy = (y - gy) / 110.0
        for x in range(840):
            dx = (x - gx) / 260.0
            f = math.exp(-(dx * dx + dy * dy))
            px[x, y] = (min(255, int(col[0] + 205 * f)),
                        min(255, int(col[1] + 145 * f)),
                        min(255, int(col[2] + 80 * f)), 255)

    # Stars, denser and brighter toward the top, fading near the dawn.
    stars = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    sdraw = ImageDraw.Draw(stars)
    rng = random.Random(1204)
    for _ in range(110):
        sx, sy = rng.randrange(16, 824), rng.randrange(10, 300)
        a = int(rng.randrange(60, 170) * (1 - sy / 340.0))
        r = rng.choice((1, 1, 1, 2))
        sdraw.ellipse([sx, sy, sx + r, sy + r], fill=(255, 255, 255, a))
    bg.alpha_composite(stars)

    # Rolling hill silhouettes, and a Clyde watching the sunrise.
    sil = Image.new("RGBA", bg.size, (0, 0, 0, 0))
    sdw = ImageDraw.Draw(sil)
    sdw.ellipse([-340, 434, 470, 740], fill=(17, 13, 40, 255))
    sdw.ellipse([390, 452, 1180, 780], fill=(23, 17, 52, 255))
    clyde = clyde_glyph(120, 255)
    dark_clyde = Image.new("RGBA", clyde.size, (0, 0, 0, 0))
    dark_clyde.paste((17, 13, 40, 255), (0, 0), clyde.split()[3])
    sil.alpha_composite(dark_clyde, (356, 368))
    bg.alpha_composite(sil)

# Header strip: small bubble + title + subtitle, above the gate area but
# clear of the top edge the console crops away (~40px of safe margin).
bg.alpha_composite(rounded(src.resize((64, 64), Image.LANCZOS), 15), (56, 56))
d.text((137, 53), "DawnCord", font=font(44), fill=(10, 10, 25, 160))  # shadow
d.text((136, 52), "DawnCord", font=font(44), fill=(255, 255, 255, 255))
d.text((138, 108), "Discord per PS Vita", font=font(22, bold=False), fill=SUBTLE + (255,))
d.line([(56, 142), (784, 142)], fill=BLURPLE + (120,), width=2)

# Credit in the bottom strip, on a dark pill so it reads on any artwork,
# lifted off the bottom edge for the same crop reason.
credit = "by dawnrays & Claude"
credit_font = font(20, bold=False)
cw = d.textbbox((0, 0), credit, font=credit_font)[2]
pill = Image.new("RGBA", bg.size, (0, 0, 0, 0))
ImageDraw.Draw(pill).rounded_rectangle(
    [42, 424, 42 + cw + 28, 462], radius=19, fill=(31, 26, 62, 200))
bg.alpha_composite(pill)
d.text((56, 432), credit, font=credit_font, fill=(240, 241, 245, 255))
to_palette_bg(bg).save(OUT / "livearea" / "contents" / "bg.png")
print("bg.png ok" + (" (photo)" if PHOTO and PHOTO.exists() else " (dawn)"))

# --- startup.png: 280x158, the gate button ---
st = Image.new("RGBA", (280, 158), (0, 0, 0, 0))
d = ImageDraw.Draw(st)
d.rounded_rectangle([10, 10, 269, 147], radius=24, fill=BLURPLE + (255,))
st.alpha_composite(src.resize((72, 72), Image.LANCZOS), (34, 43))
d.text((120, 56), "Avvia", font=font(34), fill=(255, 255, 255, 255))
to_palette(st).save(OUT / "livearea" / "contents" / "startup.png")
print("startup.png ok")

print("Asset in", OUT)
