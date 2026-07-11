#include "voice.h"

#include <psp2/audioout.h>
#include <psp2/net/net.h>
#include <psp2/kernel/threadmgr.h>

#include <stdint.h>
#include <string.h>

#define VOICE_PORT     9102
#define SAMPLE_RATE    48000
#define GRAIN          512          /* samples per output call, ~10.7 ms */
#define RING_SAMPLES   48000        /* 1 s of mono s16 */
#define PRIME_SAMPLES  (GRAIN * 4)  /* cushion before playback starts */
#define MAX_BUFFERED   24000        /* cap latency ~0.5 s: drop older audio */

static int audio_port = -1;
static int udp_sock = -1;
static SceUID voice_thread = -1;
static volatile int running = 0;
static volatile int started = 0;

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

static int voice_thread_entry(SceSize args, void *argp)
{
    (void)args; (void)argp;
    /* We stream and buffer mono, but play through a stereo port (the most
       broadly supported mode): each mono sample is duplicated to L and R. */
    int16_t out[GRAIN * 2];
    uint8_t buf[1600];
    int primed = 0;

    while (running) {
        /* Drain whatever UDP has arrived (non-blocking). Bounded so a flood
           can't starve the output call below. */
        for (int guard = 0; guard < 32; guard++) {
            int n = sceNetRecvfrom(udp_sock, buf, sizeof(buf), 0, NULL, NULL);
            if (n <= 0)
                break;
            int samples = n / 2;
            const int16_t *s = (const int16_t *)buf;
            for (int i = 0; i < samples; i++)
                ring_push(s[i]);
        }

        /* Keep latency bounded if the PC runs slightly ahead of us. */
        while (ring_count() > MAX_BUFFERED)
            r_head = (r_head + GRAIN) % RING_SAMPLES;

        if (!primed && ring_count() >= PRIME_SAMPLES)
            primed = 1;

        int f = 0;
        if (primed)
            for (; f < GRAIN && ring_count() > 0; f++) {
                int16_t s = ring_pop();
                out[2 * f] = s;
                out[2 * f + 1] = s;
            }
        for (; f < GRAIN; f++) {       /* silence fill / underrun */
            out[2 * f] = 0;
            out[2 * f + 1] = 0;
        }
        if (primed && ring_count() == 0)
            primed = 0;               /* re-prime after we run dry */

        sceAudioOutOutput(audio_port, out);   /* blocks: this is our clock */
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

    r_head = r_tail = 0;
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
