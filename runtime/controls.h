#pragma once
#include <SDL.h>
#include <stddef.h>
#include "controller_profile.h"

typedef enum {
  LHX_NONE, LHX_CONFIRM, LHX_CANCEL, LHX_FIRE, LHX_FULL, LHX_HOVER,
  LHX_PREVIOUS, LHX_NEXT, LHX_COCKPIT, LHX_CHASE, LHX_VIEW, LHX_PAUSE,
  LHX_GEAR, LHX_UP, LHX_DOWN, LHX_LEFT, LHX_RIGHT, LHX_DECREASE, LHX_INCREASE,
  LHX_CHAFF, LHX_FLARES,
  LHX_ACTION_COUNT
} LhxControlAction;
typedef enum {
  LHX_AXIS_NONE, LHX_ROLL, LHX_PITCH, LHX_RUDDER, LHX_COLLECTIVE,
  LHX_AXIS_FIRE, LHX_AXIS_HOVER, LHX_AXIS_CHAFF, LHX_AXIS_FLARES
} LhxAxisAction;
typedef struct {
  LhxAxisAction action;
  int direction;
} LhxAxisBinding;
/* Raw indices are independent of SDL's game-controller mapping database.
 * Hat directions follow the reserved button slots: up, right, down, left. */
enum { LHX_RAW_AXES=32, LHX_RAW_BUTTONS=64, LHX_RAW_HATS=4,
       LHX_INPUT_BUTTONS=LHX_RAW_BUTTONS+4*LHX_RAW_HATS };
typedef enum { LHX_INPUT_AUTO, LHX_INPUT_GAMEPAD, LHX_INPUT_JOYSTICK } LhxInputMode;
typedef enum {
  LHX_RENDER_DEFAULT, LHX_RENDER_NATIVE, LHX_RENDER_ORIGINAL
} LhxRenderer;
typedef struct {
  /* Unspecified renderer/scale retain the interactive or headless defaults. */
  LhxRenderer renderer;
  unsigned render_scale;
  int extended_visibility, show_fps;
  int lighting, runway_textures, terrain_textures;
  int terrain_relief,shadows,military_paint,surface_detail,rotor_blades,runway_detail;
  char terrain_dir[256];
  int antialiasing, sphere_depth, hide_ground_dots;
  int smooth_distant_aircraft;
  int gpu; /* Omitted backend retains the CPU renderer. */
  int atmosphere;
  int replacement_models;
  char models_dir[256];
  int photo_skies;
  char sky_dir[256];
} LhxRendering;
typedef struct {
  LhxControlAction buttons[SDL_CONTROLLER_BUTTON_MAX];
  LhxAxisBinding axes[SDL_CONTROLLER_AXIS_MAX];
  LhxControlAction raw_buttons[LHX_INPUT_BUTTONS];
  LhxAxisBinding raw_axes[LHX_RAW_AXES];
  LhxInputMode input_mode;
  unsigned deadzone, trigger_threshold, axis_button_threshold;
  unsigned repeat_delay_ms, repeat_interval_ms, throttle_repeat_ms, fire_repeat_ms;
  int improved_sounds;
  int show_controller_setup;
  /* Empty GUID keeps automatic first-supported behavior. Serial is hex
   * encoded so device strings cannot introduce INI comments or newlines. */
  char controller_guid[33], controller_serial[241];
  LhxRendering rendering;
  LhxUsbProfile usb;
} LhxControls;

void lhx_controls_defaults(LhxControls *);
/* Missing optional files use defaults. Invalid files leave the output intact. */
int lhx_controls_load(LhxControls *, const char *path, int required);
/* Updates controller fields only; preserves audio/rendering and comments.
 * Atomic replacement in the same directory. On failure the original survives. */
int lhx_controls_save(const LhxControls *, const char *path, char *error, size_t);
const char *lhx_control_action_name(LhxControlAction);
const char *lhx_axis_action_name(LhxAxisAction);
void lhx_controls_select_device(LhxControls *, SDL_GameController *);
int lhx_controls_device_matches(const LhxControls *, SDL_GameController *);
void lhx_controls_select_joystick(LhxControls *, SDL_Joystick *);
int lhx_controls_joystick_matches(const LhxControls *, SDL_Joystick *);
