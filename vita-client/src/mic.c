#include "mic.h"
#include "mic_frame.h"

#include <psp2/audioin.h>

/* Capture is not built yet: this compiles, links and reports itself as
   inactive, which is what proves SceAudioIn_stub resolves on the build
   container before any hardware behaviour is at stake. The next build
   fills in the thread.

   Muted is the initial state and the only state this version has. */
static int muted = 1;

int mic_start(uint32_t companion_ip_be)
{
    (void)companion_ip_be;
    return -1;                 /* no capture yet */
}

void mic_stop(void)
{
    muted = 1;
}

int mic_active(void)
{
    return 0;
}

void mic_set_muted(int m)
{
    muted = m ? 1 : 0;
}

int mic_is_muted(void)
{
    return muted;
}

int mic_hw_muted(void)
{
    /* The system-wide toggle, readable whether or not a port is open.
       Anything other than a clean zero is treated as muted: a failure to
       ask must never read as "the microphone is live". */
    return sceAudioInGetStatus(SCE_AUDIO_IN_GETSTATUS_MUTE) != 0;
}

int mic_rate(void)
{
    return 0;
}

int mic_grain(void)
{
    return 0;
}

void mic_get_stats(uint32_t out[8])
{
    for (int i = 0; i < 8; i++)
        out[i] = 0;
}
