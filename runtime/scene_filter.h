#pragma once
#include "scene_capture.h"

/* Only generated main-image code includes this adapter. CPU helpers, native
 * resource modules and the capture/compositor keep their ordinary stores. */
static inline int lhx_scene_drop_byte(uint32_t address) {
  return g_lhx_scene_enabled && address >= g_lhx_scene_filter_lo &&
         address < g_lhx_scene_filter_hi;
}
static inline void lhx_scene_write8(CPU *cpu, uint16_t segment, uint16_t offset,
                                  uint8_t value) {
  if (!lhx_scene_drop_byte(seg_off(segment, offset)))
    mem_write8(cpu, segment, offset, value);
}
static inline void lhx_scene_write16(CPU *cpu, uint16_t segment, uint16_t offset,
                                   uint16_t value) {
  uint32_t address = seg_off(segment, offset);
  if (!g_lhx_scene_enabled || address + 1 < g_lhx_scene_filter_lo ||
      address >= g_lhx_scene_filter_hi) {
    mem_write16(cpu, segment, offset, value);
    return;
  }
#ifdef RECOMP_MEM_HOOK
  lhx_scene_write8(cpu, segment, offset, (uint8_t)value);
  lhx_scene_write8(cpu, segment, (uint16_t)(offset + 1), (uint8_t)(value >> 8));
#else
  /* Match cpu.h's contiguous word store, including offset FFFF. */
  if (!lhx_scene_drop_byte(address)) cpu->mem[address] = (uint8_t)value;
  if (!lhx_scene_drop_byte(address + 1))
    cpu->mem[address + 1] = (uint8_t)(value >> 8);
#endif
}
static inline void lhx_scene_write32(CPU *cpu, uint16_t segment, uint16_t offset,
                                   uint32_t value) {
  lhx_scene_write16(cpu, segment, offset, (uint16_t)value);
  lhx_scene_write16(cpu, segment, (uint16_t)(offset + 2), (uint16_t)(value >> 16));
}

#define mem_write8 lhx_scene_write8
#define mem_write16 lhx_scene_write16
#define mem_write32 lhx_scene_write32
