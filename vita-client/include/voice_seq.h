#ifndef DAWNCORD_VOICE_SEQ_H
#define DAWNCORD_VOICE_SEQ_H

#include <stdint.h>

/* Sequencing layer between the UDP socket and the playback ring.

   The old policy (keep strictly-newer, drop everything else) turned every
   reordered packet into a 10ms excision: hardware counters showed 5.7% of
   packets arriving out of order on real Wi-Fi even with paced sends, the
   cushion drained with nothing to rebuild it, and playback sat at the
   underrun floor gating the speech 94 times a second, which is what a
   robot voice is. This layer holds a few packets to let stragglers slot
   back in, and when one is truly missing it pushes faded repeats of the
   last audio instead of splicing, so the cushion never drains.

   Pure C, no Vita dependencies: the same code runs in the host test. */

typedef void (*vseq_push_fn)(const int16_t *pcm, int n);

typedef struct {
    uint32_t delivered;   /* packets pushed to the ring, in order */
    uint32_t dropped;     /* duplicates and hopeless stragglers */
    uint32_t reordered;   /* arrived early, held, then slotted in */
    uint32_t concealed;   /* missing packets replaced with faded repeats */
    uint32_t restarts;    /* sender sequence restarted (new session) */
} vseq_stats;

void vseq_reset(void);
void vseq_packet(uint32_t seq, const int16_t *pcm, int n, vseq_push_fn push);
const vseq_stats *vseq_get_stats(void);

#endif
