#include "imgcache.h"

#include <vita2d.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define IMG_MAX 48

typedef enum {
    SLOT_EMPTY = 0,
    SLOT_WANTED,   /* UI asked for it, request not sent yet */
    SLOT_PENDING,  /* request sent, waiting for IMAGE_DATA */
    SLOT_READY,    /* texture loaded */
    SLOT_FAILED,   /* companion couldn't provide it: don't ask again */
} slot_state;

typedef struct {
    char key[IMG_KEY_LEN];
    slot_state state;
    vita2d_texture *tex;
    unsigned last_used;
} img_slot;

static img_slot slots[IMG_MAX];
static unsigned tick = 0;

/* Deferred-free graveyard. Eviction happens inside img_get, i.e. in the
   middle of the frame being recorded, and the GPU also runs 2-3 frames
   behind the CPU: freeing a texture right there is a use-after-free the
   GPU pays for (SceGxm dies with no CPU exception, see the GPUCRASH core
   dumps). Evicted textures rest here for GRAVE_FRAMES swaps instead. */
#define GRAVE_MAX    64
#define GRAVE_FRAMES 4

typedef struct {
    vita2d_texture *tex;
    unsigned frame;
} grave_slot;

static grave_slot graveyard[GRAVE_MAX];
static unsigned frame_no = 0;

static void bury(vita2d_texture *tex)
{
    if (!tex)
        return;
    for (int i = 0; i < GRAVE_MAX; i++) {
        if (!graveyard[i].tex) {
            graveyard[i].tex = tex;
            graveyard[i].frame = frame_no;
            return;
        }
    }
    /* Graveyard full (should be unreachable: it drains every frame and
       evictions are bounded by draws per frame). Stall the GPU once
       rather than corrupt it or leak. */
    vita2d_wait_rendering_done();
    vita2d_free_texture(tex);
}

void img_gc(void)
{
    frame_no++;
    for (int i = 0; i < GRAVE_MAX; i++) {
        if (graveyard[i].tex &&
            frame_no - graveyard[i].frame >= GRAVE_FRAMES) {
            vita2d_free_texture(graveyard[i].tex);
            graveyard[i].tex = NULL;
        }
    }
}

static img_slot *find(const char *key)
{
    for (int i = 0; i < IMG_MAX; i++)
        if (slots[i].state != SLOT_EMPTY && strcmp(slots[i].key, key) == 0)
            return &slots[i];
    return NULL;
}

static void release(img_slot *s)
{
    bury(s->tex);
    s->tex = NULL;
    s->state = SLOT_EMPTY;
    s->key[0] = '\0';
}

static img_slot *alloc(void)
{
    img_slot *victim = NULL;
    for (int i = 0; i < IMG_MAX; i++) {
        if (slots[i].state == SLOT_EMPTY)
            return &slots[i];
        /* Evict the least-recently drawn settled slot; never one that is
           still waiting for the network. */
        if (slots[i].state == SLOT_READY || slots[i].state == SLOT_FAILED) {
            if (!victim || slots[i].last_used < victim->last_used)
                victim = &slots[i];
        }
    }
    if (victim)
        release(victim);
    return victim;
}

struct vita2d_texture *img_get_sized(const char *url, int size)
{
    char key[IMG_KEY_LEN];
    if (!url || !url[0])
        return NULL;
    if (snprintf(key, sizeof(key), "%s#%d", url, size) >= (int)sizeof(key))
        return NULL;
    return img_get(key);
}

struct vita2d_texture *img_get(const char *url)
{
    if (!url || !url[0] || strlen(url) >= IMG_KEY_LEN)
        return NULL;

    tick++;
    img_slot *s = find(url);
    if (s) {
        s->last_used = tick;
        return s->tex;  /* NULL unless READY */
    }

    s = alloc();
    if (!s)
        return NULL;  /* cache full of in-flight requests: try next frame */
    snprintf(s->key, IMG_KEY_LEN, "%s", url);
    s->state = SLOT_WANTED;
    s->tex = NULL;
    s->last_used = tick;
    return NULL;
}

int img_next_request(char *url_out, size_t out_size)
{
    for (int i = 0; i < IMG_MAX; i++) {
        if (slots[i].state == SLOT_WANTED) {
            if (strlen(slots[i].key) >= out_size)
                return 0;
            strcpy(url_out, slots[i].key);
            slots[i].state = SLOT_PENDING;
            return 1;
        }
    }
    return 0;
}

/* Punch a circular alpha mask into the texture: avatars and server icons
   draw round, Discord style. Textures from the JPEG loader are linear
   ABGR8888, alpha in the top byte. The edge is feathered ~1.5px: a hard
   cutoff shows as jagged nicks once the texture is drawn scaled. */
static void mask_circle(vita2d_texture *tex)
{
    unsigned int w = vita2d_texture_get_width(tex);
    unsigned int h = vita2d_texture_get_height(tex);
    unsigned int stride_bytes = vita2d_texture_get_stride(tex);
    unsigned int *px = (unsigned int *)vita2d_texture_get_datap(tex);
    if (!px || stride_bytes < w * 4)
        return;  /* not the 32-bit linear layout we expect: leave it square */
    unsigned int stride = stride_bytes / 4;

    float cx = (w - 1) / 2.0f, cy = (h - 1) / 2.0f;
    float r = (w < h ? w : h) / 2.0f;
    for (unsigned int y = 0; y < h; y++) {
        for (unsigned int x = 0; x < w; x++) {
            float dx = x - cx, dy = y - cy;
            float d = sqrtf(dx * dx + dy * dy);
            if (d <= r - 1.5f)
                continue;
            unsigned int p = px[y * stride + x];
            if (d >= r) {
                px[y * stride + x] = p & 0x00FFFFFFu;
            } else {
                float t = (r - d) / 1.5f;   /* 0 at the rim, 1 inside */
                unsigned int a = (unsigned int)(((p >> 24) & 0xFF) * t);
                px[y * stride + x] = (p & 0x00FFFFFFu) | (a << 24);
            }
        }
    }
}

void img_store(const char *key, const void *jpeg, size_t jpeg_len)
{
    img_slot *s = find(key);
    if (!s || s->state == SLOT_READY)
        return;

    if (jpeg && jpeg_len > 0)
        s->tex = vita2d_load_JPEG_buffer(jpeg, jpeg_len);
    /* Sized keys ("url#160") are attachment images and stay rectangular;
       plain keys are avatars/icons and go round. */
    if (s->tex && !strchr(s->key, '#'))
        mask_circle(s->tex);
    s->state = s->tex ? SLOT_READY : SLOT_FAILED;
}

void img_clear(void)
{
    /* Shutdown path: wait for the GPU for real, then free everything,
       graveyard included. Called before vita2d_fini. */
    vita2d_wait_rendering_done();
    for (int i = 0; i < IMG_MAX; i++) {
        if (slots[i].tex) {
            vita2d_free_texture(slots[i].tex);
            slots[i].tex = NULL;
        }
        slots[i].state = SLOT_EMPTY;
        slots[i].key[0] = '\0';
    }
    for (int i = 0; i < GRAVE_MAX; i++) {
        if (graveyard[i].tex) {
            vita2d_free_texture(graveyard[i].tex);
            graveyard[i].tex = NULL;
        }
    }
}
