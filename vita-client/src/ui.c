#include "ui.h"
#include "imgcache.h"

#include <vita2d.h>
#include <psp2/common_dialog.h>
#include <psp2/power.h>
#include <stdio.h>
#include <string.h>

/* vita2d rasterizes each glyph ONCE, at the first size it's drawn at, and
   rescales that bitmap forever after (its atlas is keyed by glyph only).
   Ten different text sizes through one handle = blurry rescales and glyph
   drops everywhere (hardware screenshots: overlapping letters, truncated
   names). So: one font handle PER SIZE, five consolidated sizes, each
   atlas holding pixel-exact glyphs, pre-warmed at init so no atlas is
   ever touched mid-frame. */
#define FONT_SIZES 5
static const int font_px[FONT_SIZES] = { 13, 15, 18, 20, 22 };
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
#define ROW_H       30
#define LINE_H      26
#define FOOTER_H    28
#define CHAT_SCALE  20
#define LIST_SCALE  19
#define PAD_X       14

#define AVATAR_SZ   36

/* Workspace: channels | chat | members. The chat column gets whatever the
   two rails leave (960 - 230 - 190 = 540px). */
#define RAIL_W      230
#define MEMBERS_W   190
#define RAIL_ROW_H  30
#define RAIL_SCALE  18

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

