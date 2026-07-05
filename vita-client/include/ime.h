#ifndef DAWNCORD_IME_H
#define DAWNCORD_IME_H

/* Native on-screen keyboard (SceImeDialog). The dialog renders as a common
   dialog overlay: while it is active the main loop must keep drawing frames
   and calling vita2d_common_dialog_update() (ui_end() does this). */

#define IME_MAX_INPUT 512

typedef enum {
    IME_NONE = 0,     /* no dialog active */
    IME_RUNNING,      /* dialog on screen, keep rendering */
    IME_DONE,         /* user confirmed: text copied to out */
    IME_CANCELED,     /* user closed without confirming */
} ime_status;

/* Open the keyboard. Returns 0 on success, -1 if it failed to init
   (e.g. another common dialog is already up). */
int ime_start(const char *title_utf8, const char *initial_utf8);

/* Poll once per frame while active. On IME_DONE the entered text is
   written to out as UTF-8. */
ime_status ime_update(char *out_utf8, int out_size);

int ime_active(void);

#endif
