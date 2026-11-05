/* Select an ahead-of-time compiled VGA blitter after the original loader. */
#include "cockpit_module.h"
#include <string.h>

int lhx_cockpit_dispatch(CPU *cpu, uint16_t segment, uint16_t offset)
{
    uint32_t base = (uint32_t)segment << 4;
    for (unsigned i = 0; i < g_cockpit_module_count; i++) {
        const LhxCockpitModule *module = &g_cockpit_modules[i];
        if (base + module->size > MEM_SIZE ||
            memcmp(cpu->mem + base, module->identity, module->size) != 0)
            continue;
        if (offset != module->entry) {
            recomp_unhandled(cpu, segment, offset, "untranslated native cockpit callback");
            return 1;
        }
        uint16_t old_cs = cpu->cs, old_ip = cpu->ip;
        cpu->cs = segment;
        cpu->ip = offset;
        module->draw(cpu);
        cpu->cs = old_cs;
        cpu->ip = old_ip;
        return 1;
    }
    return 0;
}
