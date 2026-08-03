#include "voice_seq.h"

#include <string.h>

/* Packets are nominally 480 samples (10ms); tolerate anything the socket
   buffer can carry. */
#define VSEQ_MAX_SAMPLES 800
#define VSEQ_HOLD 3          /* early packets parked while a straggler runs */
#define VSEQ_RESTART_GAP 3000 /* backward jump bigger than this = new stream */
#define VSEQ_CONCEAL_MAX 4   /* longest gap patched before giving up */

typedef struct {
    int used;
    uint32_t seq;
    int n;
    int16_t pcm[VSEQ_MAX_SAMPLES];
} vseq_slot;

static vseq_slot hold[VSEQ_HOLD];
static uint32_t next_seq = 0;      /* the packet the ring wants next */
static int have_seq = 0;
static vseq_stats st;

/* The tail of what was last played, for concealment. */
static int16_t hist[VSEQ_MAX_SAMPLES];
static int hist_n = 0;

void vseq_reset(void)
{
    memset(hold, 0, sizeof(hold));
    memset(&st, 0, sizeof(st));
    have_seq = 0;
    hist_n = 0;
}

const vseq_stats *vseq_get_stats(void)
{
    return &st;
}

static void deliver(const int16_t *pcm, int n, vseq_push_fn push)
{
    push(pcm, n);
    if (n > VSEQ_MAX_SAMPLES)
        n = VSEQ_MAX_SAMPLES;
    memcpy(hist, pcm, (size_t)n * sizeof(int16_t));
    hist_n = n;
    st.delivered++;
}

/* A missing packet becomes a faded repeat of the last audio rather than
   an excision: the waveform stays continuous and, crucially, the cushion
   keeps the 10ms it would otherwise lose forever. Successive repeats
   fade harder so a long gap melts toward silence instead of stuttering. */
static void conceal_one(int step, vseq_push_fn push)
{
    static const int num[VSEQ_CONCEAL_MAX] = { 3, 2, 1, 1 };
    static const int den[VSEQ_CONCEAL_MAX] = { 4, 4, 4, 8 };
    int16_t out[VSEQ_MAX_SAMPLES];
    int n = hist_n > 0 ? hist_n : 480;

    if (step >= VSEQ_CONCEAL_MAX)
        step = VSEQ_CONCEAL_MAX - 1;
    for (int i = 0; i < n; i++) {
        int v = hist_n > 0 ? hist[i] : 0;
        out[i] = (int16_t)(v * num[step] / den[step]);
    }
    push(out, n);
    st.concealed++;
}

/* Hand over every parked packet that is now consecutive. */
static void flush_hold(vseq_push_fn push)
{
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (int i = 0; i < VSEQ_HOLD; i++) {
            if (hold[i].used && hold[i].seq == next_seq) {
                deliver(hold[i].pcm, hold[i].n, push);
                st.reordered++;
                hold[i].used = 0;
                next_seq++;
                progressed = 1;
            }
        }
    }
}

void vseq_packet(uint32_t seq, const int16_t *pcm, int n, vseq_push_fn push)
{
    if (n <= 0 || n > VSEQ_MAX_SAMPLES)
        return;

    if (!have_seq) {
        have_seq = 1;
        next_seq = seq + 1;
        deliver(pcm, n, push);
        return;
    }

    int32_t delta = (int32_t)(seq - next_seq);

    if (delta < 0) {
        /* Older than what the ring already wants. A huge backward jump is
           the companion starting a fresh session (its counter restarts at
           zero); adopting it instead of dropping is what prevents the
           channel-switch lockout where a stale packet poisoned the
           sequence and everything after it got discarded. */
        if (delta < -VSEQ_RESTART_GAP) {
            st.restarts++;
            memset(hold, 0, sizeof(hold));
            next_seq = seq + 1;
            deliver(pcm, n, push);
        } else {
            st.dropped++;
        }
        return;
    }

    if (delta == 0) {
        deliver(pcm, n, push);
        next_seq++;
        flush_hold(push);
        return;
    }

    /* Early: the packets between next_seq and seq are still in flight.
       Park this one. If the parking lot is full or the gap is too wide
       to ever heal, give the missing ones up: conceal them and move on. */
    int free_slot = -1, held = 0;
    for (int i = 0; i < VSEQ_HOLD; i++) {
        if (hold[i].used)
            held++;
        else
            free_slot = i;
    }

    if (delta <= VSEQ_HOLD && free_slot >= 0) {
        vseq_slot *s = &hold[free_slot];
        s->used = 1;
        s->seq = seq;
        s->n = n;
        memcpy(s->pcm, pcm, (size_t)n * sizeof(int16_t));
        (void)held;
        return;
    }

    /* Gap too wide or lot full: the stragglers are lost. Patch the hole
       up to the first parked packet (or this one), FLUSH the lot so its
       slots free up, and only then handle the packet that triggered all
       this as a fresh arrival: parking it first silently lost it
       whenever the lot was full (the host test caught that). */
    uint32_t resume = seq;
    for (int i = 0; i < VSEQ_HOLD; i++) {
        if (hold[i].used && (int32_t)(hold[i].seq - resume) < 0)
            resume = hold[i].seq;
    }
    int missing = (int32_t)(resume - next_seq);
    for (int k = 0; k < missing && k < VSEQ_CONCEAL_MAX; k++)
        conceal_one(k, push);
    if (missing > VSEQ_CONCEAL_MAX)
        st.dropped += (uint32_t)(missing - VSEQ_CONCEAL_MAX);
    next_seq = resume;
    flush_hold(push);

    delta = (int32_t)(seq - next_seq);
    if (delta == 0) {
        deliver(pcm, n, push);
        next_seq++;
        flush_hold(push);
    } else if (delta > 0) {
        for (int i = 0; i < VSEQ_HOLD; i++) {
            if (!hold[i].used) {
                hold[i].used = 1;
                hold[i].seq = seq;
                hold[i].n = n;
                memcpy(hold[i].pcm, pcm, (size_t)n * sizeof(int16_t));
                break;
            }
        }
    } else {
        st.dropped++;      /* defensive: cannot normally happen */
    }
}