void ui_init(void)
{
    vita2d_init();
    vita2d_set_clear_color(COLOR_BG);

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
    ttf_ok = 1;
    for (int i = 0; i < FONT_SIZES; i++) {
        ttf_h[i] = try_load_font(font_path);
        if (!ttf_h[i])
            ttf_ok = 0;
    }

    /* Pre-warm every atlas with the charset people actually type: the
       width path fills the atlas without drawing, so it's safe outside a
       frame, and no glyph upload ever happens mid-frame again. */
    if (ttf_ok) {
        static const char warm[] =
            " !\"#$%&'()*+,-./0123456789:;<=>?@"
            "ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`"
            "abcdefghijklmnopqrstuvwxyz{|}~"
            "àèéìíòóùú"
            "ÀÈÉÌÒÙ«»…€";
        for (int i = 0; i < FONT_SIZES; i++)
            vita2d_font_text_width(ttf_h[i], font_px[i], warm);
    }

    font = vita2d_load_default_pgf();

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

static void draw_box(int x, int y, int w, int h, unsigned int color)
{
    vita2d_draw_rectangle((float)x, (float)y, (float)w, 2, color);
    vita2d_draw_rectangle((float)x, (float)(y + h - 2), (float)w, 2, color);
    vita2d_draw_rectangle((float)x, (float)y, 2, (float)h, color);
    vita2d_draw_rectangle((float)(x + w - 2), (float)y, 2, (float)h, color);
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

static void draw_header(const char *title, const char *right)
{
    vita2d_draw_rectangle(0, 0, SCREEN_W, HEADER_H, COLOR_HEADER);
    vita2d_draw_rectangle(0, HEADER_H - 1, SCREEN_W, 1, COLOR_BORDER);
    ui_text(PAD_X, 32, COLOR_WHITE, 22, title);
    if (right && right[0]) {
        /* Leave room for the battery widget on the far right. */
        int w = ui_text_width(15, right);
        ui_text(SCREEN_W - PAD_X - 68 - w, 31, COLOR_TEXT_DIM, 15, right);
    }
    draw_battery();
}

static void draw_footer(const char *hints, const char *status)
{
    vita2d_draw_rectangle(0, SCREEN_H - FOOTER_H, SCREEN_W, FOOTER_H, COLOR_HEADER);
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

/* Scrolling list of named entries with a selection bar. Entries with an
   icon URL get a tile; if any entry has one, all rows reserve the column
   so text stays aligned. */
static void draw_list(const st_named *items, int count, int sel, const char *prefix)
{
    int top = HEADER_H + 6;
    int visible = (SCREEN_H - top - FOOTER_H) / ROW_H;

    int has_icons = 0;
    for (int i = 0; i < count; i++) {
        if (items[i].icon[0]) {
            has_icons = 1;
            break;
        }
    }
    int text_x = PAD_X + 6 + (has_icons ? ROW_H : 0);

    int first = sel - visible / 2;
    if (first > count - visible)
        first = count - visible;
    if (first < 0)
        first = 0;

    for (int i = 0; i < visible && first + i < count; i++) {
        int idx = first + i;
        int y = top + i * ROW_H;

        /* Category header rows: dim label, no prefix, no highlight. */
        if (strncmp(items[idx].id, "cat:", 4) == 0) {
            ui_text(PAD_X + 4, y + 22, COLOR_TEXT_DIM,
                                 0.75f, items[idx].name);
            continue;
        }

        if (idx == sel) {
            vita2d_draw_rectangle(6, y, SCREEN_W - 12, ROW_H, COLOR_SELECT);
            vita2d_draw_rectangle(6, y, 4, ROW_H, COLOR_ACCENT);
        }

        if (has_icons)
            draw_avatar(PAD_X + 2, y + 3, ROW_H - 6,
                        items[idx].icon, items[idx].name);

        char row[ST_NAME_LEN + 4];
        snprintf(row, sizeof(row), "%s%s", prefix, items[idx].name);
        ui_text(text_x, y + 22,
                             idx == sel ? COLOR_WHITE : COLOR_TEXT,
                             LIST_SCALE, row);
    }

    if (count == 0)
        ui_text(PAD_X, top + 24, COLOR_TEXT_DIM, 17,
                             "Nothing here yet...");
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

static void render_guilds(const app_state *st)
{
    draw_header("DawnCord", "Servers");
    draw_list(st->guilds, st->guild_count, st->guild_sel, "");
    draw_footer("X select    SELECT quit", status_line(st));
}

/* ---- workspace: channels | chat | members ---- */

static void render_channel_rail(const app_state *st)
{
    int top = HEADER_H, bottom = SCREEN_H - FOOTER_H;
    vita2d_draw_rectangle(0, top, RAIL_W, bottom - top, COLOR_SIDEBAR);

    /* Profile box pinned at the bottom, VitaCord style: who you are. */
    if (st->self_name[0]) {
        int py = bottom - PROFILE_H;
        vita2d_draw_rectangle(0, py, RAIL_W, PROFILE_H, COLOR_HEADER);
        vita2d_draw_rectangle(0, py, RAIL_W, 2, COLOR_BORDER);
        draw_avatar(10, py + 10, 36, st->self_avatar, st->self_name);
        draw_text_clipped(56, py + 24, COLOR_WHITE, 16,
                          st->self_name, RAIL_W - 66);
        vita2d_draw_fill_circle(61.0f, (float)(py + 38), 4.0f,
                                RGBA8(35, 165, 90, 255));
        ui_text(70, py + 43, COLOR_TEXT_DIM, 12,
                             "online");
        bottom = py;
    }

    if (st->focus == FOCUS_CHANNELS)
        vita2d_draw_rectangle(0, top, RAIL_W, 2, COLOR_ACCENT);

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
            draw_text_clipped(10, y + 21, COLOR_TEXT_DIM, 13,
                              c->name, RAIL_W - 20);
            continue;
        }
        /* "vu:" rows: one connected user each, indented under their voice
           channel with a little presence dot, like Discord's sidebar. */
        if (strncmp(c->id, "vu:", 3) == 0) {
            vita2d_draw_fill_circle(38.0f, (float)(y + 15), 3.0f,
                                    RGBA8(35, 165, 90, 255));
            draw_text_clipped(48, y + 20, COLOR_TEXT_DIM, 15,
                              c->name, RAIL_W - 58);
            continue;
        }

        int is_open = st->channel_id[0] &&
                      strcmp(c->id, st->channel_id) == 0;
        int in_voice = c->is_voice && st->voice_id[0] &&
                       strcmp(c->id, st->voice_id) == 0;
        if (idx == st->channel_sel) {
            vita2d_draw_rectangle(4, y, RAIL_W - 8, RAIL_ROW_H,
                                  st->focus == FOCUS_CHANNELS ? COLOR_SELECT
                                                              : COLOR_HEADER);
            if (st->focus == FOCUS_CHANNELS)
                vita2d_draw_rectangle(4, y, 3, RAIL_ROW_H, COLOR_ACCENT);
        }
        /* The open channel (or the joined voice one) keeps its accent tick
           even when the cursor wanders, like Discord's sidebar. */
        if (is_open || in_voice)
            vita2d_draw_rectangle(0, y, 3, RAIL_ROW_H, COLOR_ACCENT);

        int tx = 14;
        if (c->is_voice) {
            /* Little speaker glyph so voice channels read at a glance. */
            unsigned int vc = in_voice ? RGBA8(35, 165, 90, 255)
                            : idx == st->channel_sel ? COLOR_TEXT
                                                     : COLOR_TEXT_DIM;
            vita2d_draw_rectangle(14, y + 11, 4, 8, vc);
            for (int t = 0; t < 6; t++)
                vita2d_draw_rectangle(18 + t, y + 11 - t, 1, 8 + 2 * t, vc);
            tx = 30;
        }

        char row[ST_NAME_LEN + 4];
        snprintf(row, sizeof(row), "%s%s", c->is_voice ? "" : (is_dm ? "" : "# "),
                 c->name);
        draw_text_clipped(tx, y + 22,
                          (is_open || in_voice) ? COLOR_WHITE
                          : idx == st->channel_sel ? COLOR_TEXT
                                                   : COLOR_TEXT_DIM,
                          RAIL_SCALE, row, RAIL_W - tx - 12);
    }

    if (st->channel_count == 0)
        ui_text(12, top + 28, COLOR_TEXT_DIM, 14,
                             "Loading...");
}

static void render_member_rail(const app_state *st)
{
    int x = SCREEN_W - MEMBERS_W;
    int top = HEADER_H, bottom = SCREEN_H - FOOTER_H;
    vita2d_draw_rectangle(x, top, MEMBERS_W, bottom - top, COLOR_SIDEBAR);
    /* Squared, boxed rail (author's request, borrowed from old VitaCord). */
    draw_box(x, top, MEMBERS_W, bottom - top, COLOR_BORDER);
    if (st->focus == FOCUS_MEMBERS)
        vita2d_draw_rectangle(x, top, MEMBERS_W, 2, COLOR_ACCENT);

    char hdr[32];
    snprintf(hdr, sizeof(hdr), "Members - %d", st->member_count);
    ui_text(x + 12, top + 24, COLOR_TEXT_DIM, 14, hdr);

    int list_top = top + 34;
    int visible = (bottom - list_top - 4) / RAIL_ROW_H;
    int first = st->member_scroll;
    if (first > st->member_count - visible)
        first = st->member_count - visible;
    if (first < 0)
        first = 0;

    for (int i = 0; i < visible && first + i < st->member_count; i++) {
        const st_member *m = &st->members[first + i];
        int y = list_top + i * RAIL_ROW_H;
        vita2d_draw_fill_circle((float)(x + 18), (float)(y + RAIL_ROW_H / 2 + 2),
                                5.0f, presence_color(m->status));
        draw_text_clipped(x + 32, y + 22, COLOR_TEXT, RAIL_SCALE,
                          m->name, MEMBERS_W - 42);
    }

    if (st->channel_id[0] && st->member_count == 0)
        ui_text(x + 12, list_top + 20, COLOR_TEXT_DIM,
                             0.7f, "Nobody visible");
}

static void render_chat_pane(const app_state *st)
{
    /* The member rail only exists while a channel is open: browsing, the
       chat pane takes the full width (author's call: three columns at all
       times crowd a 5-inch screen). */
    int x0 = RAIL_W;
    int x1 = st->channel_id[0] ? SCREEN_W - MEMBERS_W : SCREEN_W;
    hit_count = 0;
    if (st->focus == FOCUS_CHAT)
        vita2d_draw_rectangle(x0, HEADER_H, x1 - x0, 2, COLOR_ACCENT);

    if (st->channel_id[0] == '\0') {
        ui_text(x0 + 24, HEADER_H + 48, COLOR_TEXT_DIM,
                             0.9f, "Pick a channel on the left,");
        ui_text(x0 + 24, HEADER_H + 48 + LINE_H + 4,
                             COLOR_TEXT_DIM, 17, "X opens it here.");
        return;
    }

    int avatar_x = x0 + PAD_X;
    int text_x = avatar_x + AVATAR_SZ + 10;
    int input_top = SCREEN_H - FOOTER_H - 36;
    int text_w = x1 - text_x - PAD_X;

    /* Scroll-back feedback pinned under the title bar. */
    if (st->history_pending) {
        const char *msg = "Loading older messages...";
        int w = ui_text_width(15, msg);
        ui_text(x0 + (x1 - x0 - w) / 2, HEADER_H + 18,
                             COLOR_ACCENT, 15, msg);
    } else if (st->history_done && st->message_count > 0 &&
               st->chat_scroll >= st->message_count - 1) {
        /* Honest label: a fresh channel top is one thing, a companion too
           old to serve scroll-back is another. */
        const char *msg = st->companion_old
            ? "Companion outdated: can't load older messages"
            : "Beginning of history";
        int w = ui_text_width(15, msg);
        ui_text(x0 + (x1 - x0 - w) / 2, HEADER_H + 18,
                             st->companion_old ? COLOR_ERROR : COLOR_TEXT_DIM,
                             0.8f, msg);
    }

    int typing = st->typing_ttl > 0 && st->typing_name[0];

    /* Messages, newest at the bottom, drawn upward until we run out of
       room. Consecutive messages by the same author group under one
       avatar + name header, Discord style. */
    static char lines[WRAP_MAX_LINES][WRAP_LINE_LEN];
    static char elines[ST_MAX_EMBEDS][3][WRAP_LINE_LEN];
    int y_bottom = input_top - 8 - (typing ? 18 : 0);

    int newest = st->message_count - 1 - st->chat_scroll;
    for (int i = newest; i >= 0 && y_bottom > HEADER_H + LINE_H; i--) {
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
        if (starts_group && y + 4 >= HEADER_H) {
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
            if (ly > HEADER_H + LINE_H && ly < input_top)
                ui_text(text_x, ly, COLOR_TEXT,
                                     CHAT_SCALE, lines[l]);
        }

        int below_y = content_top + nlines * LINE_H + 4;
        for (int e = 0; e < m->embed_count; e++) {
            const st_embed *em = &m->embeds[e];
            if (below_y >= HEADER_H + 4 && below_y + eh[e] < input_top) {
                unsigned int bar = em->color
                    ? RGBA8((em->color >> 16) & 255, (em->color >> 8) & 255,
                            em->color & 255, 255)
                    : COLOR_ACCENT;
                vita2d_draw_rectangle(text_x, below_y, emb_w, eh[e],
                                      RGBA8(43, 45, 49, 255));
                vita2d_draw_rectangle(text_x, below_y, 4, eh[e], bar);
                int ty = below_y + LINE_H;
                if (em->title[0]) {
                    draw_text_clipped(text_x + 14, ty, COLOR_WHITE, 17,
                                      em->title, emb_w - 26);
                    ty += LINE_H;
                }
                for (int l = 0; l < en[e]; l++) {
                    ui_text(text_x + 14, ty, COLOR_TEXT,
                                         0.85f, elines[e][l]);
                    ty += LINE_H;
                }
            }
            below_y += eh[e] + 6;
        }

        if (m->image[0]) {
            int iy = below_y;
            if (iy >= HEADER_H + 4 && iy + thumb_h < input_top) {
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
                    vita2d_draw_rectangle(text_x, iy, thumb_w, thumb_h,
                                          COLOR_INPUT_BG);
                    ui_text(text_x + 12,
                                         iy + thumb_h / 2 + 6,
                                         COLOR_TEXT_DIM, 14, "image...");
                }
                draw_box(text_x - 2, iy - 2, thumb_w + 4, thumb_h + 4,
                         COLOR_BORDER);
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
        ui_text(x0 + PAD_X, HEADER_H + 30, COLOR_TEXT_DIM,
                             0.9f, "No messages loaded.");

    if (typing) {
        char tline[ST_AUTHOR_LEN + 24];
        snprintf(tline, sizeof(tline), "%s is typing...", st->typing_name);
        ui_text(x0 + PAD_X + 4, input_top - 6,
                             COLOR_TEXT_DIM, 13, tline);
    }

    /* Input bar: squared with a visible border, the old-VitaCord look. */
    vita2d_draw_rectangle(x0 + 6, input_top, x1 - x0 - 12, 32, COLOR_INPUT_BG);
    draw_box(x0 + 6, input_top, x1 - x0 - 12, 32, COLOR_BORDER);
    if (st->chat_scroll > 0) {
        char more[48];
        snprintf(more, sizeof(more), "v %d newer below - DOWN to return",
                 st->chat_scroll);
        ui_text(x0 + PAD_X + 4, input_top + 22, COLOR_ACCENT,
                             0.85f, more);
    } else {
        ui_text(x0 + PAD_X + 4, input_top + 22,
                             COLOR_TEXT_DIM, 16, "START to write");
    }
}

static const char *workspace_hints(const app_state *st)
{
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
    char title[ST_NAME_LEN + 2];
    if (st->channel_id[0])
        snprintf(title, sizeof(title), "# %s", st->channel_name);
    else
        snprintf(title, sizeof(title), "%s", st->guild_name);
    draw_header(title, st->guild_name);

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
