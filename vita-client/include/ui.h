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

typedef enum {
    VIEW_GUILD_LIST,   /* full-screen server list (comfortable from the couch) */
    VIEW_WORKSPACE,    /* three columns: channels | chat | members */
} dawncord_view;

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

/* Attachment thumbnails record their on-screen rects while rendering;
   this checks a (front touch) tap against them. Returns 1 and copies the
   image URL when one is hit. */
int ui_hit_image(int x, int y, char *url_out, int out_size);

#endif
