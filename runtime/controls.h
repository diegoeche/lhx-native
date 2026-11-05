#pragma once
#include <SDL.h>

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
  int antialiasing, sphere_depth;
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
  unsigned deadzone, trigger_threshold, axis_button_threshold;
  unsigned repeat_delay_ms, repeat_interval_ms, throttle_repeat_ms, fire_repeat_ms;
  LhxRendering rendering;
} LhxControls;

void lhx_controls_defaults(LhxControls *);
/* Missing optional files use defaults. Invalid files leave the output intact. */
int lhx_controls_load(LhxControls *, const char *path, int required);
