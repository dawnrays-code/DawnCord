#include "ui.h"
#include "imgcache.h"

#include <vita2d.h>
#include <psp2/common_dialog.h>
#include <psp2/power.h>
#include <psp2/rtc.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

/* vita2d rasterizes each glyph ONCE, at the first size it's drawn at, and
   rescales that bitmap forever after (its atlas is keyed by glyph only).
   Ten different text sizes through one handle = blurry rescales and glyph
   drops everywhere (hardware screenshots: overlapping letters, truncated
   names). So: one font handle PER SIZE, five consolidated sizes, each
   atlas holding pixel-exact glyphs, pre-warmed at init so no atlas is
   ever touched mid-frame. */
/* Raster sizes tuned for the 5" 220-DPI panel held at console distance:
   one notch up from phone-comfortable (hardware feedback, twice). */
#define FONT_SIZES 5
static const int font_px[FONT_SIZES] = { 14, 16, 19, 22, 24 };
static vita2d_font *ttf_h[FONT_SIZES];
static int ttf_ok = 0;            /* all handles loaded */
static vita2d_pgf *font = NULL;   /* fallback if the TTF doesn't load */

static int px_slot(int px)
{
    if (px <= 13) return 0;
    if (px <= 16) return 1;
    if (px <= 18) return 2;
    if (px <= 21) return 3;
    return 4;
}

/* All text sizes are in PIXELS now: the TTF renders any size sharply,
   which is what finally kills the scaled-bitmap-PGF grain. */
#define HEADER_H    48
#define ROW_H       32
#define LINE_H      28
#define FOOTER_H    28
#define CHAT_SCALE  20
#define LIST_SCALE  19
#define PAD_X       14

#define AVATAR_SZ   36

/* Workspace: channels | chat | members. The chat column gets whatever the
   two rails leave (960 - 230 - 190 = 540px). */
#define RAIL_W      230
#define MEMBERS_W   190
#define RAIL_ROW_H  32
#define RAIL_SCALE  18

/* Adaptive widths: the column you are working in gets room, the others
   give some back. Five inches cannot afford three full columns at once. */
#define RAIL_W_WIDE     248
#define RAIL_W_NARROW   200
#define MEMBERS_W_WIDE  206
#define MEMBERS_W_NARROW 160

/* The three panes are cards floating on the frame colour, with a gutter
   all round. The frame showing through between them is what separates
   them now: no borders, no adjacent full-bleed fills. */
#define GUTTER   8
#define CARD_TOP (HEADER_H + GUTTER)
#define CARD_BOT (SCREEN_H - FOOTER_H - GUTTER)
#define CARD_H   (CARD_BOT - CARD_TOP)

/* Live widths for the current frame, eased in render_workspace and read by
   all three pane renderers. */
static int rail_w = RAIL_W;
static int members_w = MEMBERS_W;

#define WRAP_MAX_LINES 12
#define WRAP_LINE_LEN  256

#define COLOR_BORDER RGBA8(72, 76, 84, 255)

/* Attachment thumbnails in chat and their tap targets. */
#define THUMB_MAX_W  220
#define THUMB_MAX_H  130
#define PROFILE_H    56

typedef struct {
    int x, y, w, h;
    char url[IMG_KEY_LEN];
} hit_rect;

#define HIT_MAX 24
static hit_rect hits[HIT_MAX];
static int hit_count = 0;

/* All text goes through these two. Size in pixels, snapped to the closest
   of the five handles so every glyph draws at its native raster size. */
static void ui_text(int x, int y, unsigned int color, int px, const char *s)
{
    int slot = px_slot(px);
    if (ttf_ok)
        vita2d_font_draw_text(ttf_h[slot], x, y, color, font_px[slot], s);
    else
        vita2d_pgf_draw_text(font, x, y, color, px / 19.0f, s);
}

static int ui_text_width(int px, const char *s)
{
    int slot = px_slot(px);
    if (ttf_ok)
        return vita2d_font_text_width(ttf_h[slot], font_px[slot], s);
    return vita2d_pgf_text_width(font, px / 19.0f, s);
}

/* ---- panel material ----

   vita2d has no rounded rectangle, no gradient and no blur. Rather than
   living with hard 90-degree corners and 1px borders (which is what makes
   hand-made interfaces look hand-made), we build two tiny textures once at
   init and tint them at draw time, so one texture serves every colour:

     tex_round  a 48x48 white rounded square, drawn as a nine-slice, giving
                a rounded panel of any size for 8 texture draws plus a fill
     tex_ramp   a 1x64 vertical alpha ramp, stretched to any rectangle,
                giving shadows under edges and soft gradients on panels

   Both fall back gracefully: if a texture fails to allocate, panels simply
   draw as the plain rectangles they used to be. */
#define RAMP_H    64

/* A radius hierarchy rather than one value on everything: small for rows,
   chips and pills, medium for cards and the composer, large for the image
   viewer. Each is rasterized at its own native size, so a 6px corner is
   drawn as a 6px corner instead of a 14px one squeezed down. */
#define R_SM 0
#define R_MD 1
#define R_LG 2
static const int round_r[3] = { 6, 12, 20 };
static vita2d_texture *tex_round[3] = { NULL, NULL, NULL };
static vita2d_texture *tex_ramp = NULL;

static vita2d_texture *build_rounded(int r)
{
    int size = 2 * r + 8;
    vita2d_texture *t = vita2d_create_empty_texture(size, size);
    if (!t)
        return NULL;
    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(t);
    unsigned int stride_b = vita2d_texture_get_stride(t);
    if (!px || stride_b < (unsigned int)size * 4) {
        vita2d_free_texture(t);
        return NULL;
    }
    unsigned int stride = stride_b / 4;

    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            /* Distance past the corner centre. In the straight part of an
               edge both deltas are zero, so alpha stays full. */
            float dx = 0.0f, dy = 0.0f;
            if (x < r)              dx = (r - 0.5f) - x;
            else if (x >= size - r) dx = x - (size - r - 0.5f);
            if (y < r)              dy = (r - 0.5f) - y;
            else if (y >= size - r) dy = y - (size - r - 0.5f);

            float a = r - sqrtf(dx * dx + dy * dy);   /* 1px soft edge */
            if (a > 1.0f) a = 1.0f;
            if (a < 0.0f) a = 0.0f;
            px[y * stride + x] = ((unsigned int)(a * 255.0f) << 24) | 0x00FFFFFFu;
        }
    }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR,
                               SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

static vita2d_texture *build_ramp(void)
{
    vita2d_texture *t = vita2d_create_empty_texture(1, RAMP_H);
    if (!t)
        return NULL;
    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(t);
    unsigned int stride_b = vita2d_texture_get_stride(t);
    if (!px || stride_b < 4) {
        vita2d_free_texture(t);
        return NULL;
    }
    unsigned int stride = stride_b / 4;
    for (int y = 0; y < RAMP_H; y++) {
        /* Opaque at the top, gone by the bottom, curved so the falloff
           reads as light rather than as a linear wedge. */
        float t01 = 1.0f - (float)y / (float)(RAMP_H - 1);
        unsigned int a = (unsigned int)(t01 * t01 * 255.0f);
        px[y * stride] = (a << 24) | 0x00FFFFFFu;
    }
    vita2d_texture_set_filters(t, SCE_GXM_TEXTURE_FILTER_LINEAR,
                               SCE_GXM_TEXTURE_FILTER_LINEAR);
    return t;
}

