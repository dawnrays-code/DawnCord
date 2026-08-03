/* Host test for the microphone uplink frame builder, in the style of
   vseq_test.c. No psp2 headers, so plain gcc builds it:
   gcc -Wall -Wextra -Iinclude test/mic_frame_test.c src/mic_frame.c -o mic_frame_test */
#include "mic_frame.h"

#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [ok] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); fail = 1; } \
} while (0)

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

int main(void)
{
    mic_frame_state s;
    uint8_t out[4096];
    int16_t pcm[768];
    uint16_t ref[MIC_REF_SLOTS];

    for (int i = 0; i < MIC_REF_SLOTS; i++)
        ref[i] = (uint16_t)(1000 + i);

    /* 1: header size and payload size for every grain the hardware can
       hand us. */
    static const int grains[] = { 256, 512, 768 };   /* every grain the
                                                        capture API allows */
    for (unsigned g = 0; g < sizeof(grains) / sizeof(grains[0]); g++) {
        int n = grains[g];
        mic_frame_reset(&s, 16000);
        s.gate_open = 1;
        for (int i = 0; i < n; i++)
            pcm[i] = (int16_t)i;
        int len = mic_frame_build(&s, pcm, n, ref, out, sizeof(out));
        char msg[64];
        snprintf(msg, sizeof(msg), "grain %d builds %d bytes", n,
                 MIC_HEADER_BYTES + n * 2);
        CHECK(len == MIC_HEADER_BYTES + n * 2, msg);
    }

    /* 2: byte order, big-endian header and little-endian payload. */
    mic_frame_reset(&s, 16000);
    s.gate_open = 1;
    pcm[0] = (int16_t)0x1234;
    pcm[1] = (int16_t)-2;                    /* 0xFFFE */
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK(be32(out) == 0 && be16(out + 4) == 0 + MIC_FLAG_GATE_OPEN,
          "sequence and flags are big-endian");
    CHECK(be16(out + 6) == 1000 && be16(out + 6 + 22) == 1011,
          "reference slots are big-endian, oldest first");
    CHECK(out[MIC_HEADER_BYTES] == 0x34 && out[MIC_HEADER_BYTES + 1] == 0x12,
          "payload is little-endian");
    CHECK(out[MIC_HEADER_BYTES + 2] == 0xFE &&
          out[MIC_HEADER_BYTES + 3] == 0xFF,
          "negative samples survive the round trip");

    /* 3: the sequence advances per datagram and wraps cleanly. */
    mic_frame_reset(&s, 16000);
    s.gate_open = 1;
    s.seq = 0xFFFFFFFEu;
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK(be32(out) == 0xFFFFFFFEu, "sequence written before increment");
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK(be32(out) == 0xFFFFFFFFu, "sequence advances");
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK(be32(out) == 0, "sequence wraps to zero, no gap");

    /* 4: muted and gated grains carry the header alone, and the flags say
       which of the two it was. */
    mic_frame_reset(&s, 16000);
    s.mic_muted = 1;
    s.gate_open = 1;
    int len = mic_frame_build(&s, pcm, 256, ref, out, sizeof(out));
    CHECK(len == MIC_HEADER_BYTES, "muted grain is header only");
    CHECK((be16(out + 4) & MIC_FLAG_MIC_MUTED) &&
          (be16(out + 4) & MIC_FLAG_SILENT),
          "muted grain flags mic_muted and silent");

    mic_frame_reset(&s, 16000);
    s.gate_open = 0;
    len = mic_frame_build(&s, pcm, 256, ref, out, sizeof(out));
    CHECK(len == MIC_HEADER_BYTES && s.gated == 1,
          "closed gate withholds the payload and is counted");
    CHECK(!(be16(out + 4) & MIC_FLAG_MIC_MUTED),
          "a gated grain is not reported as muted");

    /* 5: the system-wide mute is honoured even when nothing else is set. */
    mic_frame_reset(&s, 16000);
    s.gate_open = 1;
    s.hw_muted = 1;
    len = mic_frame_build(&s, pcm, 256, ref, out, sizeof(out));
    CHECK(len == MIC_HEADER_BYTES && (be16(out + 4) & MIC_FLAG_HW_MUTED),
          "system mute suppresses the payload");

    /* 6: the rate code rides in the flags, so the datagram describes
       itself without any help from the control channel. */
    mic_frame_reset(&s, 48000);
    s.gate_open = 1;
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK((be16(out + 4) & MIC_FLAG_RATE_MASK) == 1, "48kHz rate code set");
    mic_frame_reset(&s, 16000);
    s.gate_open = 1;
    mic_frame_build(&s, pcm, 2, ref, out, sizeof(out));
    CHECK((be16(out + 4) & MIC_FLAG_RATE_MASK) == 0, "16kHz rate code set");

    /* 7: a buffer too small is refused outright, and refusing costs no
       sequence number. */
    mic_frame_reset(&s, 16000);
    s.gate_open = 1;
    uint8_t tiny[MIC_HEADER_BYTES + 3];
    memset(tiny, 0xAA, sizeof(tiny));
    len = mic_frame_build(&s, pcm, 256, ref, tiny, sizeof(tiny));
    CHECK(len == 0, "short buffer refused");
    CHECK(tiny[0] == 0xAA, "refusal writes nothing at all");
    CHECK(s.seq == 0 && s.sent == 0, "refusal does not consume a sequence");

    /* 8: levels. A full-scale square wave is its own peak and RMS, and
       the most negative sample must not overflow the absolute value. */
    for (int i = 0; i < 64; i++)
        pcm[i] = (i & 1) ? 1000 : -1000;
    int32_t peak = 0;
    uint32_t rms = 0;
    mic_frame_level(pcm, 64, &peak, &rms);
    CHECK(peak == 1000 && rms == 1000, "square wave: peak and rms both 1000");

    pcm[0] = -32768;
    mic_frame_level(pcm, 64, &peak, &rms);
    CHECK(peak == 32768, "the most negative sample does not overflow");

    for (int i = 0; i < 64; i++)
        pcm[i] = 0;
    mic_frame_level(pcm, 64, &peak, &rms);
    CHECK(peak == 0 && rms == 0, "silence measures zero");

    if (fail) {
        printf("MIC FRAME TESTS FAILED\n");
        return 1;
    }
    printf("All mic frame tests passed.\n");
    return 0;
}
