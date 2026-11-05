/* Dynamic loading adapter for the ahead-of-time compiled PC speaker driver. */
#include "audio_driver.h"
#include "audio_ibm_gen.h"
#include <string.h>

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

    uint16_t old_cs = cpu->cs, old_ip = cpu->ip;
    cpu->cs = segment;
    cpu->ip = offset;
    g_audio_ibm_funcs[low].func(cpu);
    cpu->cs = old_cs;
    cpu->ip = old_ip;
    return 1;
}
