#pragma once
#include "lifted.h"

unsigned lhx_model_compile_begin(CPU *cpu);
void lhx_model_compile_point(CPU *cpu);
void lhx_model_compile_end(CPU *cpu, unsigned token);
int lhx_model_dispatch(CPU *cpu, uint16_t segment, uint16_t offset);
/* Copy absolute integer model-space XYZ from the retained compiler graph.
 * Returns zero when this allocation/count is not registered. No CPU changes. */
int lhx_model_source_vertices(uint16_t code_segment, uint16_t code_offset,
                              int32_t *xyz, unsigned count);
void lhx_model_reset(void);
