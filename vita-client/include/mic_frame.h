#ifndef DAWNCORD_MIC_FRAME_H
#define DAWNCORD_MIC_FRAME_H

#include <stdint.h>
#include <stddef.h>

/* Uplink datagram builder: turns one captured grain into the bytes that
   go out on the wire, and nothing else.

   Pure by design. No psp2 header, no allocation, and every piece of state
   belongs to the caller, so the same object file links into the console
   app today and into a taiHEN plugin later, where there is no app around
   it. That is the one thing voice_seq.c gets wrong: its state is file
   static, so it cannot be instantiated twice or reused elsewhere.

   Wire format:
     [4 bytes BE sequence]
     [2 bytes BE flags]
     [12 x 2 bytes BE recent playback RMS, oldest first]
     [grain * 2 bytes PCM, signed 16-bit LITTLE endian]

   The mixed endianness matches the downlink, where the console reads the
   sequence big-endian and casts the payload as native little-endian.

   The playback RMS slots carry what the console's speakers were doing
   over the recent past. The companion needs that to line its echo
   reference up with what the microphone actually heard: it knows what it
   sent, but not when the console played it. A muted or silent grain
   carries the header alone. */

#define MIC_REF_SLOTS      12
#define MIC_HEADER_BYTES   (4 + 2 + MIC_REF_SLOTS * 2)   /* 30 */

#define MIC_FLAG_RATE_MASK 0x0003   /* 0 = 16000 Hz, 1 = 48000 Hz */
#define MIC_FLAG_SPK_MUTED 0x0004   /* console playback silenced */
#define MIC_FLAG_MIC_MUTED 0x0008   /* user muted the microphone */
#define MIC_FLAG_HW_MUTED  0x0010   /* system-wide mic mute is on */
#define MIC_FLAG_GATE_OPEN 0x0020   /* near-end gate says this is speech */
#define MIC_FLAG_SILENT    0x0040   /* no payload: nothing worth sending */
#define MIC_FLAG_REF_BAD   0x0080   /* reference slots not trustworthy */

typedef struct {
    uint32_t seq;
    int rate_code;      /* 0 or 1, mirrors MIC_FLAG_RATE_MASK */
    int spk_muted;
    int mic_muted;
    int hw_muted;
    int gate_open;
    int ref_bad;
    uint32_t cap;       /* grains captured */
    uint32_t sent;      /* datagrams built */
    uint32_t gated;     /* grains withheld as silence */
    int32_t peak;       /* last grain, absolute peak sample */
    uint32_t rms;       /* last grain, root mean square */
} mic_frame_state;

void mic_frame_reset(mic_frame_state *s, int rate_hz);

/* Builds one datagram into out. Returns the byte count, or 0 when out is
   too small (a refusal, never a truncated frame). */
int mic_frame_build(mic_frame_state *s, const int16_t *pcm, int n,
                    const uint16_t ref[MIC_REF_SLOTS],
                    uint8_t *out, size_t out_size);

void mic_frame_level(const int16_t *pcm, int n, int32_t *peak, uint32_t *rms);

#endif
