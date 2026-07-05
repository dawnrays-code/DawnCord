#ifndef DAWNCORD_IMGCACHE_H
#define DAWNCORD_IMGCACHE_H

#include <stddef.h>

/* Texture cache for avatars / server icons, keyed by CDN URL.

   Flow per frame:
     - the UI calls img_get(url) wherever it wants an image: it either gets
       a texture back or NULL (drawing a placeholder), and the miss is
       remembered as "wanted";
     - after rendering, the main loop drains img_next_request() and sends
       one MSG_REQUEST_IMAGE per wanted entry (marked pending, so it's
       requested exactly once);
     - when MSG_IMAGE_DATA arrives, img_store() decodes the JPEG into a
       texture (or marks the entry failed so it's never re-requested).

   All calls must happen on the main thread: vita2d textures are not
   thread-safe, and the receiver thread only ever queues raw messages. */

#define IMG_KEY_LEN 336  /* matches ST_URL_LEN */

/* Bare struct tag to avoid re-typedefing vita2d_texture. */
struct vita2d_texture;

struct vita2d_texture *img_get(const char *url);

/* Same, but for a specific size (attachment thumbnails vs the expanded
   view). The cache key becomes "url#size"; img_next_request hands back
   that key, and the sender splits it into url + size. Plain img_get
   entries are avatars/icons and get circle-masked on store. */
struct vita2d_texture *img_get_sized(const char *url, int size);

int img_next_request(char *url_out, size_t out_size);
void img_store(const char *key, const void *jpeg, size_t jpeg_len);
void img_clear(void);

/* Call once per frame, after ui_render: frees evicted textures once the
   GPU can no longer be reading them. Freeing them at eviction time (from
   inside the frame being drawn) is what used to crash SceGxm. */
void img_gc(void);

#endif
