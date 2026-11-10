#pragma once
#include "lifted.h"

/* Return 0 for a different module. A recognized module's unknown code target
 * fails explicitly; it is never interpreted or treated as a successful call.
 */
int lhx_audio_dispatch(CPU *cpu, uint16_t segment, uint16_t offset);

enum { LHX_AUDIO_VOICES = 4 };
typedef struct {
    unsigned divisor, effect, noise, active;
    uint32_t serial;
} LhxAudioVoice;
typedef struct { LhxAudioVoice voices[LHX_AUDIO_VOICES]; } LhxAudioSnapshot;
/* Optional read-only observation of the supported original driver. Called on
 * the simulation thread; the audio callback must never read guest memory. */
typedef void (*LhxAudioObserver)(const LhxAudioSnapshot *, void *);
void lhx_audio_observe(LhxAudioObserver observer, void *userdata);
