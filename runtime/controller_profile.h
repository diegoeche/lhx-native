#pragma once
#include <SDL.h>
#include <stddef.h>

/* V2 is opt-in. Indices identify profile records, never USB enumeration order. */
enum { LHX_USB_DEVICES=16, LHX_USB_BINDINGS=128, LHX_USB_AXES=32 };
typedef enum {
  USB_ROLL, USB_PITCH, USB_RUDDER, USB_THROTTLE, USB_INCREASE, USB_DECREASE,
  USB_FULL, USB_HALF, USB_GEAR, USB_FIRE, USB_PREVIOUS, USB_NEXT, USB_CHAFF,
  USB_FLARES, USB_COCKPIT, USB_CHASE, USB_VIEW, USB_PAUSE, USB_CONFIRM,
  USB_BACK, USB_UP, USB_DOWN, USB_LEFT, USB_RIGHT, USB_ACTIONS
} LhxUsbAction;
typedef enum { USB_ANALOG, USB_DIRECT, USB_RELATIVE, USB_INC, USB_DEC,
  USB_PRESET, USB_POSITIVE, USB_NEGATIVE, USB_HOLD, USB_LEGACY, USB_BEHAVIORS } LhxUsbBehavior;
typedef struct {
  int set, lever, minimum, center, maximum, invert, deadzone;
} LhxCalibration;
typedef struct {
  char guid[33], serial[241], name[193]; /* name and serial are UTF-8 hex */
  int enabled, mapped, session_only;
  SDL_JoystickID occurrence; /* explicitly assigned session only; never persisted */
  LhxCalibration calibration[LHX_USB_AXES];
} LhxUsbDevice;
typedef struct {
  int device, axis, input, action, behavior, direction, percent, threshold;
} LhxUsbBinding;
typedef struct {
  int version, devices, bindings;
  LhxUsbDevice device[LHX_USB_DEVICES];
  LhxUsbBinding binding[LHX_USB_BINDINGS];
} LhxUsbProfile;

const char *lhx_usb_action_name(int);
const char *lhx_usb_behavior_name(int);
const char *lhx_usb_behavior_label(int);
const char *lhx_usb_action_label(int);
int lhx_usb_behavior_allowed(const LhxUsbBinding *, int behavior);
int lhx_usb_profile_valid(const LhxUsbProfile *);
/* Returns 0 for invalid, 1 for known section/key. Duplicate keys are rejected by loader. */
int lhx_usb_profile_section(const char *);
int lhx_usb_profile_parse(LhxUsbProfile *, const char *section, const char *key, const char *value);
/* Flat, bounded list for the existing preserving atomic INI writer. */
typedef struct { char section[40], key[40], value[480]; } LhxUsbIniKey;
int lhx_usb_profile_keys(const LhxUsbProfile *, LhxUsbIniKey *, int capacity);
int lhx_calibration_valid(const LhxCalibration *);
int lhx_calibrated(const LhxCalibration *, int raw, int lever);
int lhx_usb_hex(char *, size_t, const char *);
void lhx_usb_unhex(char *, size_t, const char *);
