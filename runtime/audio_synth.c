#include "audio_synth.h"
#include <math.h>
#include <string.h>

static float clamp(float x, float low, float high)
{
    return x < low ? low : x > high ? high : x;
}

void lhx_sound_init(LhxSound *sound, unsigned rate)
{
    memset(sound, 0, sizeof *sound);
    sound->rate = rate >= 8000 && rate <= 192000 ? rate : 48000;
    atomic_init(&sound->received, 0);
    for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i) {
        atomic_init(&sound->targets[i], 0);
        sound->voices[i].random = 0x9e3779b9u ^ (i * 0x85ebca6bu);
    }
    for (unsigned i = 0; i <= 1024; ++i)
        sound->sine[i] = sin(6.283185307179586 * i / 1024);
    sound->attack = 1 - expf(-1.0f / (.002f * sound->rate));
    sound->release = 1 - expf(-1.0f / (.015f * sound->rate));
    sound->pitch_slew = 1 - expf(-1.0f / (.002f * sound->rate));
    sound->noise_slew = 1 - expf(-6.2831853f * 1800 / sound->rate);
    sound->body_decay = expf(-1.0f / (.035f * sound->rate));
}

void lhx_sound_submit(const LhxAudioSnapshot *snapshot, void *userdata)
{
    LhxSound *sound = userdata;
    for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i) {
        const LhxAudioVoice *v = &snapshot->voices[i];
        unsigned gun = v->active && v->effect == 15;
        if (gun && !sound->gun_active[i]) ++sound->gun_serial[i];
        sound->gun_active[i] = gun;
        /* A gun command can extend an already playing burst. Its API serial
         * is not a new envelope; use the driver's actual inactive/active edge. */
        uint32_t serial = v->effect == 15 ? sound->gun_serial[i] : v->serial;
        uint64_t packed = (v->divisor & 65535u) |
            (uint64_t)(v->noise & 255u) << 16 |
            (uint64_t)(v->effect & 31u) << 24 |
            (uint64_t)!!v->active << 29 | (uint64_t)serial << 32;
        atomic_store_explicit(&sound->targets[i], packed, memory_order_relaxed);
    }
    atomic_store_explicit(&sound->received, 1, memory_order_release);
}

static float sine(const LhxSound *sound, double phase)
{
    phase -= floor(phase);
    double at = phase * 1024;
    unsigned i = (unsigned)at;
    return sound->sine[i] + (sound->sine[i+1] - sound->sine[i]) * (float)(at-i);
}

static float noise(LhxSoundVoice *voice)
{
    uint32_t n = voice->random;
    n ^= n << 13; n ^= n >> 17; n ^= n << 5;
    voice->random = n;
    return (float)(n >> 8) / 8388608.0f - 1;
}

void lhx_sound_render(LhxSound *sound, int16_t *out, size_t frames, unsigned channels)
{
    if (!channels) return;
    struct Target { unsigned effect, active; float frequency, noisy; } target[4];
    static const float left[4] = {.72f,.83f,.60f,.72f};
    static const float right[4] = {.72f,.60f,.83f,.72f};
    for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i) {
        uint64_t packed = atomic_load_explicit(&sound->targets[i], memory_order_relaxed);
        unsigned divisor = packed & 65535u, effect = (packed >> 24) & 31u;
        unsigned active = (packed >> 29) & 1u;
        uint32_t serial = packed >> 32;
        LhxSoundVoice *v = &sound->voices[i];
        if (active && (!v->was_active || serial != v->serial || effect != v->effect)) {
            v->body = 1;
            if (effect == 15) v->body_phase = 0;
        }
        if (active) {
            v->serial = serial; v->effect = effect;
            v->noise_amount = (float)((packed >> 16) & 255u) / 255;
        }
        v->was_active = active;
        target[i].effect = v->effect; target[i].active = active;
        target[i].noisy = v->noise_amount;
        /* Empty/muted snapshots fade the previous timbre and pitch; they must
         * not reinterpret a finishing note as effect zero (the engine). */
        if (!active) {
            target[i].frequency = v->frequency;
            continue;
        }
        if (effect == 0) {
            /* Creative timbre; the original engine command sets the tempo. */
            float speed = (65 - clamp((float)divisor, 5, 65)) / 60;
            target[i].frequency = 6 + 14 * speed;
        } else {
            target[i].frequency = divisor ?
                clamp(1193182.0f / divisor, 20, .20f * sound->rate) : 100;
        }
    }
    for (size_t n = 0; n < frames; ++n) {
        float l = 0, r = 0;
        for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i) {
            LhxSoundVoice *v = &sound->voices[i];
            float goal = target[i].active ? 1 : 0;
            v->gain += (goal-v->gain) * (goal > v->gain ? sound->attack : sound->release);
            v->frequency += (target[i].frequency-v->frequency) * sound->pitch_slew;
            v->phase += v->frequency / sound->rate;
            if (v->phase >= 1) v->phase -= 1;
            float random = noise(v);
            v->noise_low += sound->noise_slew * (random-v->noise_low);
            float sample;
            if (target[i].effect == 0) {
                /* A broad blade pulse, low engine harmonics and filtered air. */
                float pulse = .5f + .5f * sine(sound, v->phase);
                pulse *= pulse; pulse *= pulse;
                v->body_phase += (85 + 3*v->frequency) / sound->rate;
                if (v->body_phase >= 1) v->body_phase -= 1;
                sample = (.30f*sine(sound,v->body_phase) +
                          .12f*sine(sound,2*v->body_phase) + .45f*v->noise_low) *
                         (.25f + .75f*pulse) + .16f*sine(sound,v->phase);
            } else if (target[i].effect == 15) {
                /* Creative cannon timbre: a falling low body, a short
                 * broadband crack and a darker pressure tail. Each follows
                 * the observed driver envelope edge, without a free-running
                 * firing clock or a sustained pitched hiss. */
                float body2 = v->body*v->body;
                float crack = body2*body2;
                crack *= crack;
                v->body_phase += (75 + 95*body2) / sound->rate;
                if (v->body_phase >= 1) v->body_phase -= 1;
                sample = 1.35f*v->body*sine(sound,v->body_phase) +
                         .9f*body2*v->noise_low +
                         .55f*crack*(random-v->noise_low);
                /* Smoothly bound this transient to +/-1, retaining mix
                 * headroom even if every slot starts a cannon together. */
                sample /= 1 + .25f*sample*sample;
            } else {
                float pitched = .82f*sine(sound,v->phase) +
                    .18f*sine(sound,2*v->phase);
                float amount = .75f*target[i].noisy;
                sample = pitched*(1-amount) + 1.6f*v->noise_low*amount;
            }
            v->body *= sound->body_decay;
            sample *= .23f*v->gain;
            l += sample*left[i]; r += sample*right[i];
        }
        /* Leave headroom even for simultaneous effects; no wraparound clipping. */
        if (channels == 1) out[n] = (int16_t)(clamp((l+r)*.5f,-.95f,.95f)*22000);
        else {
            out[n*channels] = (int16_t)(clamp(l,-.95f,.95f)*22000);
            out[n*channels+1] = (int16_t)(clamp(r,-.95f,.95f)*22000);
            for (unsigned c = 2; c < channels; ++c) out[n*channels+c] = 0;
        }
    }
}
