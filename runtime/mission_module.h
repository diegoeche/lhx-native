#pragma once
#include "lifted.h"

typedef struct {
    uint16_t offset, size;
} LhxMissionSpan;

typedef struct {
    const char *name;
    const uint8_t *identity;
    unsigned size;
    const RecompFunc *functions;
    unsigned function_count;
    const LhxMissionSpan *identity_spans;
    unsigned identity_span_count;
} LhxMissionModule;

extern const LhxMissionModule g_mission_modules[];
extern const unsigned g_mission_module_count;
int lhx_mission_dispatch(CPU *cpu, uint16_t segment, uint16_t offset);
