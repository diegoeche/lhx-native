#pragma once
#include "controller_model.h"

typedef struct {
  int enabled, page, selection, footer_focus, binding, binding_scroll, device_cursor, reassign, import_prompt;
  int editing, collisions, resolution, collision_scroll, monitor, candidate_count;
  LhxUsbRuntime live;
  LhxUsbCandidate candidates[LHX_USB_CANDIDATES];
  LhxUsbBinding proposal;
  LhxCalibration calibration;
  int cal_return, cal_bits, cal_flip, endpoints[2], monitor_device, samples[32], sample_count, sample_next;
  Uint32 sample_time[32], last_sample, capture_time;
  int axes[LHX_USB_DEVICES][32], baseline[LHX_USB_DEVICES][32], reported[LHX_USB_DEVICES][32];
  Uint8 buttons[LHX_USB_DEVICES][80], ready[LHX_USB_DEVICES][80], axis_ready[LHX_USB_DEVICES][32];
  char signal[3][180];
  int signals;
  LhxHotasPreset hotas;
  int hotas_page;
} LhxFlexibleSetup;
struct LhxControllerSetup;
void lhx_flex_init(struct LhxControllerSetup *);
void lhx_flex_close(struct LhxControllerSetup *);
void lhx_flex_tick(struct LhxControllerSetup *);
void lhx_flex_event(struct LhxControllerSetup *,const SDL_Event *);
void lhx_flex_draw(const struct LhxControllerSetup *,SDL_Renderer *,const char *);
void lhx_flex_bindings(const struct LhxControllerSetup *,int,char *,size_t);
int lhx_flex_hotas_offer(const struct LhxControllerSetup *);
void lhx_flex_hotas_preset(struct LhxControllerSetup *);
/* Same existing bitmap UI primitives; no additional font or renderer. */
void lhx_ui_color(SDL_Renderer *,int,int,int);
void lhx_ui_rect(SDL_Renderer *,int,int,int,int);
void lhx_ui_text(SDL_Renderer *,int,int,const char *,int);
void lhx_ui_wrapped(SDL_Renderer *,int,int,const char *,int,int);
void lhx_ui_button(SDL_Renderer *,int,int,int,const char *,int);
