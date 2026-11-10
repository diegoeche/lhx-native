#pragma once

/* Interface between the ahead-of-time generated game code and Linux. */
#define RECOMP_IRQ
#include "../third_party/pcrecomp/runtime/cpu.h"

typedef struct {
  uint32_t address;
  void (*func)(CPU *);
} RecompFunc;
extern const uint16_t g_load_seg, g_psp_seg;
extern const uint16_t g_entry_cs, g_entry_ip, g_entry_ss, g_entry_sp;
extern const unsigned g_func_count, g_image_len, g_nrelocs;
extern const RecompFunc g_funcs[];
extern const uint8_t g_image[];
extern const uint16_t g_relocs[][2];

void int_handler(CPU *, unsigned);
void dos_int21(CPU *);
void bios_int10(CPU *);
void bios_int16(CPU *);
void mouse_int33(CPU *);
void lhx_int86(CPU *);
void lhx_project(CPU *);
void lhx_frame_pace(CPU *);
/* Main-loop instruction observer, before the original TOW save/clear. */
void lhx_gamepad_apply(CPU *);
extern int g_lhx_scene_enabled;
void lhx_scene_event(CPU *, unsigned);
unsigned lhx_model_compile_begin(CPU *);
void lhx_model_compile_end(CPU *, unsigned);
void lhx_model_compile_point(CPU *);
int lhx_model_dispatch(CPU *, uint16_t, uint16_t);
uint8_t port_in8(CPU *, uint16_t);
uint16_t port_in16(CPU *, uint16_t);
void port_out8(CPU *, uint16_t, uint8_t);
void port_out16(CPU *, uint16_t, uint16_t);
void recomp_dispatch(CPU *, uint16_t, uint16_t);
void dispatch_near(CPU *, uint16_t, uint16_t);
void dispatch_far(CPU *, uint16_t, uint16_t);
int recomp_div0(const char *, uint16_t);
void recomp_unhandled(CPU *, uint16_t, uint16_t, const char *);
int cpu_lar(CPU *, uint16_t, uint16_t *);
int cpu_lsl(CPU *, uint16_t, uint16_t *);

#include "recomp_all.h"
