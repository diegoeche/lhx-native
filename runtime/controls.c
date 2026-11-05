#include "controls.h"
#include <ctype.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static const char *actions[LHX_ACTION_COUNT] = {
  "none", "confirm", "cancel", "fire", "full_collective", "hover",
  "previous_weapon", "next_weapon", "cockpit", "chase", "toggle_view", "pause",
  "gear", "menu_up", "menu_down", "menu_left", "menu_right",
  "collective_down", "collective_up", "chaff", "flares"
};
static const char *axis_actions[] = {
  "none", "roll", "pitch", "rudder", "collective", "fire", "hover", "chaff", "flares"
};

void lhx_controls_defaults(LhxControls *controls) {
  *controls = (LhxControls){
    .buttons = {
      [SDL_CONTROLLER_BUTTON_A] = LHX_CONFIRM,
      [SDL_CONTROLLER_BUTTON_B] = LHX_CANCEL,
      [SDL_CONTROLLER_BUTTON_X] = LHX_CHAFF,
      [SDL_CONTROLLER_BUTTON_Y] = LHX_FLARES,
      [SDL_CONTROLLER_BUTTON_BACK] = LHX_VIEW,
      [SDL_CONTROLLER_BUTTON_START] = LHX_PAUSE,
      [SDL_CONTROLLER_BUTTON_LEFTSTICK] = LHX_GEAR,
      [SDL_CONTROLLER_BUTTON_RIGHTSTICK] = LHX_NEXT,
      [SDL_CONTROLLER_BUTTON_LEFTSHOULDER] = LHX_DECREASE,
      [SDL_CONTROLLER_BUTTON_RIGHTSHOULDER] = LHX_INCREASE,
      [SDL_CONTROLLER_BUTTON_DPAD_UP] = LHX_UP,
      [SDL_CONTROLLER_BUTTON_DPAD_DOWN] = LHX_DOWN,
      [SDL_CONTROLLER_BUTTON_DPAD_LEFT] = LHX_LEFT,
      [SDL_CONTROLLER_BUTTON_DPAD_RIGHT] = LHX_RIGHT
    },
    .axes = {
      [SDL_CONTROLLER_AXIS_LEFTX] = {LHX_ROLL, 1},
      [SDL_CONTROLLER_AXIS_LEFTY] = {LHX_PITCH, 1},
      [SDL_CONTROLLER_AXIS_RIGHTX] = {LHX_RUDDER, 1},
      [SDL_CONTROLLER_AXIS_RIGHTY] = {LHX_COLLECTIVE, 1},
      [SDL_CONTROLLER_AXIS_TRIGGERLEFT] = {LHX_AXIS_HOVER, 1},
      [SDL_CONTROLLER_AXIS_TRIGGERRIGHT] = {LHX_AXIS_FIRE, 1}
    },
    .deadzone = 6000, .trigger_threshold = 16000, .axis_button_threshold = 12000,
    .repeat_delay_ms = 350, .repeat_interval_ms = 120,
    .throttle_repeat_ms = 200, .fire_repeat_ms = 100,
    .rendering = {.extended_visibility = 1,.terrain_dir="bin/enhanced/terrain",
                  .models_dir="bin/enhanced/models",.sky_dir="bin/enhanced/sky"}
  };
}

static char *trim(char *text) {
  while (isspace((unsigned char)*text))
    text++;
  char *end = text + strlen(text);
  while (end > text && isspace((unsigned char)end[-1]))
    *--end = 0;
  return text;
}

static int named(const char *name, const char *const *names, unsigned count) {
  for (unsigned i = 0; i < count; ++i)
    if (!strcasecmp(name, names[i]))
      return (int)i;
  return -1;
}

static int setting(LhxControls *controls, const char *key, const char *value) {
  static const struct {
    const char *name;
    size_t offset;
    unsigned minimum, maximum;
  } settings[] = {
    {"deadzone", offsetof(LhxControls, deadzone), 0, 30000},
    {"trigger_threshold", offsetof(LhxControls, trigger_threshold), 0, 32766},
    {"axis_button_threshold", offsetof(LhxControls, axis_button_threshold), 0, 32766},
    {"repeat_delay_ms", offsetof(LhxControls, repeat_delay_ms), 0, 2000},
    {"repeat_interval_ms", offsetof(LhxControls, repeat_interval_ms), 30, 2000},
    {"throttle_repeat_ms", offsetof(LhxControls, throttle_repeat_ms), 30, 2000},
    {"fire_repeat_ms", offsetof(LhxControls, fire_repeat_ms), 30, 2000}
  };
  for (unsigned i = 0; i < sizeof(settings) / sizeof(*settings); ++i) {
    if (strcasecmp(key, settings[i].name))
      continue;
    char *end;
    errno = 0;
    unsigned long number = strtoul(value, &end, 10);
    if (errno || !*value || *end || number < settings[i].minimum ||
        number > settings[i].maximum)
      return 0;
    *(unsigned *)((char *)controls + settings[i].offset) = (unsigned)number;
    return 1;
  }
  return 0;
}

