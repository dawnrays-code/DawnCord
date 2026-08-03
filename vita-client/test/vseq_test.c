/* Host test for the voice sequencing layer: the wire scenarios measured
   on real hardware, replayed deterministically. Build with any C compiler:
   gcc -Wall -Iinclude test/vseq_test.c src/voice_seq.c -o vseq_test */
#include "voice_seq.h"

#include <stdio.h>
#include <string.h>

static int fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) printf("  [ok] %s\n", msg); \
    else { printf("  [FAIL] %s\n", msg); fail = 1; } \
} while (0)

/* Each packet carries its sequence number in sample 0, so the push
   callback can record the exact delivery order. Concealment frames carry
   a faded copy of the previous value, never a fresh sequence number. */
static int out_vals[512];
static int out_n;

static void push(const int16_t *pcm, int n)
{
    (void)n;
    if (out_n < 512)
        out_vals[out_n++] = pcm[0];
}

static void send_pkt(uint32_t seq)
{
    int16_t pcm[480];
    for (int i = 0; i < 480; i++)
        pcm[i] = (int16_t)seq;
    vseq_packet(seq, pcm, 480, push);
}

static int order_is(const int *want, int n)
{
    if (out_n != n)
        return 0;
    return memcmp(out_vals, want, (size_t)n * sizeof(int)) == 0;
}

int main(void)
{
    const vseq_stats *st = vseq_get_stats();

    /* 1: clean in-order stream passes through untouched. */
    vseq_reset();
    out_n = 0;
    for (uint32_t s = 0; s < 6; s++)
        send_pkt(s);
    {
        const int want[] = { 0, 1, 2, 3, 4, 5 };
        CHECK(order_is(want, 6) && st->dropped == 0 && st->concealed == 0,
              "in-order stream untouched");
    }

    /* 2: pair swap (the measured Wi-Fi case): straggler slots back in,
       nothing dropped, nothing concealed. */
    vseq_reset();
    out_n = 0;
    send_pkt(0); send_pkt(1); send_pkt(2);
    send_pkt(4); send_pkt(3);          /* inverted pair */
    send_pkt(5);
    {
        const int want[] = { 0, 1, 2, 3, 4, 5 };
        CHECK(order_is(want, 6), "swapped pair delivered in order");
        CHECK(st->dropped == 0 && st->concealed == 0 && st->reordered == 1,
              "swap costs nothing");
    }

    /* 3: one packet truly lost: healed with one faded repeat once the
       hold fills, stream continues in order. */
    vseq_reset();
    out_n = 0;
    send_pkt(0); send_pkt(1); send_pkt(2);
    send_pkt(4); send_pkt(5); send_pkt(6);   /* 3 never arrives */
    send_pkt(7);
    {
        /* conceal repeats the last delivered value (2), faded 3/4 */
        const int want[] = { 0, 1, 2, 2 * 3 / 4, 4, 5, 6, 7 };
        CHECK(order_is(want, 8), "single loss concealed, stream continues");
        CHECK(st->concealed == 1 && st->dropped == 0,
              "loss counted as concealment, not drop");
    }

    /* 4: duplicates and stale stragglers are dropped, not replayed. */
    vseq_reset();
    out_n = 0;
    send_pkt(0); send_pkt(1); send_pkt(1); send_pkt(2); send_pkt(0);
    {
        const int want[] = { 0, 1, 2 };
        CHECK(order_is(want, 3) && st->dropped == 2,
              "duplicates dropped once each");
    }

    /* 5: burst loss beyond repair: conceal the cap, then jump. */
    vseq_reset();
    out_n = 0;
    send_pkt(0); send_pkt(1);
    send_pkt(12);                       /* 2..11 vanished in one RF event */
    send_pkt(13);
    CHECK(out_n == 2 + 4 + 2 && out_vals[6] == 12 && out_vals[7] == 13,
          "burst loss: capped concealment then resume");
    CHECK(st->concealed == 4 && st->dropped == 6,
          "burst loss accounting");

    /* 6: companion restart (channel switch): a fresh low sequence after a
       high one is adopted, not locked out. */
    vseq_reset();
    out_n = 0;
    send_pkt(50000); send_pkt(50001);
    send_pkt(0);                        /* new session begins */
    send_pkt(1);
    CHECK(out_n == 4 && out_vals[2] == 0 && out_vals[3] == 1,
          "sequence restart adopted");
    CHECK(st->restarts == 1, "restart counted");

    /* 7: reordering across a larger horizon than the pair. */
    vseq_reset();
    out_n = 0;
    send_pkt(0);
    send_pkt(2); send_pkt(3);           /* two early packets held */
    send_pkt(1);                        /* straggler 20ms late */
    send_pkt(4);
    {
        const int want[] = { 0, 1, 2, 3, 4 };
        CHECK(order_is(want, 5) && st->concealed == 0 && st->dropped == 0,
              "20ms straggler still slots in");
    }

    if (fail) {
        printf("VSEQ TESTS FAILED\n");
        return 1;
    }
    printf("All vseq tests passed.\n");
    return 0;
}