/* Filled panel with rounded corners, nine-sliced from the chosen radius. */
static void draw_panel_r(int x, int y, int w, int h, int cls, unsigned int color)
{
    if (cls < 0) cls = 0;
    if (cls > 2) cls = 2;
    const int c = round_r[cls], S = 2 * c + 8, e = S - 2 * c;
    if (!tex_round[cls] || w < 2 * c || h < 2 * c) {
        vita2d_draw_rectangle((float)x, (float)y, (float)w, (float)h, color);
        return;
    }
    vita2d_texture *t = tex_round[cls];
    float sx = (float)(w - 2 * c) / (float)e;
    float sy = (float)(h - 2 * c) / (float)e;

    vita2d_draw_texture_tint_part_scale(t, x, y, 0, 0, c, c, 1, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x + w - c, y,
                                        S - c, 0, c, c, 1, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x, y + h - c,
                                        0, S - c, c, c, 1, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x + w - c, y + h - c,
                                        S - c, S - c, c, c, 1, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x + c, y, c, 0, e, c, sx, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x + c, y + h - c,
                                        c, S - c, e, c, sx, 1, color);
    vita2d_draw_texture_tint_part_scale(t, x, y + c, 0, c, c, e, 1, sy, color);
    vita2d_draw_texture_tint_part_scale(t, x + w - c, y + c,
                                        S - c, c, c, e, 1, sy, color);
    vita2d_draw_rectangle((float)(x + c), (float)(y + c),
                          (float)(w - 2 * c), (float)(h - 2 * c), color);
}

static void draw_panel(int x, int y, int w, int h, unsigned int color)
{
    draw_panel_r(x, y, w, h, R_MD, color);
}

/* Colour fading downward to nothing: shadows under edges, gloss on panels. */
static void draw_fade(int x, int y, int w, int h, unsigned int color)
{
    if (!tex_ramp || w <= 0 || h <= 0)
        return;
    vita2d_draw_texture_tint_part_scale(tex_ramp, (float)x, (float)y,
                                        0, 0, 1, RAMP_H,
                                        (float)w, (float)h / (float)RAMP_H,
                                        color);
}

/* ---- animation ----

   Presentation only, so it lives here rather than in app_state: every value
   eases toward its target once per frame. Console interfaces move; a
   selection that teleports is the clearest tell that something was drawn
   rather than designed. */
static float an_sel_y = 0.0f, an_sel_h = 0.0f;
static int   an_sel_valid = 0;
static float an_rail_w = 0.0f, an_members_w = 0.0f;
static float an_focus[3] = { 0.0f, 0.0f, 0.0f };
static float an_grid_y = 0.0f;                  /* server grid scroll */
static float an_grid_sel_x = -1.0f;             /* selected card, slides */
static float an_grid_sel_y = -10000.0f;
static float an_voice_pulse = 0.0f;             /* header chip, while talking */
static float an_skeleton = 0.0f;                /* loading pulse, radians */

/* For pixel-scale values: a third of a pixel is close enough to settle. */
static float ease(float cur, float target, float k)
{
    float next = cur + (target - cur) * k;
    if (fabsf(target - next) < 0.35f)
        next = target;              /* settle instead of creeping forever */
    return next;
}

/* For 0..1 values. The pixel threshold above is a third of the WHOLE range
   here, so a fade using it snapped after four frames instead of easing;
   that is why focus changes popped rather than blended. */
static float ease01(float cur, float target, float k)
{
    float next = cur + (target - cur) * k;
    if (fabsf(target - next) < 0.004f)
        next = target;
    return next;
}

/* One card: shadow under it, an accent ring that fades in with focus, the
   body, and a hairline of light along the top edge. That hairline is the
   cheapest thing that makes a surface read as raised in a dark theme. */
static void draw_card(int x, int y, int w, int h, unsigned int body, float focus)
{
    draw_fade(x + 6, y + h, w - 12, 9, RGBA8(0, 0, 0, 120));
    if (focus > 0.01f) {
        unsigned int ring = (COLOR_ACCENT & 0x00FFFFFFu) |
                            ((unsigned int)(focus * 255.0f) << 24);
        draw_panel_r(x - 2, y - 2, w + 4, h + 4, R_MD, ring);
    }
    draw_panel_r(x, y, w, h, R_MD, body);
    vita2d_draw_rectangle(x + round_r[R_MD], y, w - 2 * round_r[R_MD], 1,
                          COLOR_RING);
}

/* Blend two colours, t in 0..1. Used to light a pane up as it takes focus. */
static unsigned int mix_color(unsigned int a, unsigned int b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    unsigned int out = 0;
    for (int i = 0; i < 4; i++) {
        int shift = i * 8;
        int ca = (a >> shift) & 0xFF, cb = (b >> shift) & 0xFF;
        out |= (unsigned int)(ca + (int)((cb - ca) * t)) << shift;
    }
    return out;
}

int ui_hit_image(int x, int y, char *url_out, int out_size)
{
    for (int i = 0; i < hit_count; i++) {
        if (x >= hits[i].x && x < hits[i].x + hits[i].w &&
            y >= hits[i].y && y < hits[i].y + hits[i].h) {
            snprintf(url_out, out_size, "%s", hits[i].url);
            return 1;
        }
    }
    return 0;
}

/* vita2d_load_font_file "succeeds" even when the file doesn't exist: the
   face is only opened lazily at first draw, which then data-aborts inside
   FreeType (hardware dump: generic_font_draw_text via ui_draw_status).
   So probe the file ourselves: it must exist and start with a real
   TTF/OTF magic before vita2d gets to see it. */
static vita2d_font *try_load_font(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    unsigned char magic[4] = {0};
    size_t n = fread(magic, 1, 4, f);
    fclose(f);
    int ok = n == 4 &&
        ((magic[0] == 0x00 && magic[1] == 0x01 &&
          magic[2] == 0x00 && magic[3] == 0x00) ||       /* TrueType */
         memcmp(magic, "OTTO", 4) == 0 ||                /* CFF/OTF  */
         memcmp(magic, "true", 4) == 0 ||
         memcmp(magic, "ttcf", 4) == 0);
    return ok ? vita2d_load_font_file(path) : NULL;
}

/* One frame of the boot splash. Rasterizing the five TTF atlases is the
   real startup cost (seconds of FreeType work), and it used to happen
   before the very first frame was ever presented: that was the frozen
   dark screen at launch, and why no loading animation ever showed. This
   draws with things that exist before the atlases do: shapes, the two
   material textures (built in microseconds) and system PGF text. */
static void splash_frame(int step, int total)
{
    vita2d_start_drawing();
    vita2d_clear_screen();

    /* The LiveArea motif in palette colours: a sun on the horizon. */
    vita2d_draw_fill_circle(480.0f, 296.0f, 44.0f, COLOR_ACCENT);
    vita2d_draw_rectangle(0, 296, SCREEN_W, SCREEN_H - 296, COLOR_RAIL);
    /* Its light bleeding down the ground. */
    draw_fade(0, 296, SCREEN_W, 70, RGBA8(88, 101, 242, 45));

    int w = ui_text_width(24, "DawnCord");
    ui_text((SCREEN_W - w) / 2, 372, COLOR_WHITE, 24, "DawnCord");

    vita2d_draw_rectangle(380, 400, 200, 6, COLOR_CARD);
    if (total > 0)
        vita2d_draw_rectangle(380, 400, 200 * step / total, 6, COLOR_ACCENT);

    vita2d_end_drawing();
    vita2d_swap_buffers();
}

