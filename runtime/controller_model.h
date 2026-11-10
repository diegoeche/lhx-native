#pragma once
#include "controller_input.h"

enum { USB_OFFLINE, USB_CONNECTED, USB_AMBIGUOUS, USB_DISABLED, USB_ERROR };
enum { LHX_USB_CANDIDATES=64 };
typedef struct {
  LhxUsbDevice identity;
  SDL_JoystickID instance;
  int index, axes, buttons, hats, gamepad;
  char name[128], error[128];
} LhxUsbCandidate;
typedef struct {
  LhxControllerInput input[LHX_USB_DEVICES];
  int initial_seeded[LHX_USB_DEVICES], initial_pending[LHX_USB_DEVICES], initial_values[LHX_USB_DEVICES][32];
  int status[LHX_USB_DEVICES], blocked[LHX_USB_DEVICES];
  SDL_JoystickID assigned[LHX_USB_DEVICES];
  int anchor[LHX_USB_BINDINGS], seeded[LHX_USB_BINDINGS], ready[LHX_USB_BINDINGS], previous[LHX_USB_BINDINGS];
  Uint64 activity[LHX_USB_BINDINGS], sequence;
  int owner[3], analog[3], throttle, throttle_device;
  unsigned char active[LHX_ACTION_COUNT];
} LhxUsbRuntime;
int lhx_usb_candidates(LhxUsbCandidate *, int capacity); /* -1 on list overflow */
int lhx_usb_identity(LhxUsbDevice *,SDL_Joystick *);
int lhx_usb_same_identity(const LhxUsbDevice *,const LhxUsbDevice *);
int lhx_usb_enable(LhxUsbProfile *,const LhxUsbCandidate *,int mapped,char *,size_t);
void lhx_usb_runtime_init(LhxUsbRuntime *);
void lhx_usb_runtime_close(LhxUsbRuntime *);
void lhx_usb_refresh(LhxUsbRuntime *,const LhxUsbProfile *);
void lhx_usb_reseed(LhxUsbRuntime *);
void lhx_usb_poll(LhxUsbRuntime *,const LhxUsbProfile *);
int lhx_usb_take_throttle(LhxUsbRuntime *);
int lhx_usb_normalize(const LhxControllerInput *,const LhxCalibration *,const LhxUsbBinding *,int raw,int *lever);
void lhx_usb_physical(const LhxUsbProfile *,const LhxUsbBinding *,char *,size_t);
/* Missing / mapped devices retain generic labels; live raw capabilities guard names. */
void lhx_usb_physical_live(const LhxUsbProfile *,const LhxUsbBinding *,const LhxControllerInput *,char *,size_t);
int lhx_usb_hotas_candidate(const LhxUsbCandidate *);
typedef struct {
  int device, added, skipped, buttons_added, buttons_skipped;
  char flight[4][128];
  char button[16][128];
} LhxHotasPreset;
/* Transactional fill only: existing actions, controls and calibration always win. */
int lhx_usb_hotas_preset(LhxUsbProfile *,const LhxUsbCandidate *,LhxHotasPreset *,char *,size_t);
/* Import only the explicitly selected, effective legacy layout. No guessed roles. */
int lhx_usb_import(LhxUsbProfile *,const LhxControls *,const LhxControllerInput *,char *,size_t);
