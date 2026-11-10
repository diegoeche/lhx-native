#pragma once
#include "audio_driver.h"
#include <stdatomic.h>
#include <stddef.h>

typedef struct {
    double phase, body_phase;
    float gain, frequency, noise_low, body, noise_amount;
    uint32_t random, serial;
    unsigned effect, was_active;
} LhxSoundVoice;
typedef struct {
    /* Producer publishes each voice atomically; all DSP state belongs only to
     * the consumer. The callback takes no lock and allocates no memory. */
    atomic_uint_fast64_t targets[LHX_AUDIO_VOICES];
    atomic_int received;
    /* Simulation-thread state: retain gun envelope edges even when the brief
     * inactive snapshot falls entirely between two audio callbacks. */
    uint32_t gun_serial[LHX_AUDIO_VOICES];
    unsigned gun_active[LHX_AUDIO_VOICES];
    LhxSoundVoice voices[LHX_AUDIO_VOICES];
    float sine[1025];
    float attack, release, pitch_slew, noise_slew, body_decay;
    unsigned rate;
} LhxSound;

void lhx_sound_init(LhxSound *, unsigned sample_rate);
void lhx_sound_submit(const LhxAudioSnapshot *, void *sound);
void lhx_sound_render(LhxSound *, int16_t *output, size_t frames, unsigned channels);
