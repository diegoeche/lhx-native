#include "gamepad.h"
#include "controls.h"
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_GameController *pad;
static LhxPadKey emit_key;
static int focused = 1, roll_axis, pitch_axis, yaw_axis, needs_center, chase_view;
static int view_held, controls_ready;
static LhxControls controls;

/* Modifiers precede ordinary keys on press and follow them on release. */
enum {
  CONTROL, ALT, ENTER, ESCAPE, FIRE, FULL, HOVER, PREVIOUS, NEXT,
  COCKPIT, CHASE, PAUSE, GEAR, CHAFF, FLARES,
  UP, DOWN, LEFT, RIGHT, DECREASE, INCREASE, KEY_COUNT
};
static const unsigned scans[KEY_COUNT] = {
  0x1d, 0x38, 0x1c, 0x01, 0x39, 0x06, 0x04, 0x1a, 0x1b,
  0x3b, 0x3f, 0x19, 0x22, 0x2e, 0x21,
  0xe048, 0xe050, 0xe04b, 0xe04d, 0x07, 0x08
};
static unsigned char held[KEY_COUNT];
static uint64_t repeat_at[KEY_COUNT];

static void ensure_controls(void) {
  if (!controls_ready) {
    lhx_controls_defaults(&controls);
    controls_ready = 1;
  }
}

static void release_keys(void) {
  for (int i = KEY_COUNT - 1; i >= 0; --i) {
    if (held[i] && emit_key)
      emit_key(scans[i], 1);
    held[i] = 0;
    repeat_at[i] = 0;
  }
  view_held = 0;
}

void lhx_gamepad_configure(const LhxControls *config) {
  release_keys();
  roll_axis = pitch_axis = yaw_axis = 0;
  controls = *config;
  controls_ready = 1;
}

void lhx_gamepad_focus(int has_focus) {
  focused = has_focus;
  if (!focused) {
    release_keys();
    roll_axis = pitch_axis = yaw_axis = 0;
    /* Keep needs_center until the next original physics update. */
  }
}

void lhx_gamepad_close(void) {
  release_keys();
  roll_axis = pitch_axis = yaw_axis = 0;
  if (pad)
    SDL_GameControllerClose(pad);
  pad = NULL;
}

static void open_first(void) {
  if (pad)
    return;
  for (int i = 0; i < SDL_NumJoysticks(); ++i) {
    if (!SDL_IsGameController(i))
      continue;
    pad = SDL_GameControllerOpen(i);
    if (pad) {
      SDL_Joystick *joystick = SDL_GameControllerGetJoystick(pad);
      fprintf(stderr, "[controller] %s (%04x:%04x)\n",
              SDL_GameControllerName(pad), SDL_JoystickGetVendor(joystick),
              SDL_JoystickGetProduct(joystick));
      return;
    }
  }
}

void lhx_gamepad_init(LhxPadKey callback) {
  ensure_controls();
  emit_key = callback;
  focused = 1;
  open_first();
}

void lhx_gamepad_event(const SDL_Event *event) {
  if (event->type == SDL_CONTROLLERDEVICEADDED)
    open_first();
  else if (event->type == SDL_CONTROLLERDEVICEREMOVED && pad &&
           event->cdevice.which == SDL_JoystickInstanceID(
                                      SDL_GameControllerGetJoystick(pad))) {
    fprintf(stderr, "[controller] disconnected\n");
    lhx_gamepad_close();
    open_first();
  }
}

/* The original flight code removes a +/-20 deadband from its +/-100 input.
 * Compensate after the hardware deadzone so the remaining travel is linear. */
static int cyclic(int value) {
  int magnitude = abs(value);
  if (magnitude <= (int)controls.deadzone)
    return 0;
  if (magnitude > 32767)
    magnitude = 32767;
  int result = 20 + (magnitude - controls.deadzone) * 80 / (32767 - controls.deadzone);
  return value < 0 ? -result : result;
}