void ui_init(void)
{
    vita2d_init();
    vita2d_set_clear_color(COLOR_FRAME);

    for (int i = 0; i < 3; i++)
        tex_round[i] = build_rounded(round_r[i]);
    tex_ramp = build_ramp();

    /* System PGF first: it loads fast and gives the splash a voice while
       the TTF atlases rasterize. */
    font = vita2d_load_default_pgf();

    /* Font: the user's override first, then the Inter bundled in the VPK,
       and system PGF only if neither loads. One handle per size. */
    const char *font_path = "ux0:data/dawncord/font.ttf";
    {
        FILE *probe = fopen(font_path, "rb");
        if (probe)
            fclose(probe);
        else
            font_path = "app0:font.ttf";
    }

    /* The charset people actually type. The width call fills each atlas
       without drawing, so it is safe outside a frame, and no glyph upload
       ever happens mid-frame later. One size per splash frame, so the
       progress bar advances while the real work happens. */
    static const char warm[] =
        " !\"#$%&'()*+,-./0123456789:;<=>?@"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
        "abcdefghijklmnopqrstuvwxyz{|}~"
        "àèéìíòóùú"
        "ÀÈÉÌÒÙ«»…€";

    /* ttf_ok must stay 0 until every handle exists: the splash frames in
       between draw text through ui_text, and flipping it early would route
       a 24px string to a handle that is not loaded yet. */
    splash_frame(0, FONT_SIZES);
    int all_ok = 1;
    for (int i = 0; i < FONT_SIZES; i++) {
        ttf_h[i] = try_load_font(font_path);
        if (ttf_h[i])
            vita2d_font_text_width(ttf_h[i], font_px[i], warm);
        else
            all_ok = 0;
        splash_frame(i + 1, FONT_SIZES);
    }
    ttf_ok = all_ok;

    /* Required before any common dialog (IME keyboard) is opened: without
       it the first dialog composites over an unconfigured state and can
       take the GPU down with it. */
    SceCommonDialogConfigParam dlg_cfg;
    sceCommonDialogConfigParamInit(&dlg_cfg);
    sceCommonDialogSetConfigParam(&dlg_cfg);
}

void ui_term(void)
{
    img_clear();
    for (int i = 0; i < FONT_SIZES; i++) {
        if (ttf_h[i]) {
            vita2d_free_font(ttf_h[i]);
            ttf_h[i] = NULL;
        }
    }
    if (font)
        vita2d_free_pgf(font);
    for (int i = 0; i < 3; i++) {
        if (tex_round[i]) {
            vita2d_free_texture(tex_round[i]);
            tex_round[i] = NULL;
        }
    }
    if (tex_ramp) {
        vita2d_free_texture(tex_ramp);
        tex_ramp = NULL;
    }
    vita2d_fini();
}

static void frame_begin(void)
{
    vita2d_start_drawing();
    vita2d_clear_screen();
}

static void frame_end(void)
{
    vita2d_end_drawing();
    /* Lets common dialogs (IME keyboard) composite over our frame. */
    vita2d_common_dialog_update();
    vita2d_swap_buffers();
}

void ui_draw_status(const char *text)
{
    frame_begin();
    int w = ui_text_width(20, text);
    ui_text((SCREEN_W - w) / 2, SCREEN_H / 2,
                         COLOR_TEXT, 20, text);
    frame_end();
}

void ui_draw_loading(const char *title, const char *subtitle, int frame)
{
    /* Eight dots on a circle; the bright "head" rotates, the rest trail
       off, so it reads as a spinner without any trig at draw time. */
    static const float cx8[8] = { 1.0f, 0.707f, 0.0f, -0.707f,
                                  -1.0f, -0.707f, 0.0f, 0.707f };
    static const float cy8[8] = { 0.0f, 0.707f, 1.0f, 0.707f,
                                  0.0f, -0.707f, -1.0f, -0.707f };
    float ccx = SCREEN_W / 2.0f, ccy = SCREEN_H / 2.0f - 34;

    frame_begin();
    int lead = (frame / 4) % 8;
    for (int i = 0; i < 8; i++) {
        int dist = (lead - i + 8) % 8;      /* 0 = the bright head */
        int a = 32 + (7 - dist) * 30;
        if (a > 255) a = 255;
        vita2d_draw_fill_circle(ccx + cx8[i] * 26.0f, ccy + cy8[i] * 26.0f,
                                4.5f, RGBA8(88, 101, 242, a));
    }
    if (title && title[0]) {
        int w = ui_text_width(20, title);
        ui_text((SCREEN_W - w) / 2, (int)ccy + 62, COLOR_TEXT, 20, title);
    }
    if (subtitle && subtitle[0]) {
        int w = ui_text_width(15, subtitle);
        ui_text((SCREEN_W - w) / 2, (int)ccy + 90, COLOR_TEXT_DIM, 15, subtitle);
    }
    frame_end();
}

/* ---- avatars & icons ---- */

/* Stable per-name color, used for placeholder tiles and author names
   (Discord's legacy default-avatar palette). */
static unsigned int name_color(const char *name)
{
    static const unsigned int palette[5] = {
        RGBA8(88, 101, 242, 255),   /* blurple */
        RGBA8(87, 242, 135, 255),   /* green */
        RGBA8(254, 231, 92, 255),   /* yellow */
        RGBA8(235, 69, 158, 255),   /* fuchsia */
        RGBA8(237, 66, 69, 255),    /* red */
    };
    unsigned int h = 0;
    for (const char *p = name; *p; p++)
        h = h * 31 + (unsigned char)*p;
    return palette[h % 5];
}

/* Draw the image at url as a size x size tile; while it loads (or if there
   is none) draw a colored tile with the name's first letter. */
static void draw_avatar(int x, int y, int size, const char *url, const char *name)
{
    struct vita2d_texture *tex = url[0] ? img_get(url) : NULL;
    if (tex) {
        float sw = (float)size / (float)vita2d_texture_get_width((vita2d_texture *)tex);
        float sh = (float)size / (float)vita2d_texture_get_height((vita2d_texture *)tex);
        vita2d_draw_texture_scale((vita2d_texture *)tex, (float)x, (float)y, sw, sh);
        return;
    }

    /* Round placeholder: loaded avatars come back circle-masked from the
       image cache, so the placeholder matches. */
    vita2d_draw_fill_circle(x + size / 2.0f, y + size / 2.0f, size / 2.0f,
                            name_color(name));
    char initial[2] = { name[0] ? name[0] : '?', '\0' };
    int px = size >= AVATAR_SZ ? 17 : 14;
    int w = ui_text_width(px, initial);
    ui_text(x + (size - w) / 2, y + size / 2 + px * 2 / 5,
            RGBA8(30, 31, 34, 255), px, initial);
}

static unsigned int presence_color(st_presence s)
{
    switch (s) {
    case ST_STATUS_ONLINE: return RGBA8(35, 165, 90, 255);
    case ST_STATUS_IDLE:   return RGBA8(240, 178, 50, 255);
    case ST_STATUS_DND:    return RGBA8(242, 63, 67, 255);
    default:               return RGBA8(128, 132, 142, 255);
    }
}

/* ---- shared chrome ---- */

/* Defined further down with the views; the header needs them. */
static void draw_text_clipped(int x, int y, unsigned int color, int scale,
                              const char *text, int max_w);
