#pragma once
#include "lifted.h"

typedef struct {
    const char *name;
    const uint8_t *identity;
    unsigned size;
    uint16_t entry;
    void (*draw)(CPU *);
} LhxCockpitModule;

extern const LhxCockpitModule g_cockpit_modules[];
extern const unsigned g_cockpit_module_count;
int lhx_cockpit_dispatch(CPU *cpu, uint16_t segment, uint16_t offset);
