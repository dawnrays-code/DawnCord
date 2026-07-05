#ifndef DAWNCORD_VOICE_H
#define DAWNCORD_VOICE_H

/* Listen-only voice playback (tentative). The companion decodes Discord
   voice to mono 48 kHz PCM on the PC and streams it here as raw UDP; this
   opens the audio port and a socket on port 9102 and plays whatever
   arrives. All the hard parts (Opus, crypto, mixing) stay on the PC.

   One thread owns the socket, the ring buffer and the audio output, so
   there is no locking. voice_start returns 0 on success. Safe to call
   voice_stop when not started. */

int voice_start(void);
void voice_stop(void);
int voice_active(void);

#endif