static void draw_guild_icon(int x, int y, int size, const st_named *g,
                            int label_px);

/* Tiny battery in the header corner, old-VitaCord style. */
static void draw_battery(void)
{
    int pct = scePowerGetBatteryLifePercent();
    if (pct < 0 || pct > 100)
        return;   /* devkit / no battery info */
    int charging = scePowerIsBatteryCharging();

    int x = SCREEN_W - PAD_X - 29, y = 17;
    unsigned int col = charging ? COLOR_ACCENT
                     : pct <= 20 ? COLOR_ERROR
                                 : RGBA8(87, 242, 135, 255);
    vita2d_draw_rectangle(x, y, 26, 14, COLOR_SELECT);        /* shell */
    vita2d_draw_rectangle(x + 26, y + 4, 3, 6, COLOR_SELECT); /* nub */
    int fill = 22 * pct / 100;
    if (fill > 0)
        vita2d_draw_rectangle(x + 2, y + 2, fill, 10, col);

    char t[8];
    snprintf(t, sizeof(t), "%d", pct);
    int tw = ui_text_width(13, t);
    /* Same baseline as the header's right-hand text: aligned, not floating. */
    ui_text(x - 6 - tw, 31, COLOR_TEXT_DIM, 13, t);
}

/* The header carries context rather than a title: who you are or where you
   are on the left, and the standing state of the app on the right. */
static void draw_header_full(const app_state *st, dawncord_view view)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, HEADER_H, COLOR_CHROME);
    draw_fade(0, 0, SCREEN_W, 12, RGBA8(255, 255, 255, 12));
    /* A hard line, then a short fade. A fade alone reads as a blurred
       edge against sharp text. */
    vita2d_draw_rectangle(0, HEADER_H - 1, SCREEN_W, 1, RGBA8(0, 0, 0, 110));
    draw_fade(0, HEADER_H, SCREEN_W, 5, RGBA8(0, 0, 0, 120));

    /* --- right cluster, laid out from the edge inward --- */
    draw_battery();
    int right_x = SCREEN_W - PAD_X - 74;      /* battery occupies the end */

    SceDateTime now;
    if (sceRtcGetCurrentClockLocalTime(&now) >= 0) {
        char clock[8];
        snprintf(clock, sizeof(clock), "%02d:%02d", now.hour, now.minute);
        int w = ui_text_width(16, clock);
        right_x -= w;
        ui_text(right_x, 31, COLOR_TEXT, 16, clock);
        right_x -= 14;
    }

    if (st->voice_id[0]) {
        /* Voice chip: the one place that says voice is live from anywhere
           in the app, including the server list. */
        an_voice_pulse = ease01(an_voice_pulse,
                                st->speaking_count > 0 ? 1.0f : 0.0f, 0.30f);
        unsigned int vc = st->voice_muted ? COLOR_TEXT_MUTE : COLOR_GREEN;
        int w = ui_text_width(16, st->voice_name);
        if (w > 150) w = 150;
        int chip_w = w + 34;
        right_x -= chip_w;
        draw_panel_r(right_x, 9, chip_w, 30, R_SM, COLOR_CARD);
        if (st->voice_muted) {
            /* A crossed-out dot: silenced here, still in the channel. */
            vita2d_draw_fill_circle((float)(right_x + 15), 24.0f, 5.0f, vc);
            for (int i = 0; i < 12; i++)
                vita2d_draw_rectangle(right_x + 9 + i, 30 - i, 2, 2,
                                      COLOR_ERROR);
        } else {
            vita2d_draw_fill_circle((float)(right_x + 15), 24.0f,
                                    4.0f + 2.0f * an_voice_pulse, vc);
        }
        draw_text_clipped(right_x + 26, 30, vc, 16, st->voice_name, w);
        right_x -= 12;
    }

    /* --- left cluster --- */
    if (view == VIEW_GUILD_LIST) {
        if (st->self_name[0]) {
            draw_avatar(PAD_X, 8, 32, st->self_avatar, st->self_name);
            vita2d_draw_fill_circle((float)(PAD_X + 26), 36.0f, 6.0f,
                                    COLOR_CHROME);
            vita2d_draw_fill_circle((float)(PAD_X + 26), 36.0f, 4.0f,
                                    COLOR_GREEN);
            draw_text_clipped(PAD_X + 42, 33, COLOR_WHITE, 22,
                              st->self_name, right_x - PAD_X - 52);
        } else {
            ui_text(PAD_X, 33, COLOR_WHITE, 22, "DawnCord");
        }
        return;
    }

    int x = PAD_X;
    if (st->guild_count > 0 && st->guild_sel < st->guild_count) {
        draw_guild_icon(x, 8, 32, &st->guilds[st->guild_sel], 14);
        x += 42;
    }
    if (st->channel_id[0]) {
        ui_text(x, 33, COLOR_TEXT_MUTE, 22, "#");
        x += ui_text_width(22, "#") + 6;
        draw_text_clipped(x, 33, COLOR_WHITE, 22, st->channel_name,
                          right_x - x - 10);
    } else {
        draw_text_clipped(x, 33, COLOR_WHITE, 22, st->guild_name,
                          right_x - x - 10);
    }
}

static void draw_footer(const char *hints, const char *status)
{
    vita2d_draw_rectangle(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, COLOR_CHROME);
    draw_fade(0, SCREEN_H - FOOTER_H, SCREEN_W, 8, RGBA8(255, 255, 255, 10));
    ui_text(PAD_X, SCREEN_H - 8, COLOR_TEXT_DIM, 14, hints);
    if (status && status[0]) {
        /* Leading '!' marks a warning: drawn in red so it can't be missed. */
        unsigned int color = COLOR_ACCENT;
        if (status[0] == '!') {
            color = COLOR_ERROR;
            status++;
        }
        int w = ui_text_width(14, status);
        ui_text(SCREEN_W - PAD_X - w, SCREEN_H - 8,
                             color, 14, status);
    }
}

/* The outdated-companion warning owns the status slot whenever nothing
   more urgent is being shown. */
static const char *status_line(const app_state *st)
{
    if (st->status[0])
        return st->status;
    if (st->companion_old)
        return "!Companion outdated: update it on the PC";
    return "";
}

/* Draw text truncated at UTF-8 boundaries so it never bleeds out of its
   column. Rails are narrow; an ellipsis would just eat pixels. */
static void draw_text_clipped(int x, int y, unsigned int color, int scale,
                              const char *text, int max_w)
{
    if (ui_text_width(scale, text) <= max_w) {
        ui_text(x, y, color, scale, text);
        return;
    }
    char buf[WRAP_LINE_LEN];
    snprintf(buf, sizeof(buf), "%s", text);
    size_t len = strlen(buf);
    while (len > 0) {
        do {
            len--;
        } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
        buf[len] = '\0';
        if (ui_text_width(scale, buf) <= max_w)
            break;
    }
    ui_text(x, y, color, scale, buf);
}

/* ---- word wrap ---- */

/* Break a UTF-8 string so no chunk is wider than max_w. Greedy on spaces;
   words wider than a whole line are hard-split at UTF-8 boundaries. */
