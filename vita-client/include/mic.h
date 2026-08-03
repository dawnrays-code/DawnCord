#ifndef DAWNCORD_MIC_H
#define DAWNCORD_MIC_H

#include <stdint.h>

/* Microphone capture and uplink.

   The mirror of voice.c: one thread owns the input port and the socket,
   sceAudioInInput blocks for a grain and is therefore the clock, and each
   return becomes one UDP datagram to the companion on port 9103. No
   codec, no crypto and no 20ms framing happens here; the console stays a
   dumb capture device exactly as it is a dumb playback device.

   Nothing is captured until mic_set_muted(0) is called: the microphone is
   muted from the moment the port opens, every time, so it can never come
   up live by accident.

   This build wires the module in and proves it links. Capture itself
   lands in the next one. */

/* 0 on success; -1 audio port, -2 socket, -3 thread. */
int mic_start(uint32_t companion_ip_be);
void mic_stop(void);
int mic_active(void);

/* Muted until explicitly unmuted, and muted again by mic_stop. */
void mic_set_muted(int muted);
int mic_is_muted(void);

/* The system-wide microphone mute, which the user can set outside this
   app entirely. Reported so the interface can say why nothing is going
   out. */
int mic_hw_muted(void);

/* Whatever the capture port actually accepted, valid after mic_start:
   the grain and rate combination is negotiated from a fallback table
   because the hardware documentation disagrees with itself. */
int mic_rate(void);
int mic_grain(void);

/* cap, sent, gated, fail, open_idx, open_err, peak, rms */
void mic_get_stats(uint32_t out[8]);

#endif
