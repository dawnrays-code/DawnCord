#include "ime.h"

#include <psp2/ime_dialog.h>
#include <psp2/common_dialog.h>

#include <string.h>
#include <stdint.h>

static uint16_t ime_title[64];
static uint16_t ime_initial[IME_MAX_INPUT + 1];
static uint16_t ime_input[IME_MAX_INPUT + 1];
static int active = 0;

/* ---- minimal UTF-8 <-> UTF-16 (with surrogate pairs) ---- */

static void utf8_to_utf16(const char *src, uint16_t *dst, int dst_max)
{
    int out = 0;
    while (*src && out < dst_max - 1) {
        unsigned char c = (unsigned char)*src;
        uint32_t cp;
        int len;

        if (c < 0x80)        { cp = c;          len = 1; }
        else if (c < 0xE0)   { cp = c & 0x1F;   len = 2; }
        else if (c < 0xF0)   { cp = c & 0x0F;   len = 3; }
        else                 { cp = c & 0x07;   len = 4; }

        for (int i = 1; i < len; i++) {
            if ((src[i] & 0xC0) != 0x80) { len = 1; cp = '?'; break; }
            cp = (cp << 6) | (src[i] & 0x3F);
        }
        src += len;

        if (cp >= 0x10000) {
            if (out >= dst_max - 2)
                break;
            cp -= 0x10000;
            dst[out++] = (uint16_t)(0xD800 | (cp >> 10));
            dst[out++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            dst[out++] = (uint16_t)cp;
        }
    }
    dst[out] = 0;
}

static void utf16_to_utf8(const uint16_t *src, char *dst, int dst_max)
{
    int out = 0;
    while (*src) {
        uint32_t cp = *src++;
        if (cp >= 0xD800 && cp < 0xDC00 && *src >= 0xDC00 && *src < 0xE000)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (*src++ - 0xDC00);

        int need = cp < 0x80 ? 1 : cp < 0x800 ? 2 : cp < 0x10000 ? 3 : 4;
        if (out + need >= dst_max)
            break;

        if (need == 1) {
            dst[out++] = (char)cp;
        } else if (need == 2) {
            dst[out++] = (char)(0xC0 | (cp >> 6));
            dst[out++] = (char)(0x80 | (cp & 0x3F));
        } else if (need == 3) {
            dst[out++] = (char)(0xE0 | (cp >> 12));
            dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[out++] = (char)(0x80 | (cp & 0x3F));
        } else {
            dst[out++] = (char)(0xF0 | (cp >> 18));
            dst[out++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            dst[out++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            dst[out++] = (char)(0x80 | (cp & 0x3F));
        }
    }
    dst[out] = '\0';
}

/* ---- dialog lifecycle ---- */

int ime_start(const char *title_utf8, const char *initial_utf8)
{
    if (active)
        return -1;

    utf8_to_utf16(title_utf8 ? title_utf8 : "", ime_title, 64);
    utf8_to_utf16(initial_utf8 ? initial_utf8 : "", ime_initial, IME_MAX_INPUT + 1);
    memset(ime_input, 0, sizeof(ime_input));

    SceImeDialogParam param;
    sceImeDialogParamInit(&param);
    param.supportedLanguages = 0x0001FFFF;
    param.languagesForced = SCE_TRUE;
    param.type = SCE_IME_TYPE_DEFAULT;
    param.title = ime_title;
    param.maxTextLength = IME_MAX_INPUT;
    param.initialText = ime_initial;
    param.inputTextBuffer = ime_input;

    if (sceImeDialogInit(&param) < 0)
        return -1;

    active = 1;
    return 0;
}

ime_status ime_update(char *out_utf8, int out_size)
{
    if (!active)
        return IME_NONE;

    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return IME_RUNNING;

    SceImeDialogResult result;
    memset(&result, 0, sizeof(result));
    sceImeDialogGetResult(&result);
    sceImeDialogTerm();
    active = 0;

    if (result.button == SCE_IME_DIALOG_BUTTON_ENTER) {
        utf16_to_utf8(ime_input, out_utf8, out_size);
        return IME_DONE;
    }
    return IME_CANCELED;
}

int ime_active(void)
{
    return active;
}