static int wrap_text(const char *text, int scale, int max_w,
                     char out[][WRAP_LINE_LEN], int max_lines)
{
    int nlines = 0;
    char line[WRAP_LINE_LEN] = "";

    const char *p = text;
    while (*p && nlines < max_lines) {
        /* take one token: run of non-space, or a single space/newline */
        if (*p == '\n') {
            snprintf(out[nlines++], WRAP_LINE_LEN, "%s", line);
            line[0] = '\0';
            p++;
            continue;
        }

        const char *tok_end = p;
        if (*p == ' ') {
            tok_end = p + 1;
        } else {
            while (*tok_end && *tok_end != ' ' && *tok_end != '\n')
                tok_end++;
        }
        int tok_len = (int)(tok_end - p);

        char candidate[WRAP_LINE_LEN];
        snprintf(candidate, sizeof(candidate), "%s%.*s", line, tok_len, p);

        if (ui_text_width(scale, candidate) <= max_w) {
            snprintf(line, sizeof(line), "%s", candidate);
            p = tok_end;
            continue;
        }

        if (line[0] != '\0') {
            /* flush current line, retry token on a fresh one */
            snprintf(out[nlines++], WRAP_LINE_LEN, "%s", line);
            line[0] = '\0';
            if (*p == ' ')
                p++;  /* don't start the next line with the wrap space */
            continue;
        }

        /* token alone is too wide: hard-split at a UTF-8 boundary */
        int fit = 0;
        for (int i = 1; i <= tok_len && i < WRAP_LINE_LEN - 1; i++) {
            if ((p[i] & 0xC0) == 0x80)
                continue;  /* mid-sequence: not a valid split point */
            char probe[WRAP_LINE_LEN];
            snprintf(probe, sizeof(probe), "%.*s", i, p);
            if (ui_text_width(scale, probe) > max_w)
                break;
            fit = i;
        }
        if (fit == 0)
            fit = 1;  /* guarantee progress */
        snprintf(out[nlines++], WRAP_LINE_LEN, "%.*s", fit, p);
        p += fit;
    }

    if (nlines < max_lines && (line[0] != '\0' || nlines == 0))
        snprintf(out[nlines++], WRAP_LINE_LEN, "%s", line);
    return nlines;
}

/* ---- views ---- */

/* A guild icon drawn as a rounded square, with a coloured tile carrying
   initials while it loads or when the server simply has none. */
static void draw_guild_icon(int x, int y, int size, const st_named *g,
                            int label_px)
{
    struct vita2d_texture *tex = g->icon[0] ? img_get_icon(g->icon, 128) : NULL;
    if (tex) {
        float s = (float)size /
                  (float)vita2d_texture_get_width((vita2d_texture *)tex);
        vita2d_draw_texture_scale((vita2d_texture *)tex, (float)x, (float)y, s, s);
        return;
    }

    /* The DM pseudo-guild is always icon-less and is always first, so it
       gets the brand colour and a real label instead of looking broken. */
    int is_dm = g->id[0] == '0' && g->id[1] == '\0';
    draw_panel_r(x, y, size, size, R_MD,
                 is_dm ? COLOR_ACCENT : name_color(g->name));

    char initials[3] = { 0, 0, 0 };
    if (is_dm) {
        initials[0] = 'D';
        initials[1] = 'M';
    } else {
        int n = 0;
        for (const char *p = g->name; *p && n < 2; p++) {
            if ((unsigned char)*p <= ' ')
                continue;
            if (n == 0 || p[-1] == ' ') {
                char c = *p;
                if (c >= 'a' && c <= 'z')
                    c = (char)(c - 'a' + 'A');
                initials[n++] = c;
            }
        }
        if (n == 0)
            initials[0] = '?';
    }
    int w = ui_text_width(label_px, initials);
    ui_text(x + (size - w) / 2, y + size / 2 + label_px * 2 / 5,
            COLOR_WHITE, label_px, initials);
}

/* The first screen. A grid of cards rather than a list of strips: the
   servers are places, and places deserve a face. */
#define GRID_COLS   GUILD_GRID_COLS
#define GRID_CARD_W 220
#define GRID_CARD_H 138
#define GRID_PITCH  150
#define GRID_TOP    60

static void render_guilds(const app_state *st)
{
    draw_header_full(st, VIEW_GUILD_LIST);

    int view_top = HEADER_H + 8, view_bot = SCREEN_H - FOOTER_H - 8;
    int sel_row = st->guild_count ? st->guild_sel / GRID_COLS : 0;

    /* Keep the selected row inside the window, scrolling in whole rows. */
    int rows = (st->guild_count + GRID_COLS - 1) / GRID_COLS;
    int vis_rows = (view_bot - view_top) / GRID_PITCH;
    if (vis_rows < 1) vis_rows = 1;
    int first_row = sel_row - vis_rows / 2;
    if (first_row > rows - vis_rows) first_row = rows - vis_rows;
    if (first_row < 0) first_row = 0;
    an_grid_y = ease(an_grid_y, (float)(first_row * GRID_PITCH), 0.24f);

    vita2d_set_clip_rectangle(0, view_top, SCREEN_W, view_bot);
    vita2d_enable_clipping();

    for (int i = 0; i < st->guild_count; i++) {
        int col = i % GRID_COLS, row = i / GRID_COLS;
        int x = 16 + col * (GRID_CARD_W + 16);
        int y = GRID_TOP + row * GRID_PITCH - (int)an_grid_y;
        if (y > view_bot || y + GRID_CARD_H < view_top)
            continue;

        const st_named *g = &st->guilds[i];
        int is_sel = i == st->guild_sel;
        if (is_sel) {
            an_grid_sel_x = an_grid_sel_x < 0 ? (float)x
                                              : ease(an_grid_sel_x, (float)x, 0.30f);
            an_grid_sel_y = an_grid_sel_y < -9000.0f ? (float)y
                                              : ease(an_grid_sel_y, (float)y, 0.30f);
            int sx = (int)an_grid_sel_x, sy = (int)an_grid_sel_y;
            draw_fade(sx + 8, sy + GRID_CARD_H, GRID_CARD_W - 16, 8,
                      RGBA8(0, 0, 0, 110));
            draw_panel_r(sx - 2, sy - 2, GRID_CARD_W + 4, GRID_CARD_H + 4,
                         R_MD, COLOR_ACCENT);
            draw_panel_r(sx, sy, GRID_CARD_W, GRID_CARD_H, R_MD, COLOR_RAIL_HI);
        } else {
            draw_panel_r(x, y, GRID_CARD_W, GRID_CARD_H, R_MD, COLOR_RAIL);
        }

        draw_guild_icon(x + (GRID_CARD_W - 88) / 2, y + 14, 88, g, 24);
        int tw = ui_text_width(19, g->name);
        int tx = x + (GRID_CARD_W - tw) / 2;
        if (tw > GRID_CARD_W - 24)
            tx = x + 12;
        draw_text_clipped(tx, y + 124, is_sel ? COLOR_WHITE : COLOR_TEXT,
                          19, g->name, GRID_CARD_W - 24);
    }

    /* Nothing yet: skeleton cards breathing in place of the real ones, so
       the first seconds after launch look like loading rather than like an
       empty screen that might be broken. */
    if (st->guild_count == 0) {
        an_skeleton += 0.045f;
        if (an_skeleton > 6.2831853f)
            an_skeleton -= 6.2831853f;
        for (int i = 0; i < 8; i++) {
            int col = i % GRID_COLS, row = i / GRID_COLS;
            int x = 16 + col * (GRID_CARD_W + 16);
            int y = GRID_TOP + row * GRID_PITCH;
            /* Each card lags the one before it, so the pulse travels. */
            float ph = an_skeleton - i * 0.35f;
            float t = (sinf(ph) + 1.0f) * 0.5f;
            unsigned int c = mix_color(COLOR_RAIL, COLOR_RAIL_HI, 0.35f + 0.65f * t);
            draw_panel_r(x, y, GRID_CARD_W, GRID_CARD_H, R_MD, c);
            draw_panel_r(x + (GRID_CARD_W - 88) / 2, y + 14, 88, 88, R_MD,
                         mix_color(COLOR_RAIL_HI, COLOR_CARD, 0.35f + 0.65f * t));
            draw_panel_r(x + 50, y + 112, GRID_CARD_W - 100, 14, R_SM,
                         mix_color(COLOR_RAIL_HI, COLOR_CARD, 0.35f + 0.65f * t));
        }
    }

    vita2d_disable_clipping();

    draw_footer("X open    D-pad move    SELECT quit", status_line(st));
}

