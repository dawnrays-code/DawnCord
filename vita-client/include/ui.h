#ifndef DAWNCORD_UI_H
#define DAWNCORD_UI_H

#include "state.h"

#define SCREEN_W 960
#define SCREEN_H 544

#define COLOR_BG        RGBA8(54, 57, 63, 255)
#define COLOR_SIDEBAR   RGBA8(47, 49, 54, 255)
#define COLOR_HEADER    RGBA8(41, 43, 47, 255)
#define COLOR_INPUT_BG  RGBA8(64, 68, 75, 255)
#define COLOR_SELECT    RGBA8(71, 76, 84, 255)
#define COLOR_TEXT      RGBA8(220, 221, 222, 255)
#define COLOR_TEXT_DIM  RGBA8(142, 146, 151, 255)
#define COLOR_ACCENT    RGBA8(88, 101, 242, 255)
#define COLOR_WHITE     RGBA8(255, 255, 255, 255)
#define COLOR_ERROR     RGBA8(237, 66, 69, 255)

/* Layered surfaces, darkest at the back. Depth here comes from value
   steps and soft shadows rather than from borders: a 1px outline around
   everything is what makes an interface look drawn instead of designed. */
#define COLOR_FRAME     RGBA8(26, 27, 30, 255)    /* the room the cards sit in */
#define COLOR_GREEN     RGBA8(35, 165, 90, 255)   /* online, connected, talking */
#define COLOR_RING      RGBA8(255, 255, 255, 16)  /* top hairline on a card */
#define COLOR_TEXT_MUTE RGBA8(128, 132, 142, 255) /* glyphs, quietest text */
#define COLOR_CHROME    RGBA8(24, 25, 28, 255)    /* header and footer */
#define COLOR_RAIL      RGBA8(32, 34, 38, 255)    /* side columns */
#define COLOR_RAIL_HI   RGBA8(40, 42, 47, 255)    /* side column, focused */
#define COLOR_CARD      RGBA8(49, 51, 56, 255)    /* raised cards */
#define COLOR_ROW_SEL   RGBA8(64, 68, 77, 255)    /* selected row */
#define COLOR_SHADOW    RGBA8(0, 0, 0, 150)

typedef enum {
    VIEW_GUILD_LIST,   /* full-screen server list (comfortable from the couch) */
    VIEW_WORKSPACE,    /* three columns: channels | chat | members */
} dawncord_view;

/* Columns in the server grid; main.c needs it for D-pad navigation. */
#define GUILD_GRID_COLS 4

/* Workspace pane focus values kept in app_state.focus. */
#define FOCUS_CHANNELS 0
#define FOCUS_CHAT     1
#define FOCUS_MEMBERS  2

void ui_init(void);
void ui_term(void);

/* Draw one full frame from state. Redrawn every frame by the main loop;
   ui_render ends the frame itself (including the common-dialog update the
   IME overlay needs). */
void ui_render(const app_state *st, dawncord_view view);

/* Full-screen centered message for phases where no state exists yet
   (connecting, fatal errors). Also a complete frame. */
void ui_draw_status(const char *text);

/* Full-screen loading/connection screen: title, optional subtitle, and an
   animated spinner. `frame` is any incrementing counter (drives the
   animation). A complete frame on its own. */
void ui_draw_loading(const char *title, const char *subtitle, int frame);

/* Attachment thumbnails record their on-screen rects while rendering;
   this checks a (front touch) tap against them. Returns 1 and copies the
   image URL when one is hit. */
int ui_hit_image(int x, int y, char *url_out, int out_size);

#endif
