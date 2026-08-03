#include "voice.h"
#include "voice_seq.h"

#include <psp2/audioout.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#define VOICE_PORT     9102
#define SAMPLE_RATE    48000
#define GRAIN          512          /* samples per output call, ~10.7 ms */
#define RING_SAMPLES   48000        /* 1 s of mono s16 */
/* Buffer depths. 60ms was too thin for Wi-Fi and made the crackle worse,
   so the cushion goes back up: the latency it costs is not noticeable in
   conversation, and underruns very much are. */
#define PRIME_SAMPLES  5760         /* ~120ms cushion before playback starts */
#define MAX_BUFFERED   12000        /* ~250ms: above this, shave gently */
#define RESYNC_SAMPLES 28800        /* ~600ms: too far gone, jump back */
#define DRY_GRAINS     6            /* run dry this often before re-priming */

static int audio_port = -1;
static int udp_sock = -1;
static SceUID voice_thread = -1;
static volatile int running = 0;
static volatile int started = 0;
static volatile int muted = 0;

void voice_set_muted(int m)
{
    muted = m ? 1 : 0;
}

int voice_is_muted(void)
{
    return muted;
}

/* Link counters, reported to the companion so its log finally shows the
   receive side of the Wi-Fi. Ordering, holds and concealment live in
   voice_seq.c, which the host test exercises with the wire scenarios
   measured on hardware. */
static volatile uint32_t st_rx = 0, st_under = 0;
static volatile uint32_t st_resync = 0, st_trim = 0;

void voice_get_stats(uint32_t out[7])
{
    const vseq_stats *vs = vseq_get_stats();
    out[0] = st_rx;
    out[1] = vs->dropped;
    out[2] = st_under;
    out[3] = st_resync;
    out[4] = st_trim;
    out[5] = vs->reordered;
    out[6] = vs->concealed;
}

/* Ring buffer, touched only by the voice thread once running. */
static int16_t ring[RING_SAMPLES];
static int r_head = 0, r_tail = 0;

static int ring_count(void)
{
    return (r_tail - r_head + RING_SAMPLES) % RING_SAMPLES;
}

static void ring_push(int16_t s)
{
    int next = (r_tail + 1) % RING_SAMPLES;
    if (next == r_head)                   /* full: drop oldest sample */
        r_head = (r_head + 1) % RING_SAMPLES;
    ring[r_tail] = s;
    r_tail = next;
}

static int16_t ring_pop(void)
{
    if (r_head == r_tail)
        return 0;
    int16_t s = ring[r_head];
    r_head = (r_head + 1) % RING_SAMPLES;
    return s;
}

static void ring_push_bulk(const int16_t *pcm, int n)
{
    for (int i = 0; i < n; i++)
        ring_push(pcm[i]);
}

static int voice_thread_entry(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* We stream and buffer mono, but play through a stereo port (the most
       broadly supported mode): each mono sample is duplicated to L and R.
       Two output buffers, alternated: refilling the single buffer while
       the driver might still be reading it was a standing suspect for
       hardware-only artifacts, and every mainstream Vita audio backend
       double-buffers. */
    int16_t out[2][GRAIN * 2];
    int ob = 0;
    uint8_t buf[1600];
    int primed = 0, dry = 0;
    int16_t last = 0;               /* for the decay-to-silence fill */

    while (running) {
        /* Drain whatever UDP has arrived (non-blocking). Bounded so a flood
           can't starve the output call below. Each packet is
           [4-byte seq][PCM]; out-of-order or duplicate packets are dropped
           by comparing the sequence with wrap-around. */
        for (int guard = 0; guard < 48; guard++) {
            int n = sceNetRecvfrom(udp_sock, buf, sizeof(buf), 0, NULL, NULL);
            if (n < 0)
                break;              /* nothing more queued */
            if (n < 6)
                continue;           /* header + at least one sample */
            uint32_t seq = ((uint32_t)buf[0] << 24) | ((uint32_t)buf[1] << 16) |
                           ((uint32_t)buf[2] << 8) | (uint32_t)buf[3];
            st_rx++;
            vseq_packet(seq, (const int16_t *)(buf + 4), (n - 4) / 2,
                        ring_push_bulk);
        }

        /* Clock drift correction. The PC's audio clock and this console's
           are independent, so the buffer creeps in one direction forever.
           Cutting a whole grain when it got too full (what this used to
           do) removes 10ms of waveform in one step, and a step is a click:
           that was the periodic crackle. Instead, shave a SINGLE sample
           per grain, which is a 0.2% speed change, well under audible, and
           enough to cancel any realistic drift. */
        int level = ring_count();
        int trim = 0;
        if (level > RESYNC_SAMPLES) {
            /* Something big went wrong (a burst, a stall). Jump to the
               target depth once rather than grinding down a sample at a
               time for the next minute. */
            r_head = (r_tail - PRIME_SAMPLES + RING_SAMPLES) % RING_SAMPLES;
            st_resync++;
        } else if (level > MAX_BUFFERED) {
            trim = 1;
            st_trim++;
        }

        if (!primed && ring_count() >= PRIME_SAMPLES)
            primed = 1;

        /* Muted: keep draining the socket so we do not come back to a
           minute-old backlog, but play nothing. */
        if (muted) {
            r_head = r_tail;
            primed = 0;
            last = 0;
            memset(out[ob], 0, sizeof(out[0]));
            sceAudioOutOutput(audio_port, out[ob]);
            ob ^= 1;
            continue;
        }

        int f = 0;
        if (primed)
            for (; f < GRAIN && ring_count() > 0; f++) {
                last = ring_pop();
                /* Shaving a sample by discarding it leaves a one-sample
                   step, and 93 steps a second is heard as dirt on the top
                   end. Merging the pair into their average removes the
                   sample with no discontinuity at all. */
                if (trim && ring_count() > 0) {
                    int16_t nxt = ring_pop();
                    last = (int16_t)(((int)last + (int)nxt) / 2);
                    trim = 0;
                }
                out[ob][2 * f] = last;
                out[ob][2 * f + 1] = last;
            }
        /* Underrun fill. Jumping straight to zero puts a step in the
           waveform and a step is a click, which is most of what "crackly"
           actually is; decaying the last sample instead lands on silence
           smoothly. A PARTIAL grain means we ran dry mid-stream, which is
           the link stumbling, not speech ending: count it. */
        if (f > 0 && f < GRAIN)
            st_under++;
        for (; f < GRAIN; f++) {
            last = (int16_t)(last - last / 8);
            out[ob][2 * f] = last;
            out[ob][2 * f + 1] = last;
        }

        /* Only treat a gap as the end of speech after several empty
           grains. A single late packet used to cost a full re-prime, which
           swallowed the start of the next word. */
        if (ring_count() == 0) {
            if (++dry >= DRY_GRAINS) {
                primed = 0;
                last = 0;
            }
        } else {
            dry = 0;
        }

        sceAudioOutOutput(audio_port, out[ob]);  /* blocks: this is our clock */
        ob ^= 1;
    }
    return sceKernelExitDeleteThread(0);
}