/* ---- workspace: channels | chat | members ---- */

static void render_channel_rail(const app_state *st)
{
    int cx = GUTTER, top = CARD_TOP, bottom = CARD_BOT;
    float f = an_focus[FOCUS_CHANNELS];
    draw_card(cx, top, rail_w, CARD_H,
              mix_color(COLOR_RAIL, COLOR_RAIL_HI, f), f);

    /* Content is clipped to the card so nothing bleeds while it resizes. */
    vita2d_set_clip_rectangle(cx, top, cx + rail_w, bottom);
    vita2d_enable_clipping();

    /* Profile card pinned at the bottom: who you are. */
    if (st->self_name[0]) {
        int py = bottom - PROFILE_H;
        draw_panel_r(cx + 8, py, rail_w - 16, PROFILE_H - 8, R_SM, COLOR_CARD);
        draw_avatar(cx + 16, py + 8, 34, st->self_avatar, st->self_name);
        draw_text_clipped(cx + 58, py + 22, COLOR_WHITE, 16,
                          st->self_name, rail_w - 74);
        vita2d_draw_fill_circle((float)(cx + 63), (float)(py + 34), 3.5f,
                                COLOR_GREEN);
        ui_text(cx + 72, py + 39, COLOR_TEXT_DIM, 13, "online");
        bottom = py - 8;
    }

    int is_dm = st->guild_id[0] == '0' && st->guild_id[1] == '\0';
    int visible = (bottom - top - 8) / RAIL_ROW_H;
    int first = st->channel_sel - visible / 2;
    if (first > st->channel_count - visible)
        first = st->channel_count - visible;
    if (first < 0)
        first = 0;

    for (int i = 0; i < visible && first + i < st->channel_count; i++) {
        int idx = first + i;
        int y = top + 6 + i * RAIL_ROW_H;
        const st_named *c = &st->channels[idx];

        if (strncmp(c->id, "cat:", 4) == 0) {
            draw_text_clipped(cx + 14, y + 21, COLOR_TEXT_DIM, 13,
                              c->name, rail_w - 26);
            continue;
        }
        /* "vu:" rows: one connected user each, indented under their voice
           channel with a little presence dot, like Discord's sidebar. */
        if (strncmp(c->id, "vu:", 3) == 0) {
            /* The dot swells and brightens while that person is actually
               talking, which is the only cue you get that voice is alive. */
            int talking = state_is_speaking(st, c->name);
            vita2d_draw_fill_circle((float)(cx + 40), (float)(y + 15),
                                    talking ? 5.5f : 3.0f,
                                    talking ? RGBA8(59, 226, 130, 255)
                                            : RGBA8(90, 96, 106, 255));
            draw_text_clipped(cx + 52, y + 20,
                              talking ? COLOR_TEXT : COLOR_TEXT_DIM, 15,
                              c->name, rail_w - 64);
            continue;
        }

        int is_open = st->channel_id[0] &&
                      strcmp(c->id, st->channel_id) == 0;
        int in_voice = c->is_voice && st->voice_id[0] &&
                       strcmp(c->id, st->voice_id) == 0;
        /* The selected row is drawn once, outside this loop, so it can
           slide between rows instead of jumping. */
        if (idx == st->channel_sel) {
            an_sel_y = an_sel_valid ? ease(an_sel_y, (float)y, 0.35f) : (float)y;
            an_sel_h = an_sel_valid ? ease(an_sel_h, (float)RAIL_ROW_H, 0.35f)
                                    : (float)RAIL_ROW_H;
            an_sel_valid = 1;
            draw_panel_r(cx + 6, (int)an_sel_y, rail_w - 12, (int)an_sel_h,
                         R_SM, mix_color(COLOR_HEADER, COLOR_ROW_SEL, f));
            if (f > 0.02f)
                vita2d_draw_rectangle(cx + 6, (int)an_sel_y + 6, 3,
                                      (int)an_sel_h - 12, COLOR_ACCENT);
        }
        /* The open channel (or the joined voice one) keeps its accent tick
           even when the cursor wanders, like Discord's sidebar. */
        if (is_open || in_voice)
            vita2d_draw_rectangle(cx, y + 7, 4, RAIL_ROW_H - 14, COLOR_WHITE);

        int tx = cx + 14;
        if (c->is_voice) {
            /* Little speaker glyph so voice channels read at a glance. */
            unsigned int vc = in_voice ? RGBA8(35, 165, 90, 255)
                            : idx == st->channel_sel ? COLOR_TEXT
                                                     : COLOR_TEXT_DIM;
            vita2d_draw_rectangle(cx + 14, y + 11, 4, 8, vc);
            for (int t = 0; t < 6; t++)
                vita2d_draw_rectangle(cx + 18 + t, y + 11 - t, 1, 8 + 2 * t, vc);
            tx = cx + 30;
        }

        char row[ST_NAME_LEN + 4];
        snprintf(row, sizeof(row), "%s%s", c->is_voice ? "" : (is_dm ? "" : "# "),
                 c->name);
        draw_text_clipped(tx, y + 22,
                          (is_open || in_voice) ? COLOR_WHITE
                          : idx == st->channel_sel ? COLOR_TEXT
                                                   : COLOR_TEXT_DIM,
                          RAIL_SCALE, row, rail_w - tx - 14);
    }

    if (st->channel_count == 0)
        ui_text(cx + 14, top + 28, COLOR_TEXT_DIM, 14, "Loading...");

    vita2d_disable_clipping();
}

static void render_member_rail(const app_state *st)
{
    int x = SCREEN_W - GUTTER - members_w;
    int top = CARD_TOP, bottom = CARD_BOT;
    float f = an_focus[FOCUS_MEMBERS];
    draw_card(x, top, members_w, CARD_H,
              mix_color(COLOR_RAIL, COLOR_RAIL_HI, f), f);

    vita2d_set_clip_rectangle(x, top, x + members_w, bottom);
    vita2d_enable_clipping();

    char hdr[32];
    snprintf(hdr, sizeof(hdr), "MEMBERS %d", st->member_count);
    ui_text(x + 14, top + 26, COLOR_TEXT_DIM, 13, hdr);

    int list_top = top + 38;
    int visible = (bottom - list_top - 4) / RAIL_ROW_H;
    int first = st->member_scroll;
    if (first > st->member_count - visible)
        first = st->member_count - visible;
    if (first < 0)
        first = 0;

    for (int i = 0; i < visible && first + i < st->member_count; i++) {
        const st_member *m = &st->members[first + i];
        int y = list_top + i * RAIL_ROW_H;
        unsigned int dot = presence_color(m->status);
        /* Offline names sit back rather than shouting in the same white. */
        int away = m->status == ST_STATUS_OFFLINE;
        vita2d_draw_fill_circle((float)(x + 20), (float)(y + RAIL_ROW_H / 2 + 2),
                                4.5f, dot);
        draw_text_clipped(x + 34, y + 22,
                          away ? COLOR_TEXT_DIM : COLOR_TEXT, RAIL_SCALE,
                          m->name, members_w - 46);
    }

    if (st->channel_id[0] && st->member_count == 0)
        ui_text(x + 14, list_top + 20, COLOR_TEXT_DIM, 14, "Nobody visible");

    vita2d_disable_clipping();
}

