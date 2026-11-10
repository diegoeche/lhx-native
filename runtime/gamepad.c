#include "gamepad.h"
#include "controller_input.h"
#include "controller_model.h"
#include "lifted.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LhxControllerInput input;
static LhxUsbRuntime usb;
static int usb_ready;
static LhxPadKey emit_key;
static int focused = 1, roll_axis, pitch_axis, yaw_axis, needs_center, chase_view;
static int view_held, controls_ready;
static int manual_guidance, fire_repeat_suppressed;
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
  manual_guidance = fire_repeat_suppressed = 0;
}

void lhx_gamepad_configure(const LhxControls *config) {
  release_keys();
  if (usb_ready) lhx_usb_runtime_close(&usb);
  else { lhx_usb_runtime_init(&usb);usb_ready=1; }
  if (config->usb.version) lhx_input_close(&input);
  roll_axis = pitch_axis = yaw_axis = 0;
  controls = *config;
  controls_ready = 1;
  if (input.joy && (!lhx_controls_joystick_matches(&controls,input.joy) ||
      (controls.input_mode!=LHX_INPUT_AUTO && input.raw!=(controls.input_mode==LHX_INPUT_JOYSTICK))))
    lhx_gamepad_close();
}

void lhx_gamepad_focus(int has_focus) {
  focused = has_focus;
  if (controls.usb.version) lhx_usb_reseed(&usb);
  if (!focused) {
    release_keys();
    roll_axis = pitch_axis = yaw_axis = 0;
    /* Keep needs_center until the next original physics update. */
  }
}

void lhx_gamepad_close(void) {
  release_keys();
  roll_axis = pitch_axis = yaw_axis = 0;
  lhx_input_close(&input);
  if (usb_ready) lhx_usb_runtime_close(&usb);
}

static void open_first(void) {
  if (controls.usb.version) {
    if (!usb_ready) { lhx_usb_runtime_init(&usb);usb_ready=1; }
    lhx_usb_refresh(&usb,&controls.usb);return;
  }
  if (input.joy) return;
  if (lhx_input_open_selected(&input,&controls))
    fprintf(stderr,"[controller] %s (%04x:%04x) [%s]\n",SDL_JoystickName(input.joy),
      SDL_JoystickGetVendor(input.joy),SDL_JoystickGetProduct(input.joy),
      input.raw?"native joystick":"mapped gamepad");
}

void lhx_gamepad_init(LhxPadKey callback) {
  ensure_controls();
  emit_key = callback;
  focused = 1;
  open_first();
  if (!controls.usb.version && !input.joy && *controls.controller_guid)
    fprintf(stderr,"[controller] selected device unavailable; keyboard remains active\n");
}

