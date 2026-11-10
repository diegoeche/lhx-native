#pragma once
#include "controller_input.h"
#include "controller_flex_setup.h"

enum { LHX_SETUP_PENDING, LHX_SETUP_SESSION, LHX_SETUP_SAVE, LHX_SETUP_CANCEL };
enum {
  LHX_SETUP_ROLL_ROW, LHX_SETUP_PITCH_ROW, LHX_SETUP_RUDDER_ROW, LHX_SETUP_COLLECTIVE_ROW,
  LHX_SETUP_INCREASE_ROW, LHX_SETUP_DECREASE_ROW, LHX_SETUP_FULL_ROW, LHX_SETUP_HOVER_ROW,
  LHX_SETUP_GEAR_ROW, LHX_SETUP_FIRE_ROW, LHX_SETUP_PREVIOUS_ROW, LHX_SETUP_NEXT_ROW,
  LHX_SETUP_CHAFF_ROW, LHX_SETUP_FLARES_ROW, LHX_SETUP_COCKPIT_ROW, LHX_SETUP_CHASE_ROW,
  LHX_SETUP_VIEW_ROW, LHX_SETUP_PAUSE_ROW, LHX_SETUP_CONFIRM_ROW, LHX_SETUP_BACK_ROW,
  LHX_SETUP_UP_ROW, LHX_SETUP_DOWN_ROW, LHX_SETUP_LEFT_ROW, LHX_SETUP_RIGHT_ROW,
  LHX_SETUP_ACTIONS, LHX_SETUP_STARTUP_ROW, LHX_SETUP_DEADZONE_ROW,
  LHX_SETUP_TRIGGER_ROW, LHX_SETUP_THRESHOLD_ROW, LHX_SETUP_DEFAULTS_ROW, LHX_SETUP_ROWS
};
#define LHX_SETUP_DEVICE_ROW LHX_SETUP_ACTIONS
#define LHX_SETUP_WIDTH 1200
#define LHX_SETUP_HEIGHT 840

typedef struct { char input[64], action[128]; } LhxSetupSignal;
/* Private draft: capture/conflict never edits it until replacement is confirmed.
 * No game callbacks run during setup. Physical names resolve through the selected input adapter. */
typedef struct LhxControllerSetup {
  LhxFlexibleSetup flex;
  LhxControls edits, pending;
  LhxControllerInput input;
  SDL_JoystickID instance;
  int row, scroll, settings, capture, conflict, result, focused;
  int candidate_axis, candidate_input, candidate_direction, monitor_page;
  unsigned displaced;
  Uint32 armed_at, highlight[LHX_SETUP_ACTIONS], axis_time[LHX_RAW_AXES];
  int baseline[LHX_RAW_AXES], values[LHX_RAW_AXES];
  int axis_reported[LHX_RAW_AXES];
  Uint8 previous[LHX_INPUT_BUTTONS], ready[LHX_INPUT_BUTTONS];
  Uint8 trigger_ready[LHX_RAW_AXES], active[LHX_SETUP_ACTIONS];
  LhxSetupSignal buttons[3], motion[2];
  char status[256];
} LhxControllerSetup;

void lhx_controller_setup_init(LhxControllerSetup *, const LhxControls *);
void lhx_controller_setup_close(LhxControllerSetup *);
void lhx_controller_setup_tick(LhxControllerSetup *);
void lhx_controller_setup_event(LhxControllerSetup *, const SDL_Event *);
void lhx_controller_setup_draw(const LhxControllerSetup *, SDL_Renderer *, const char *path);
/* Full current list, also used by the selected-action detail and confirmation. */
void lhx_controller_setup_bindings(const LhxControllerSetup *, int row, char *, size_t);
/* Output unchanged on cancel/error; save error stays in setup. */
int lhx_controller_setup_accept(LhxControllerSetup *, const char *path, LhxControls *);
/* Owns its temporary SDL subsystems/window. 1=start, 0=cancel, -1=SDL error. */
int lhx_controller_setup_run(LhxControls *, const char *path);