static void render_chat_pane(const app_state *st)
{
    /* The member rail only exists while a channel is open: browsing, the
       chat pane takes the full width (author's call: three columns at all
       times crowd a 5-inch screen). */
    int x0 = GUTTER + rail_w + GUTTER;
    int x1 = st->channel_id[0] ? SCREEN_W - GUTTER - members_w - GUTTER
                               : SCREEN_W - GUTTER;
    hit_count = 0;
    draw_card(x0, CARD_TOP, x1 - x0, CARD_H, COLOR_BG, an_focus[FOCUS_CHAT]);
    vita2d_set_clip_rectangle(x0, CARD_TOP, x1, CARD_BOT);
    vita2d_enable_clipping();

    if (st->channel_id[0] == '\0') {
        ui_text(x0 + 24, CARD_TOP + 48, COLOR_TEXT_DIM, 19,
                "Pick a channel on the left,");
        ui_text(x0 + 24, CARD_TOP + 48 + LINE_H + 4, COLOR_TEXT_DIM, 19,
                "X opens it here.");
        vita2d_disable_clipping();
        return;
    }

    int avatar_x = x0 + PAD_X;
    int text_x = avatar_x + AVATAR_SZ + 10;
    int input_top = CARD_BOT - 44;
    int text_w = x1 - text_x - PAD_X;

    /* Scroll-back feedback pinned under the title bar. */
    if (st->history_pending) {
        const char *msg = "Loading older messages...";
        int w = ui_text_width(15, msg);
        ui_text(x0 + (x1 - x0 - w) / 2, CARD_TOP + 14,
                             COLOR_ACCENT, 15, msg);
    } else if (st->history_done && st->message_count > 0 &&
               st->chat_scroll >= st->message_count - 1) {
        /* Honest label: a fresh channel top is one thing, a companion too
           old to serve scroll-back is another. */
        const char *msg = st->companion_old
            ? "Companion outdated: can't load older messages"
            : "Beginning of history";
        int w = ui_text_width(15, msg);
        ui_text(x0 + (x1 - x0 - w) / 2, CARD_TOP + 14,
                st->companion_old ? COLOR_ERROR : COLOR_TEXT_DIM, 15, msg);
    }

    int typing = st->typing_ttl > 0 && st->typing_name[0];

    /* Messages, newest at the bottom, drawn upward until we run out of
       room. Consecutive messages by the same author group under one
       avatar + name header, Discord style. */
    static char lines[WRAP_MAX_LINES][WRAP_LINE_LEN];
    static char elines[ST_MAX_EMBEDS][3][WRAP_LINE_LEN];
    int y_bottom = input_top - 8 - (typing ? 18 : 0);

    int newest = st->message_count - 1 - st->chat_scroll;
    for (int i = newest; i >= 0 && y_bottom > CARD_TOP + LINE_H; i--) {
        const st_message *m = &st->messages[i];
        int starts_group =
            (i == 0 || strcmp(st->messages[i - 1].author, m->author) != 0);

        int nlines = m->content[0]
            ? wrap_text(m->content, CHAT_SCALE, text_w, lines, WRAP_MAX_LINES)
            : 0;

        /* Inline image attachment: thumbnail under the text, tappable. */
        int thumb_w = 0, thumb_h = 0;
        struct vita2d_texture *thumb = NULL;
        if (m->image[0]) {
            thumb = img_get_sized(m->image, 160);
            if (thumb) {
                int tw = vita2d_texture_get_width((vita2d_texture *)thumb);
                int th = vita2d_texture_get_height((vita2d_texture *)thumb);
                float s = 1.0f;
                if (tw * s > THUMB_MAX_W) s = (float)THUMB_MAX_W / tw;
                if (th * s > THUMB_MAX_H) s = (float)THUMB_MAX_H / th;
                thumb_w = (int)(tw * s);
                thumb_h = (int)(th * s);
            } else {
                thumb_w = 160;   /* placeholder while it loads */
                thumb_h = 90;
            }
        }

        /* Discord-style embed boxes: color bar + title + short description.
           Measured first so the block height is right. */
        int en[ST_MAX_EMBEDS] = {0, 0}, eh[ST_MAX_EMBEDS] = {0, 0};
        int embeds_h = 0;
        int emb_w = text_w < 420 ? text_w : 420;
        for (int e = 0; e < m->embed_count; e++) {
            const st_embed *em = &m->embeds[e];
            en[e] = em->desc[0]
                ? wrap_text(em->desc, 16, emb_w - 26, elines[e], 3) : 0;
            eh[e] = 8 + (em->title[0] ? LINE_H : 0) + en[e] * LINE_H + 6;
            embeds_h += eh[e] + 6;
        }

        int block_h = nlines * LINE_H + (starts_group ? LINE_H + 10 : 4)
                    + embeds_h
                    + (m->image[0] ? thumb_h + 8 : 0);
        int y = y_bottom - block_h;
        int content_top = y + (starts_group ? LINE_H + 6 : 2);

        /* Topmost message may only partially fit: draw the author header
           only when it clears the title bar, otherwise an orphan name shows
           up under the header with its text swallowed (hardware screenshot,
           v0.17 era). Content lines have their own per-line guard below. */
        if (starts_group && y + 4 >= CARD_TOP) {
            draw_avatar(avatar_x, y + 4, AVATAR_SZ, m->avatar, m->author);
            /* Avatars are tappable too: they open full-size in the viewer. */
            if (m->avatar[0] && hit_count < HIT_MAX) {
                hit_rect *hr = &hits[hit_count++];
                hr->x = avatar_x;
                hr->y = y + 4;
                hr->w = AVATAR_SZ;
                hr->h = AVATAR_SZ;
                snprintf(hr->url, sizeof(hr->url), "%s", m->avatar);
            }
            ui_text(text_x, y + LINE_H - 2,
                                 name_color(m->author), CHAT_SCALE, m->author);
            if (m->time[0]) {
                int aw = ui_text_width(CHAT_SCALE, m->author);
                ui_text(text_x + aw + 10, y + LINE_H - 2,
                                     COLOR_TEXT_DIM, 14, m->time);
            }
        }
        for (int l = 0; l < nlines; l++) {
            int ly = content_top + (l + 1) * LINE_H - 4;
            if (ly > CARD_TOP + LINE_H && ly < input_top)
                ui_text(text_x, ly, COLOR_TEXT,
                                     CHAT_SCALE, lines[l]);
        }

        int below_y = content_top + nlines * LINE_H + 4;
        for (int e = 0; e < m->embed_count; e++) {
            const st_embed *em = &m->embeds[e];
            if (below_y >= CARD_TOP + 4 && below_y + eh[e] < input_top) {
                unsigned int bar = em->color
                    ? RGBA8((em->color >> 16) & 255, (em->color >> 8) & 255,
                            em->color & 255, 255)
                    : COLOR_ACCENT;
                draw_panel(text_x, below_y, emb_w, eh[e], COLOR_CARD);
                vita2d_draw_rectangle(text_x + 2, below_y + 6, 3,
                                      eh[e] - 12, bar);
                int ty = below_y + LINE_H;
                if (em->title[0]) {
                    draw_text_clipped(text_x + 14, ty, COLOR_WHITE, 17,
                                      em->title, emb_w - 26);
                    ty += LINE_H;
                }
                for (int l = 0; l < en[e]; l++) {
                    ui_text(text_x + 14, ty, COLOR_TEXT, 16,
                            elines[e][l]);
                    ty += LINE_H;
                }
            }
            below_y += eh[e] + 6;
        }

        if (m->image[0]) {
            int iy = below_y;
            if (iy >= CARD_TOP + 4 && iy + thumb_h < input_top) {
                /* Soft drop shadow instead of a hard outline: the picture
                   sits on the conversation rather than being framed by it. */
                draw_fade(text_x + 3, iy + thumb_h, thumb_w - 6, 7,
                          RGBA8(0, 0, 0, 90));
                if (thumb) {
                    int tw = vita2d_texture_get_width((vita2d_texture *)thumb);
                    float s = (float)thumb_w / (float)tw;
                    vita2d_draw_texture_scale((vita2d_texture *)thumb,
                                              (float)text_x, (float)iy, s, s);
                    if (m->video) {
                        /* Play mark: honest about being a video preview,
                           equally honest about not being clickable. */
                        float pcx = text_x + thumb_w / 2.0f;
                        float pcy = iy + thumb_h / 2.0f;
                        vita2d_draw_fill_circle(pcx, pcy, 17.0f,
                                                RGBA8(18, 18, 24, 210));
                        for (int t = 0; t < 13; t++) {
                            int hh = 18 - t * 18 / 13;
                            vita2d_draw_rectangle(pcx - 5 + t,
                                                  pcy - hh / 2.0f,
                                                  1, hh, COLOR_WHITE);
                        }
                    }
                } else {
                    draw_panel(text_x, iy, thumb_w, thumb_h, COLOR_CARD);
                    ui_text(text_x + 12, iy + thumb_h / 2 + 6,
                            COLOR_TEXT_DIM, 14, "image...");
                }
                if (hit_count < HIT_MAX) {
                    hit_rect *hr = &hits[hit_count++];
                    hr->x = text_x;
                    hr->y = iy;
                    hr->w = thumb_w;
                    hr->h = thumb_h;
                    snprintf(hr->url, sizeof(hr->url), "%s", m->image);
                }
            }
        }
        y_bottom = y;
    }

    if (st->message_count == 0)
        ui_text(x0 + PAD_X, CARD_TOP + 30, COLOR_TEXT_DIM, 17,
                "No messages loaded.");

    if (typing) {
        char tline[ST_AUTHOR_LEN + 24];
        snprintf(tline, sizeof(tline), "%s is typing...", st->typing_name);
        ui_text(x0 + PAD_X + 4, input_top - 6,
                             COLOR_TEXT_DIM, 13, tline);
    }

    /* Input bar: a rounded field lifted off the background, with the chat
       fading out behind it instead of ending on a hard line. */
    draw_fade(x0, input_top - 14, x1 - x0, 14, RGBA8(0, 0, 0, 55));
    draw_panel(x0 + 10, input_top, x1 - x0 - 20, 34, COLOR_INPUT_BG);
    if (st->chat_scroll > 0) {
        char more[48];
        snprintf(more, sizeof(more), "v %d newer below - DOWN to return",
                 st->chat_scroll);
        ui_text(x0 + PAD_X + 4, input_top + 22, COLOR_ACCENT, 16, more);
    } else {
        ui_text(x0 + PAD_X + 4, input_top + 22,
                             COLOR_TEXT_DIM, 16, "START to write");
    }

    vita2d_disable_clipping();
}

