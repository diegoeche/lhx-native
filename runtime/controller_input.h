#pragma once
#include "controls.h"

/* One opened device, one stable interpretation. Native hats are digital inputs. */
typedef struct {
  SDL_GameController *pad;
  SDL_Joystick *joy;
  int raw, axes, buttons, hats;
} LhxControllerInput;
/* Count distinct meaningful axis changes in one coherent poll batch. */
int lhx_input_axis_batch(const int *before,const int *after,const Uint8 *ready,int count,int threshold,int *last);
int lhx_input_eligible(int index);
int lhx_input_open(LhxControllerInput *, int index, LhxInputMode);
int lhx_input_open_explicit(LhxControllerInput *, int index, int mapped);
void lhx_input_close(LhxControllerInput *);
int lhx_input_open_selected(LhxControllerInput *, const LhxControls *);
int lhx_input_attached(const LhxControllerInput *);
int lhx_input_axis(const LhxControllerInput *, int);
int lhx_input_button(const LhxControllerInput *, int);
int lhx_input_unipolar(const LhxControllerInput *, int);
/* Published Linux raw layout only; never infer roles from a device name. */
int lhx_input_hotas_match(int raw, unsigned vendor, unsigned product, int axes, int buttons, int hats);
int lhx_input_hotas_one(const LhxControllerInput *);
void lhx_input_axis_label(const LhxControllerInput *, int, char *, size_t);
SDL_GameControllerButtonBind lhx_input_bind(const LhxControllerInput *, int axis, int input);
void lhx_input_select(const LhxControllerInput *, LhxControls *);
LhxControlAction *lhx_input_buttons(const LhxControllerInput *, LhxControls *);
LhxAxisBinding *lhx_input_axes(const LhxControllerInput *, LhxControls *);
