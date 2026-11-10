#include "controller_input.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

int lhx_input_hotas_match(int raw,unsigned vendor,unsigned product,int axes,int buttons,int hats) {
  return !strcmp(SDL_GetPlatform(),"Linux") && raw && vendor==0x044f && product==0xb68d &&
    axes==8 && buttons==15 && hats==1;
}
int lhx_input_hotas_one(const LhxControllerInput *in) {
  return in && lhx_input_attached(in) && lhx_input_hotas_match(in->raw,
    SDL_JoystickGetVendor(in->joy),SDL_JoystickGetProduct(in->joy),in->axes,
    SDL_JoystickNumButtons(in->joy),SDL_JoystickNumHats(in->joy));
}
void lhx_input_axis_label(const LhxControllerInput *in,int axis,char *out,size_t size) {
  /* Official T.Flight Hotas One diagram + Linux ascending ABS / SDL order.
   * RX is the RIGHT toe brake; RY is LEFT. These are identities, not calibration. */
  static const char *const names[]={"Stick left/right","Stick forward/back","Throttle lever",
    "Right toe brake","Left toe brake","Stick twist","Throttle rocker","Pedal rudder"};
  if (axis>=0 && axis<8 && lhx_input_hotas_one(in)) snprintf(out,size,"Axis %d %s",axis+1,names[axis]);
  else snprintf(out,size,"Axis %d",axis+1);
}

int lhx_input_axis_batch(const int *before,const int *after,const Uint8 *ready,int count,int threshold,int *last) {
  int changed=0;
  for (int a=0;a<count;++a) if ((!ready || ready[a]) && abs(after[a]-before[a])>=threshold) { ++changed;if (last) *last=a; }
  return changed;
}

/* Consumer-control keyboards generally have no flight axes. Do not open these
 * as game inputs. Eligibility does not depend on known-device labels. */
int lhx_input_eligible(int index) {
  if (SDL_IsGameController(index)) return 1;
  SDL_Joystick *joy=SDL_JoystickOpen(index);
  if (!joy) return 0;
  int eligible=SDL_JoystickNumAxes(joy)>=2 && SDL_JoystickNumButtons(joy)>=2;
  SDL_JoystickClose(joy);
  return eligible;
}
int lhx_input_open(LhxControllerInput *in,int index,LhxInputMode mode) {
  *in=(LhxControllerInput){0};
  if (!lhx_input_eligible(index)) return 0;
  return lhx_input_open_explicit(in,index,mode!=LHX_INPUT_JOYSTICK && (mode==LHX_INPUT_GAMEPAD || SDL_IsGameController(index)));
}
int lhx_input_open_explicit(LhxControllerInput *in,int index,int mapped) {
  *in=(LhxControllerInput){0};
  in->raw=!mapped;
  if (!in->raw) {
    in->pad=SDL_GameControllerOpen(index);
    if (!in->pad) return 0;
    in->joy=SDL_GameControllerGetJoystick(in->pad);
    in->axes=SDL_CONTROLLER_AXIS_MAX; in->buttons=SDL_CONTROLLER_BUTTON_MAX;
  } else {
    in->joy=SDL_JoystickOpen(index);
    if (!in->joy) return 0;
    in->axes=SDL_JoystickNumAxes(in->joy);
    in->hats=SDL_JoystickNumHats(in->joy);
    int buttons=SDL_JoystickNumButtons(in->joy);
    if (in->axes>LHX_RAW_AXES || buttons>LHX_RAW_BUTTONS || in->hats>LHX_RAW_HATS) {
      SDL_SetError("Native joystick exceeds supported limits (%d axes, %d buttons, %d hats)",
        LHX_RAW_AXES,LHX_RAW_BUTTONS,LHX_RAW_HATS);
      lhx_input_close(in); return 0;
    }
    in->buttons=in->hats?LHX_RAW_BUTTONS+4*in->hats:buttons;
  }
  return 1;
}
void lhx_input_close(LhxControllerInput *in) {
  if (in->pad) SDL_GameControllerClose(in->pad);
  else if (in->joy) SDL_JoystickClose(in->joy);
  *in=(LhxControllerInput){0};
}
int lhx_input_open_selected(LhxControllerInput *in,const LhxControls *c) {
  /* Preserve first supported gamepad behavior, even if raw devices came first. */
  for (int pass=0;pass<2;++pass) for (int i=0;i<SDL_NumJoysticks();++i) {
    if ((SDL_IsGameController(i)!=SDL_FALSE)!=(pass==0)) continue;
    LhxControllerInput candidate;
    if (!lhx_input_open(&candidate,i,c->input_mode)) continue;
    if (lhx_controls_joystick_matches(c,candidate.joy)) { *in=candidate; return 1; }
    lhx_input_close(&candidate);
  }
  return 0;
}
int lhx_input_attached(const LhxControllerInput *in) {
  return in->joy && SDL_JoystickGetAttached(in->joy);
}
int lhx_input_axis(const LhxControllerInput *in,int i) {
  return in->raw?SDL_JoystickGetAxis(in->joy,i):SDL_GameControllerGetAxis(in->pad,(SDL_GameControllerAxis)i);
}
int lhx_input_button(const LhxControllerInput *in,int i) {
  if (!in->raw) return SDL_GameControllerGetButton(in->pad,(SDL_GameControllerButton)i);
  if (i<LHX_RAW_BUTTONS) return i<SDL_JoystickNumButtons(in->joy)?SDL_JoystickGetButton(in->joy,i):0;
  i-=LHX_RAW_BUTTONS;
  return (SDL_JoystickGetHat(in->joy,i/4) & (1<<(i%4)))!=0;
}
int lhx_input_unipolar(const LhxControllerInput *in,int i) {
  return !in->raw && i>=SDL_CONTROLLER_AXIS_TRIGGERLEFT;
}
SDL_GameControllerButtonBind lhx_input_bind(const LhxControllerInput *in,int axis,int i) {
  SDL_GameControllerButtonBind b={0};
  if (!in->raw) {
    if (!in->pad) return b;
    return axis?SDL_GameControllerGetBindForAxis(in->pad,(SDL_GameControllerAxis)i):
      SDL_GameControllerGetBindForButton(in->pad,(SDL_GameControllerButton)i);
  }
  if (axis) { b.bindType=SDL_CONTROLLER_BINDTYPE_AXIS; b.value.axis=i; }
  else if (i<LHX_RAW_BUTTONS) { b.bindType=SDL_CONTROLLER_BINDTYPE_BUTTON; b.value.button=i; }
  else { b.bindType=SDL_CONTROLLER_BINDTYPE_HAT; b.value.hat.hat=(i-LHX_RAW_BUTTONS)/4;
    b.value.hat.hat_mask=1<<((i-LHX_RAW_BUTTONS)%4); }
  return b;
}
void lhx_input_select(const LhxControllerInput *in,LhxControls *c) {
  lhx_controls_select_joystick(c,in->joy);
  c->input_mode=in->raw?LHX_INPUT_JOYSTICK:LHX_INPUT_GAMEPAD;
}
LhxControlAction *lhx_input_buttons(const LhxControllerInput *in,LhxControls *c) {
  return in->raw?c->raw_buttons:c->buttons;
}
LhxAxisBinding *lhx_input_axes(const LhxControllerInput *in,LhxControls *c) {
  return in->raw?c->raw_axes:c->axes;
}