int voice_start(void)
{
    if (started)
        return 0;

    audio_port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_MAIN, GRAIN,
                                     SAMPLE_RATE, SCE_AUDIO_OUT_MODE_STEREO);
    if (audio_port < 0)
        return -1;   /* audio port */
    int vol[2] = { SCE_AUDIO_OUT_MAX_VOL, SCE_AUDIO_OUT_MAX_VOL };
    sceAudioOutSetVolume(audio_port,
                         SCE_AUDIO_VOLUME_FLAG_L_CH | SCE_AUDIO_VOLUME_FLAG_R_CH,
                         vol);

    /* sceNet is already up (the TCP connection initialised it). */
    udp_sock = sceNetSocket("dawncord_voice", SCE_NET_AF_INET,
                            SCE_NET_SOCK_DGRAM, 0);
    if (udp_sock < 0) {
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
        return -2;   /* socket */
    }
    /* Fast leave/rejoin reuses the port before the old socket fully dies:
       without REUSEADDR the bind fails ("Voice: network error"). */
    int reuse = 1;
    sceNetSetsockopt(udp_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_REUSEADDR,
                     &reuse, sizeof(reuse));
    SceNetSockaddrIn addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = SCE_NET_AF_INET;
    addr.sin_port = sceNetHtons(VOICE_PORT);
    addr.sin_addr.s_addr = SCE_NET_INADDR_ANY;
    if (sceNetBind(udp_sock, (SceNetSockaddr *)&addr, sizeof(addr)) < 0) {
        sceNetSocketClose(udp_sock);
        sceAudioOutReleasePort(audio_port);
        udp_sock = audio_port = -1;
        return -2;   /* socket */
    }
    int nbio = 1;
    sceNetSetsockopt(udp_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_NBIO,
                     &nbio, sizeof(nbio));
    /* Roomy receive buffer: a short send burst shouldn't drop packets
       while the audio thread is blocked in sceAudioOutOutput. */
    int rcvbuf = 128 * 1024;
    sceNetSetsockopt(udp_sock, SCE_NET_SOL_SOCKET, SCE_NET_SO_RCVBUF,
                     &rcvbuf, sizeof(rcvbuf));

    r_head = r_tail = 0;
    vseq_reset();
    st_rx = st_under = st_resync = st_trim = 0;   /* per-session counters */
    /* Flush datagrams a previous session left queued: one stale packet
       used to poison the sequence and mute the whole new session. */
    {
        uint8_t junk[1600];
        while (sceNetRecvfrom(udp_sock, junk, sizeof(junk), 0, NULL, NULL) >= 0)
            ;
    }
    running = 1;
    /* Same priority as the network receiver: a known-good value (a too-low
       relative priority makes CreateThread fail). */
    voice_thread = sceKernelCreateThread("dawncord_voice", voice_thread_entry,
                                         0x10000100, 0x8000, 0, 0, NULL);
    if (voice_thread < 0) {
        running = 0;
        sceNetSocketClose(udp_sock);
        sceAudioOutReleasePort(audio_port);
        udp_sock = audio_port = -1;
        return -3;   /* thread */
    }
    sceKernelStartThread(voice_thread, 0, NULL);
    started = 1;
    return 0;
}

void voice_stop(void)
{
    if (!started)
        return;
    running = 0;
    if (voice_thread >= 0) {
        sceKernelWaitThreadEnd(voice_thread, NULL, NULL);
        voice_thread = -1;
    }
    if (udp_sock >= 0) {
        sceNetSocketClose(udp_sock);
        udp_sock = -1;
    }
    if (audio_port >= 0) {
        sceAudioOutReleasePort(audio_port);
        audio_port = -1;
    }
    started = 0;
}

int voice_active(void)
{
    return started;
}
