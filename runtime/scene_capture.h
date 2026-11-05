#pragma once
#include "lifted.h"
#include "scene_types.h"

extern int g_lhx_scene_enabled;
extern int g_lhx_scene_world_enabled;
/* Physical interval of original world pixel stores to suppress. Empty at rest. */
extern uint32_t g_lhx_scene_filter_lo, g_lhx_scene_filter_hi;

void lhx_scene_event(CPU *cpu, unsigned image_offset);
const LhxSceneFrame *lhx_scene_frame(void);
void lhx_scene_reset(void);