static int rendering(LhxRendering *options, const char *key, const char *value) {
  if (!strcmp(key, "renderer")) {
    const char *const names[] = {"native", "original"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    options->renderer = mode == 0 ? LHX_RENDER_NATIVE : LHX_RENDER_ORIGINAL;
  } else if (!strcmp(key,"backend")) {
    const char *const names[]={"cpu","gpu"};
    int mode=named(value,names,2);
    if (mode<0) return 0;
    options->gpu=mode;
  } else if (!strcmp(key, "render_scale")) {
    char *end;
    errno = 0;
    unsigned long scale = strtoul(value, &end, 10);
    if (errno || !*value || *end || scale < 1 || scale > 8) return 0;
    options->render_scale = (unsigned)scale;
  } else if (!strcmp(key, "visibility")) {
    const char *const names[] = {"original", "extended"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    options->extended_visibility = mode;
  } else if (!strcmp(key,"sky_dir")) {
    if (!*value || strlen(value)>=sizeof options->sky_dir) return 0;
    strcpy(options->sky_dir,value);
  } else if (!strcmp(key,"models_dir")) {
    if (!*value || strlen(value)>=sizeof options->models_dir) return 0;
    strcpy(options->models_dir,value);
  } else if (!strcmp(key,"terrain_dir")) {
    if (!*value || strlen(value)>=sizeof options->terrain_dir) return 0;
    strcpy(options->terrain_dir,value);
  } else if (!strcmp(key, "show_fps") || !strcmp(key,"lighting") ||
             !strcmp(key,"runway_textures") || !strcmp(key,"terrain_textures") ||
             !strcmp(key,"antialiasing") || !strcmp(key,"sphere_depth") ||
             !strcmp(key,"atmosphere") || !strcmp(key,"replacement_models") ||
             !strcmp(key,"photo_skies") || !strcmp(key,"terrain_relief") ||
             !strcmp(key,"shadows") || !strcmp(key,"military_paint") ||
             !strcmp(key,"surface_detail") || !strcmp(key,"rotor_blades") || !strcmp(key,"runway_detail")) {
    const char *const names[] = {"false", "true"};
    int mode = named(value, names, 2);
    if (mode < 0) return 0;
    if (!strcmp(key,"show_fps")) options->show_fps=mode;
    else if (!strcmp(key,"lighting")) options->lighting=mode;
    else if (!strcmp(key,"runway_textures")) options->runway_textures=mode;
    else if (!strcmp(key,"terrain_textures")) options->terrain_textures=mode;
    else if (!strcmp(key,"antialiasing")) options->antialiasing=mode;
    else if (!strcmp(key,"sphere_depth")) options->sphere_depth=mode;
    else if (!strcmp(key,"atmosphere")) options->atmosphere=mode;
    else if (!strcmp(key,"replacement_models")) options->replacement_models=mode;
    else if (!strcmp(key,"photo_skies")) options->photo_skies=mode;
    else if (!strcmp(key,"terrain_relief")) options->terrain_relief=mode;
    else if (!strcmp(key,"shadows")) options->shadows=mode;
    else if (!strcmp(key,"military_paint")) options->military_paint=mode;
    else if (!strcmp(key,"surface_detail")) options->surface_detail=mode;
    else if (!strcmp(key,"rotor_blades")) options->rotor_blades=mode;
    else options->runway_detail=mode;
  } else return 0;
  return 1;
}

int lhx_controls_load(LhxControls *controls, const char *path, int required) {
  LhxControls candidate;
  lhx_controls_defaults(&candidate);
  FILE *file = fopen(path, "r");
  if (!file) {
    if (!required && errno == ENOENT) {
      *controls = candidate;
      return 1;
    }
    fprintf(stderr, "[controls] %s: %s\n", path, strerror(errno));
    return 0;
  }
  enum { UNSET, BUTTONS, AXES, SETTINGS, RENDERING } section = UNSET;
  char line[512];
  unsigned line_number = 0;
  const char *error = NULL;
  while (fgets(line, sizeof(line), file)) {
    line_number++;
    if (!strchr(line, '\n') && !feof(file)) {
      error = "line too long";
      break;
    }
    char *comment = strpbrk(line, "#;");
    if (comment)
      *comment = 0;
    char *key = trim(line);
    if (!*key)
      continue;
    if (*key == '[') {
      if (!strcasecmp(key, "[buttons]")) section = BUTTONS;
      else if (!strcasecmp(key, "[axes]")) section = AXES;
      else if (!strcasecmp(key, "[settings]")) section = SETTINGS;
      else if (!strcasecmp(key, "[rendering]")) section = RENDERING;
      else { error = "unknown section"; break; }
      continue;
    }
    char *value = strchr(key, '=');
    if (!value) { error = "expected input = action"; break; }
    *value++ = 0;
    key = trim(key);
    value = trim(value);
    for (char *p = key; *p; ++p)
      *p = (char)tolower((unsigned char)*p);
    if (section == BUTTONS) {
      SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(key);
      int action = named(value, actions, LHX_ACTION_COUNT);
      if (button == SDL_CONTROLLER_BUTTON_INVALID || action < 0) {
        error = "unknown button or action"; break;
      }
      candidate.buttons[button] = (LhxControlAction)action;
    } else if (section == AXES) {
      SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(key);
      int direction = *value == '-' ? -1 : 1;
      int action = named(value + (direction < 0), axis_actions,
                         sizeof(axis_actions) / sizeof(*axis_actions));
      if (axis == SDL_CONTROLLER_AXIS_INVALID || action < 0) {
        error = "unknown axis or action"; break;
      }
      candidate.axes[axis] = (LhxAxisBinding){(LhxAxisAction)action, direction};
    } else if (section == SETTINGS) {
      if (!setting(&candidate, key, value)) {
        error = "unknown setting or value outside its allowed range"; break;
      }
    } else if (section == RENDERING) {
      if (!rendering(&candidate.rendering, key, value)) {
        error = "unknown rendering option or invalid value"; break;
      }
    } else { error = "expected [buttons], [axes], [settings], or [rendering]"; break; }
  }
  if (!error && ferror(file))
    error = "read failed";
  fclose(file);
  if (error) {
    fprintf(stderr, "[controls] %s:%u: %s\n", path, line_number, error);
    return 0;
  }
  *controls = candidate;
  fprintf(stderr, "[controls] %s\n", path);
  return 1;
}