void lhx_gamepad_event(const SDL_Event *event) {
  if (controls.usb.version) {
    if (event->type==SDL_JOYDEVICEADDED || event->type==SDL_JOYDEVICEREMOVED ||
        event->type==SDL_CONTROLLERDEVICEADDED || event->type==SDL_CONTROLLERDEVICEREMOVED) open_first();
    return;
  }
  if (event->type==SDL_JOYDEVICEADDED || event->type==SDL_CONTROLLERDEVICEADDED)
    open_first();
  else if ((event->type==SDL_JOYDEVICEREMOVED || event->type==SDL_CONTROLLERDEVICEREMOVED) &&
      input.joy && event->jdevice.which==SDL_JoystickInstanceID(input.joy)) {
    fprintf(stderr,"[controller] disconnected\n");
    lhx_gamepad_close(); open_first();
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
  if (!focused || (!controls.usb.version && !input.joy))
    return;
  if (!controls.usb.version && !lhx_input_attached(&input)) {
    lhx_gamepad_close();
    return;
  }
  unsigned char active[LHX_ACTION_COUNT] = {0};
  unsigned char desired[KEY_COUNT] = {0};
  int analog[3] = {0};
  if (controls.usb.version) {
    lhx_usb_poll(&usb,&controls.usb);
    memcpy(active,usb.active,sizeof active);
    for (int a=0;a<3;++a) {
      int v=usb.analog[a];
      /* Hardware deadzone/calibration already applied by the per-device model. */
      analog[a]=v?(v<0?-1:1)*(20+abs(v)*80/32767):0;
    }
  } else {
  for (int button = 0; button < input.buttons; ++button)
    if (lhx_input_button(&input,button))
      active[lhx_input_buttons(&input,&controls)[button]] = 1;
  for (int axis = 0; axis < input.axes; ++axis) {
    LhxAxisBinding binding = lhx_input_axes(&input,&controls)[axis];
    int value = lhx_input_axis(&input,axis) * binding.direction;
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
  /* A second fire action detonates the original manually guided TOW. Keep
   * suppression for this entire press, even after the missile disappears. */
  if (!desired[FIRE])
    fire_repeat_suppressed = 0;
  else if (manual_guidance)
    fire_repeat_suppressed = 1;

  for (int i = KEY_COUNT - 1; i >= 0; --i)
    if (held[i] && !desired[i] && emit_key)
      emit_key(scans[i], 1);
  for (int i = 0; i < KEY_COUNT; ++i) {
    if (desired[i] && !held[i]) {
      if (emit_key)
        emit_key(scans[i], 0);
      repeat_at[i] = now_us + (uint64_t)controls.repeat_delay_ms * 1000;
    } else if (desired[i] && ((i == FIRE && !fire_repeat_suppressed) || i >= UP) &&
               now_us >= repeat_at[i]) {
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

/* Parent REA exact-artifact dossier ev_bd7184208869e99e9387d4c9e13def0c207c500b252affd7a43ef8636c3ebe40.
 * Same target, availability, bounds and reset fields as original fn_276E1.
 * Host requests are consumed once at the existing pre-TOW observer. */
static void apply_throttle(CPU *cpu,int percent) {
  if (percent<0 || percent>100 || mem_read8(cpu,cpu->ds,0xd863)) return;
  uint16_t off=mem_read16(cpu,cpu->ds,0xf626),seg=mem_read16(cpu,cpu->ds,0xf628);
  if (!seg || off>0xffff-0x341 || ((unsigned)seg<<4)+off+0x341>MEM_SIZE) return;
  int32_t nominal=(int16_t)mem_read16(cpu,seg,off+0x240)*256;
  int32_t current=(int32_t)mem_read32(cpu,seg,off+0x21c);
  int32_t low=(int32_t)mem_read32(cpu,seg,off+0x224);
  int32_t high=(int32_t)mem_read32(cpu,seg,off+(mem_read8(cpu,seg,off+0x340)&1?0x234:0x220));
  if (nominal<=0 || low>high) return;
  int32_t target=(int32_t)((int64_t)nominal*percent/100);
  if (target<low) target=low;
  if (target>high) target=high;
  if (target==current || (target>current && !mem_read32(cpu,seg,off+0x276))) return;
  mem_write32(cpu,seg,off+0x242,0);
  mem_write32(cpu,seg,off+0x21c,(uint32_t)target);
}
void lhx_gamepad_apply(CPU *cpu) {
  if (controls.usb.version && focused) apply_throttle(cpu,lhx_usb_take_throttle(&usb));
  /* Called before the original main loop saves the TOW's shared stick axes
   * and clears them for helicopter physics. This also runs with a centered
   * stick so a held trigger cannot become another TOW fire action. */
  manual_guidance = mem_read8(cpu, cpu->ds, 0xd855) != 0 &&
                    mem_read16(cpu, cpu->ds, 0x128) != 0;
  if (manual_guidance && held[FIRE])
    fire_repeat_suppressed = 1;
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
  for (int i=0;i<count;++i) {
    LhxControllerInput device;
    int supported=lhx_input_open(&device,i,LHX_INPUT_AUTO);
    printf("%d: %s (%04x:%04x) [%s]",i,SDL_JoystickNameForIndex(i),
      SDL_JoystickGetDeviceVendor(i),SDL_JoystickGetDeviceProduct(i),
      supported?(device.raw?"supported native joystick":"supported gamepad"):
        lhx_input_eligible(i)?"unsupported: exceeds limits or open failed":"excluded: needs at least 2 axes and 2 buttons");
    if (supported) {
      printf(" %d axes %d buttons %d hats",SDL_JoystickNumAxes(device.joy),
        SDL_JoystickNumButtons(device.joy),SDL_JoystickNumHats(device.joy));
      lhx_input_close(&device);
    }
    puts("");
  }
  LhxUsbCandidate list[LHX_USB_CANDIDATES];
  int n=lhx_usb_candidates(list,LHX_USB_CANDIDATES);
  if (n<0) puts("USB list exceeds 64 candidates; no silent truncation. Disconnect unused devices.");
  for (int i=0;i<n;++i) printf("USB candidate: %s GUID=%s serial=%s %d axes %d buttons %d hats [%s; explicit enable required]\n",
    list[i].name,list[i].identity.guid,*list[i].identity.serial?list[i].identity.serial:"none",list[i].axes,list[i].buttons,list[i].hats,
    *list[i].error?list[i].error:"capable");
}
