#include "mic_frame.h"

#include <string.h>

void mic_frame_reset(mic_frame_state *s, int rate_hz)
{
    memset(s, 0, sizeof(*s));
    s->rate_code = (rate_hz == 48000) ? 1 : 0;
}

void mic_frame_level(const int16_t *pcm, int n, int32_t *peak, uint32_t *rms)
{
    int32_t p = 0;
    uint64_t acc = 0;

    for (int i = 0; i < n; i++) {
        int32_t v = pcm[i];
        int32_t a = v < 0 ? -v : v;      /* -32768 negates to itself in
                                            16 bits, so widen first */
        if (a > p)
            p = a;
        acc += (uint64_t)((int64_t)v * v);
    }

    if (peak)
        *peak = p;
    if (rms) {
        /* Integer square root: no libm on this path, and the value only
           ever feeds a level meter and a gate. */
        uint64_t mean = n > 0 ? acc / (uint64_t)n : 0;
        uint64_t r = 0, bit = 1ULL << 30;
        while (bit > mean)
            bit >>= 2;
        while (bit) {
            if (mean >= r + bit) {
                mean -= r + bit;
                r = (r >> 1) + bit;
            } else {
                r >>= 1;
            }
            bit >>= 2;
        }
        *rms = (uint32_t)r;
    }
}

int mic_frame_build(mic_frame_state *s, const int16_t *pcm, int n,
                    const uint16_t ref[MIC_REF_SLOTS],
                    uint8_t *out, size_t out_size)
{
    if (!s || !out || n < 0)
        return 0;

    s->cap++;

    /* A muted or silent grain still travels: the header alone keeps the
       sequence unbroken and keeps telling the companion what the console
       is doing, which is what lets it distinguish "quiet" from "the link
       died". */
    int carry_pcm = !s->mic_muted && !s->hw_muted && n > 0;
    if (carry_pcm && !s->gate_open) {
        carry_pcm = 0;
        s->gated++;
    }

    size_t need = MIC_HEADER_BYTES + (carry_pcm ? (size_t)n * 2 : 0);
    if (out_size < need)
        return 0;                      /* refuse rather than truncate */

    if (n > 0 && pcm)
        mic_frame_level(pcm, n, &s->peak, &s->rms);
    else
        s->peak = 0, s->rms = 0;

    uint16_t flags = (uint16_t)(s->rate_code & MIC_FLAG_RATE_MASK);
    if (s->spk_muted) flags |= MIC_FLAG_SPK_MUTED;
    if (s->mic_muted) flags |= MIC_FLAG_MIC_MUTED;
    if (s->hw_muted)  flags |= MIC_FLAG_HW_MUTED;
    if (s->gate_open) flags |= MIC_FLAG_GATE_OPEN;
    if (s->ref_bad)   flags |= MIC_FLAG_REF_BAD;
    if (!carry_pcm)   flags |= MIC_FLAG_SILENT;

    out[0] = (uint8_t)(s->seq >> 24);
    out[1] = (uint8_t)(s->seq >> 16);
    out[2] = (uint8_t)(s->seq >> 8);
    out[3] = (uint8_t)(s->seq);
    out[4] = (uint8_t)(flags >> 8);
    out[5] = (uint8_t)(flags);

    for (int i = 0; i < MIC_REF_SLOTS; i++) {
        uint16_t v = ref ? ref[i] : 0;
        out[6 + i * 2] = (uint8_t)(v >> 8);
        out[7 + i * 2] = (uint8_t)(v);
    }

    if (carry_pcm) {
        /* Little-endian payload: the companion reads it as raw s16 and
           the console is little-endian, so this is a straight copy on
           hardware and an explicit one everywhere else. */
        uint8_t *p = out + MIC_HEADER_BYTES;
        for (int i = 0; i < n; i++) {
            uint16_t v = (uint16_t)pcm[i];
            *p++ = (uint8_t)(v);
            *p++ = (uint8_t)(v >> 8);
        }
    }

    s->seq++;                          /* wraps at 2^32, as the companion expects */
    s->sent++;
    return (int)need;
}