static const char *workspace_hints(const app_state *st)
{
    if (st->voice_id[0])
        return "SQUARE mute    X leave voice    LEFT/RIGHT pane    O servers";
    switch (st->focus) {
    case FOCUS_CHANNELS:
        return "X open    LEFT/RIGHT pane    O servers    SELECT quit";
    case FOCUS_CHAT:
        return "UP/DOWN scroll    START write    TRIANGLE refresh    LEFT/RIGHT pane    O servers";
    default:
        return "UP/DOWN scroll    LEFT/RIGHT pane    O servers    SELECT quit";
    }
}

static void render_workspace(const app_state *st)
{
    /* One frame of animation for the whole workspace: the focused column
       widens, the others give the space back, and the focus highlight
       follows. Everything eases, nothing snaps. */
    int rail_target = st->focus == FOCUS_CHANNELS ? RAIL_W_WIDE : RAIL_W_NARROW;
    int memb_target = st->focus == FOCUS_MEMBERS ? MEMBERS_W_WIDE
                                                 : MEMBERS_W_NARROW;
    if (an_rail_w <= 0.0f) {          /* first frame: start where we are */
        an_rail_w = (float)rail_target;
        an_members_w = (float)memb_target;
    }
    an_rail_w = ease(an_rail_w, (float)rail_target, 0.22f);
    an_members_w = ease(an_members_w, (float)memb_target, 0.22f);
    rail_w = (int)(an_rail_w + 0.5f);
    members_w = (int)(an_members_w + 0.5f);

    for (int i = 0; i < 3; i++)
        an_focus[i] = ease01(an_focus[i], st->focus == i ? 1.0f : 0.0f, 0.18f);

    draw_header_full(st, VIEW_WORKSPACE);

    render_channel_rail(st);
    render_chat_pane(st);
    if (st->channel_id[0])
        render_member_rail(st);

    draw_footer(workspace_hints(st), status_line(st));
}

void ui_render(const app_state *st, dawncord_view view)
{
    frame_begin();
    switch (view) {
    case VIEW_GUILD_LIST: render_guilds(st);    break;
    case VIEW_WORKSPACE:  render_workspace(st); break;
    }

    /* Attachment viewer: image over a dimmed workspace, Discord style. */
    if (view == VIEW_WORKSPACE && st->expanded_image[0]) {
        vita2d_draw_rectangle(0, 0, SCREEN_W, SCREEN_H,
                              RGBA8(10, 10, 14, 215));
        struct vita2d_texture *tex = img_get_sized(st->expanded_image, 512);
        if (tex) {
            int tw = vita2d_texture_get_width((vita2d_texture *)tex);
            int th = vita2d_texture_get_height((vita2d_texture *)tex);
            float s = 1.0f;
            if (tw * s > 860.0f) s = 860.0f / tw;
            if (th * s > 440.0f) s = 440.0f / th;
            vita2d_draw_texture_scale((vita2d_texture *)tex,
                                      (SCREEN_W - tw * s) / 2.0f,
                                      (SCREEN_H - th * s) / 2.0f, s, s);
        } else {
            const char *msg = "Loading image...";
            int w = ui_text_width(20, msg);
            ui_text((SCREEN_W - w) / 2, SCREEN_H / 2,
                                 COLOR_TEXT, 20, msg);
        }
        const char *hint = "O / tap to close";
        int hw = ui_text_width(15, hint);
        ui_text((SCREEN_W - hw) / 2, SCREEN_H - 10,
                             COLOR_TEXT_DIM, 15, hint);
    }
    frame_end();
}
