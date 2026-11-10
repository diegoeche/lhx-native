/* Dynamic loading adapter for the ahead-of-time compiled PC speaker driver. */
#include "audio_driver.h"
#include "audio_ibm_gen.h"
#include <string.h>

static LhxAudioObserver observer;
static void *observer_data;
static uint32_t voice_serial[LHX_AUDIO_VOICES];

void lhx_audio_observe(LhxAudioObserver callback, void *userdata)
{
    observer = callback;
    observer_data = userdata;
    memset(voice_serial, 0, sizeof voice_serial);
}

static void observe(CPU *cpu, uint16_t segment)
{
    LhxAudioSnapshot snapshot = {0};
    /* These offsets belong only to the SHA-256-gated IBMDRIVE.DRV translated
     * by lift_audio.py: four state pointers/IDs, mute flag and engine control.
     * Read current envelopes; do not advance or otherwise modify the driver. */
    if (!mem_read8(cpu, segment, 0x502)) {
        for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i) {
            unsigned p = mem_read16(cpu, segment, 0x4ed + 2*i);
            unsigned effect = mem_read16(cpu, segment, 0x4f5 + 2*i);
            /* The music sequencer writes note-state pointers directly rather
             * than updating the effect ID used by the ordinary stop API. */
            if (p >= 0xe13 && p < g_audio_ibm_payload_bytes)
                effect = 20 + (p - 0xe13) / 0x2c;
            if (p < 0x827 || p + 0x2c > g_audio_ibm_payload_bytes || effect >= 30)
                continue;
            LhxAudioVoice *voice = &snapshot.voices[i];
            voice->effect = effect;
            voice->serial = voice_serial[i];
            if (effect == 0) {
                /* Effect zero's rate callback updates this rest-period word.
                 * Enhanced synthesis uses it for rotor tempo, not a PIT tone. */
                voice->divisor = mem_read16(cpu, segment, 0x503);
                voice->active = 1;
            } else {
                unsigned divisor = mem_read16(cpu, segment, p);
                unsigned modulation = mem_read16(cpu, segment, p + 4);
                voice->divisor = divisor;
                voice->noise = divisor ? modulation * 255u / divisor : 0;
                if (voice->noise > 255) voice->noise = 255;
                voice->active = divisor && mem_read16(cpu, segment, p + 6);
            }
        }
    }
    observer(&snapshot, observer_data);
}

int lhx_audio_dispatch(CPU *cpu, uint16_t segment, uint16_t offset)
{
    uint32_t base = (uint32_t)segment << 4;
    if (base + g_audio_ibm_payload_bytes > MEM_SIZE ||
        memcmp(cpu->mem + base, g_audio_ibm_prefix, g_audio_ibm_prefix_bytes) != 0)
        return 0;

    unsigned low = 0, high = g_audio_ibm_func_count;
    while (low < high) {
        unsigned middle = low + (high - low) / 2;
        if (g_audio_ibm_funcs[middle].address < offset)
            low = middle + 1;
        else
            high = middle;
    }
    if (low == g_audio_ibm_func_count || g_audio_ibm_funcs[low].address != offset) {
        recomp_unhandled(cpu, segment, offset, "untranslated PC speaker driver target");
        return 1;
    }

    unsigned command = 0xffff, effect = 0xffff;
    if (observer && offset == 0) {
        command = mem_read16(cpu, cpu->ss, cpu->sp + 4);
        effect = mem_read16(cpu, cpu->ss, cpu->sp + 6);
    }
    uint16_t old_cs = cpu->cs, old_ip = cpu->ip;
    cpu->cs = segment;
    cpu->ip = offset;
    g_audio_ibm_funcs[low].func(cpu);
    cpu->cs = old_cs;
    cpu->ip = old_ip;
    if (observer && (offset == 0 || offset == 0x363)) {
        if (command == 0 && effect < 30) {
            unsigned slot = mem_read16(cpu, segment, 0x173 + 8*effect) / 2;
            if (slot < LHX_AUDIO_VOICES) ++voice_serial[slot];
        }
        if (command == 0x10) {
            LhxAudioSnapshot stopped = {0};
            observer(&stopped, observer_data);
        } else observe(cpu, segment);
    }
    return 1;
}