void lhx_gamepad_poll(uint64_t now_us) {
  if (!pad || !focused)
    return;
  if (!SDL_GameControllerGetAttached(pad)) {
    lhx_gamepad_close();
    return;
  }
  unsigned char active[LHX_ACTION_COUNT] = {0};
  unsigned char desired[KEY_COUNT] = {0};
  int analog[3] = {0};
  for (int button = 0; button < SDL_CONTROLLER_BUTTON_MAX; ++button)
    if (SDL_GameControllerGetButton(pad, (SDL_GameControllerButton)button))
      active[controls.buttons[button]] = 1;
  for (int axis = 0; axis < SDL_CONTROLLER_AXIS_MAX; ++axis) {
    LhxAxisBinding binding = controls.axes[axis];
    int value = SDL_GameControllerGetAxis(pad, (SDL_GameControllerAxis)axis) * binding.direction;
    if (binding.action >= LHX_ROLL && binding.action <= LHX_RUDDER) {
      int scaled = cyclic(value), slot = binding.action - LHX_ROLL;
      if (abs(scaled) > abs(analog[slot]))
        analog[slot] = scaled;
    } else if (binding.action == LHX_COLLECTIVE) {
      active[LHX_INCREASE] |= value < -(int)controls.axis_button_threshold;
      active[LHX_DECREASE] |= value > (int)controls.axis_button_threshold;
    } else if (binding.action == LHX_AXIS_FIRE)
      active[LHX_FIRE] |= value > (int)controls.trigger_threshold;
    else if (binding.action == LHX_AXIS_HOVER)
      active[LHX_HOVER] |= value > (int)controls.trigger_threshold;
    else if (binding.action == LHX_AXIS_CHAFF)
      active[LHX_CHAFF] |= value > (int)controls.trigger_threshold;
    else if (binding.action == LHX_AXIS_FLARES)
      active[LHX_FLARES] |= value > (int)controls.trigger_threshold;
  }
  roll_axis = analog[0]; pitch_axis = analog[1]; yaw_axis = analog[2];
  if (roll_axis || pitch_axis || yaw_axis)
    needs_center = 1;
  desired[ALT] = yaw_axis != 0;
  static const uint32_t action_keys[LHX_ACTION_COUNT] = {
    [LHX_CONFIRM] = 1u << ENTER, [LHX_CANCEL] = 1u << ESCAPE,
    [LHX_FIRE] = 1u << FIRE, [LHX_FULL] = 1u << FULL,
    [LHX_HOVER] = 1u << HOVER, [LHX_PREVIOUS] = 1u << PREVIOUS,
    [LHX_NEXT] = 1u << NEXT, [LHX_COCKPIT] = 1u << COCKPIT,
    [LHX_CHASE] = 1u << CHASE, [LHX_PAUSE] = (1u << CONTROL) | (1u << PAUSE),
    [LHX_GEAR] = 1u << GEAR, [LHX_UP] = 1u << UP, [LHX_DOWN] = 1u << DOWN,
    [LHX_LEFT] = 1u << LEFT, [LHX_RIGHT] = 1u << RIGHT,
    [LHX_DECREASE] = 1u << DECREASE, [LHX_INCREASE] = 1u << INCREASE,
    [LHX_CHAFF] = 1u << CHAFF, [LHX_FLARES] = 1u << FLARES
  };
  for (unsigned action = 1; action < LHX_ACTION_COUNT; ++action)
    if (active[action])
      for (unsigned key = 0; key < KEY_COUNT; ++key)
        desired[key] |= (action_keys[action] >> key) & 1;
  if (active[LHX_VIEW] && !view_held)
    chase_view = !chase_view;
  view_held = active[LHX_VIEW];
  desired[chase_view ? CHASE : COCKPIT] |= view_held;
  if (desired[INCREASE] && desired[DECREASE])
    desired[INCREASE] = desired[DECREASE] = 0;

  for (int i = KEY_COUNT - 1; i >= 0; --i)
    if (held[i] && !desired[i] && emit_key)
      emit_key(scans[i], 1);
  for (int i = 0; i < KEY_COUNT; ++i) {
    if (desired[i] && !held[i]) {
      if (emit_key)
        emit_key(scans[i], 0);
      repeat_at[i] = now_us + (uint64_t)controls.repeat_delay_ms * 1000;
    } else if (desired[i] && (i == FIRE || i >= UP) && now_us >= repeat_at[i]) {
      if (emit_key) {
        /* LHX's keyboard reader can discard another make for a held key.
         * A break followed by a make becomes a distinct game action. */
        emit_key(scans[i], 1);
        emit_key(scans[i], 0);
      }
      unsigned period = i == FIRE ? controls.fire_repeat_ms :
          (i == INCREASE || i == DECREASE) ? controls.throttle_repeat_ms :
          controls.repeat_interval_ms;
      repeat_at[i] = now_us + (uint64_t)period * 1000;
    }
    held[i] = desired[i];
  }
}

void lhx_gamepad_apply(CPU *cpu) {
  if (!needs_center)
    return;
  /* Alt switches the original shared lateral axis to rudder control. */
  mem_write16(cpu, cpu->ds, 0xead6, (uint16_t)(yaw_axis ? yaw_axis : roll_axis));
  mem_write16(cpu, cpu->ds, 0xead8, (uint16_t)pitch_axis);
  needs_center = roll_axis || pitch_axis || yaw_axis;
}

void lhx_gamepad_list(void) {
  int count = SDL_NumJoysticks();
  if (!count)
    puts("No controllers detected.");
  for (int i = 0; i < count; ++i)
    printf("%d: %s (%04x:%04x)%s\n", i, SDL_JoystickNameForIndex(i),
           SDL_JoystickGetDeviceVendor(i), SDL_JoystickGetDeviceProduct(i),
           SDL_IsGameController(i) ? " [supported gamepad]" : " [unmapped device]");
}
