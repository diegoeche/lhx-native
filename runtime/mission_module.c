/* Dispatch already-compiled callbacks after the original mission loader runs. */
#include "mission_module.h"
#include <string.h>

int lhx_mission_dispatch(CPU *cpu, uint16_t segment, uint16_t offset)
{
    uint32_t base = (uint32_t)segment << 4;
    for (unsigned i = 0; i < g_mission_module_count; i++) {
        const LhxMissionModule *module = &g_mission_modules[i];
        if (base + module->size > MEM_SIZE)
            continue;
        unsigned span;
        for (span = 0; span < module->identity_span_count; span++) {
            unsigned start = module->identity_spans[span].offset;
            unsigned size = module->identity_spans[span].size;
            if (memcmp(cpu->mem + base + start, module->identity + start, size) != 0)
                break;
        }
        if (span != module->identity_span_count)
            continue;
        /* Saved DS and authored mission counters/rewards remain mutable data.
         * Every reachable instruction and dense switch table stays verified.
         */
        for (unsigned j = 0; j < module->function_count; j++) {
            if (module->functions[j].address != offset)
                continue;
            uint16_t old_cs = cpu->cs, old_ip = cpu->ip;
            cpu->cs = segment;
            cpu->ip = offset;
            module->functions[j].func(cpu);
            cpu->cs = old_cs;
            cpu->ip = old_ip;
            return 1;
        }
        recomp_unhandled(cpu, segment, offset, "untranslated native mission callback");
        return 1;
    }
    return 0;
}
