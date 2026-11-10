#include "controller_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const LhxControlAction digital[USB_ACTIONS]={
  LHX_NONE,LHX_NONE,LHX_NONE,LHX_NONE,LHX_INCREASE,LHX_DECREASE,LHX_FULL,LHX_HOVER,
  LHX_GEAR,LHX_FIRE,LHX_PREVIOUS,LHX_NEXT,LHX_CHAFF,LHX_FLARES,LHX_COCKPIT,LHX_CHASE,
  LHX_VIEW,LHX_PAUSE,LHX_CONFIRM,LHX_CANCEL,LHX_UP,LHX_DOWN,LHX_LEFT,LHX_RIGHT
};
int lhx_usb_identity(LhxUsbDevice *d,SDL_Joystick *joy) {
  memset(d,0,sizeof *d);d->occurrence=-1;
  SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(joy),d->guid,sizeof d->guid);
#if SDL_VERSION_ATLEAST(2,0,14)
  if (!lhx_usb_hex(d->serial,sizeof d->serial,SDL_JoystickGetSerial(joy))) return 0;
#endif
  /* Names are labels only. Mark long labels honestly; identity is never truncated. */
  const char *name=SDL_JoystickName(joy);char short_name[97];
  if (name && strlen(name)>96) { snprintf(short_name,sizeof short_name,"%.90s...",name);name=short_name; }
  lhx_usb_hex(d->name,sizeof d->name,name);return 1;
}
int lhx_usb_same_identity(const LhxUsbDevice *a,const LhxUsbDevice *b) {
  return !strcmp(a->guid,b->guid) && !strcmp(a->serial,b->serial);
}
int lhx_usb_candidates(LhxUsbCandidate *out,int capacity) {
  int count=SDL_NumJoysticks();if (count>capacity) return -1;
  for (int i=0;i<count;++i) {
    LhxUsbCandidate *c=&out[i];memset(c,0,sizeof *c);c->index=i;c->instance=SDL_JoystickGetDeviceInstanceID(i);
    c->gamepad=SDL_IsGameController(i);snprintf(c->name,sizeof c->name,"%s",SDL_JoystickNameForIndex(i));
    SDL_Joystick *joy=SDL_JoystickOpen(i);
    if (!joy) { snprintf(c->error,sizeof c->error,"Open failed: %.90s",SDL_GetError());continue; }
    c->axes=SDL_JoystickNumAxes(joy);c->buttons=SDL_JoystickNumButtons(joy);c->hats=SDL_JoystickNumHats(joy);
    if (!lhx_usb_identity(&c->identity,joy)) snprintf(c->error,sizeof c->error,"Serial exceeds 120 bytes; cannot store identity safely");
    else if (c->axes>32 || c->buttons>64 || c->hats>4) snprintf(c->error,sizeof c->error,"Exceeds limits: 32 axes / 64 buttons / 4 hats");
    else if (!c->axes && !c->buttons && !c->hats) snprintf(c->error,sizeof c->error,"No controls");
    SDL_JoystickClose(joy);
  }
  return count;
}
int lhx_usb_enable(LhxUsbProfile *p,const LhxUsbCandidate *c,int mapped,char *error,size_t size) {
  if (*c->error || (mapped && !c->gamepad)) {
    snprintf(error,size,"%s",*c->error?c->error:"Device limit (16) or unavailable mapped layout");return -1;
  }
  int slot=-1;
  for (int i=0;i<p->devices;++i) if (lhx_usb_same_identity(&p->device[i],&c->identity) && p->device[i].occurrence==c->instance) slot=i;
  for (int i=0;i<p->devices && slot<0;++i) if (lhx_usb_same_identity(&p->device[i],&c->identity) && p->device[i].occurrence<0) slot=i;
  if (slot<0) {
    if (p->devices>=LHX_USB_DEVICES) { snprintf(error,size,"Device limit (16) reached");return -1; }
    slot=p->devices++;p->device[slot]=c->identity;p->device[slot].mapped=mapped;
  }
  p->version=2;p->device[slot].enabled=1;int matches=0;
  LhxUsbCandidate list[LHX_USB_CANDIDATES];int count=lhx_usb_candidates(list,LHX_USB_CANDIDATES);
  for (int i=0;i<count;++i) matches+=lhx_usb_same_identity(&list[i].identity,&c->identity);
  p->device[slot].session_only |= matches>1;
  p->device[slot].occurrence=p->device[slot].session_only?c->instance:-1;return slot;
}
void lhx_usb_runtime_init(LhxUsbRuntime *r) {
  memset(r,0,sizeof *r);r->throttle=r->throttle_device=-1;
  for (int i=0;i<3;++i) r->owner[i]=-1;
  for (int i=0;i<LHX_USB_DEVICES;++i) r->assigned[i]=-1;
}
void lhx_usb_runtime_close(LhxUsbRuntime *r) {
  for (int i=0;i<LHX_USB_DEVICES;++i) lhx_input_close(&r->input[i]);
  lhx_usb_runtime_init(r);
}
void lhx_usb_reseed(LhxUsbRuntime *r) {
  memset(r->activity,0,sizeof r->activity);r->sequence=0;
  memset(r->seeded,0,sizeof r->seeded);memset(r->previous,0,sizeof r->previous);
  memset(r->active,0,sizeof r->active);memset(r->analog,0,sizeof r->analog);
  for (int a=0;a<3;++a) r->owner[a]=-1;
  r->throttle=r->throttle_device=-1;
}
void lhx_usb_refresh(LhxUsbRuntime *r,const LhxUsbProfile *p) {
  LhxUsbCandidate candidates[LHX_USB_CANDIDATES];int count=lhx_usb_candidates(candidates,LHX_USB_CANDIDATES);
  for (int d=0;d<p->devices;++d) {
    const LhxUsbDevice *dev=&p->device[d];int matches=0,selected=-1,profiles=0;
    for (int i=0;i<p->devices;++i) profiles+=p->device[i].enabled && lhx_usb_same_identity(dev,&p->device[i]);
    for (int i=0;i<count;++i) if (!*candidates[i].error && lhx_usb_same_identity(dev,&candidates[i].identity)) {
      ++matches;if (dev->occurrence==candidates[i].instance) selected=i;
    }
    if ((matches>1 || profiles>1) && selected<0) r->blocked[d]=1;
    int attached=lhx_input_attached(&r->input[d]);
    if (r->input[d].joy && !attached) r->blocked[d]|=r->assigned[d]>=0;
    if (dev->occurrence!=r->assigned[d] && dev->occurrence>=0) r->blocked[d]=0;
    if (!dev->enabled) r->status[d]=USB_DISABLED;
    else if (count<0) r->status[d]=USB_ERROR;
    else if (matches==0) r->status[d]=USB_OFFLINE;
    else if (r->blocked[d] || (dev->session_only && selected<0) || ((matches>1 || profiles>1) && selected<0) || (dev->occurrence>=0 && selected<0)) r->status[d]=USB_AMBIGUOUS;
    else r->status[d]=USB_CONNECTED;
    if (r->status[d]==USB_CONNECTED && selected<0)
      for (int i=0;i<count;++i) if (lhx_usb_same_identity(dev,&candidates[i].identity)) { selected=i;break; }
    if (attached && (r->status[d]!=USB_CONNECTED || SDL_JoystickInstanceID(r->input[d].joy)!=candidates[selected].instance)) attached=0;
    if (!attached && r->input[d].joy) {
      lhx_input_close(&r->input[d]);r->initial_seeded[d]=0;
      if (r->throttle_device==d) r->throttle=-1;
      for (int b=0;b<p->bindings;++b) if (p->binding[b].device==d) r->seeded[b]=0;
    }
    if (r->status[d]==USB_CONNECTED && !attached) {
      /* One occurrence may never feed two profiles. */
      int used=0;for (int j=0;j<d;++j) if (r->input[j].joy && SDL_JoystickInstanceID(r->input[j].joy)==candidates[selected].instance) used=1;
      if (used) r->status[d]=USB_AMBIGUOUS;
      else if (!lhx_input_open_explicit(&r->input[d],candidates[selected].index,dev->mapped)) r->status[d]=USB_ERROR;
      else r->assigned[d]=dev->occurrence;
    }
  }
}
int lhx_usb_normalize(const LhxControllerInput *in,const LhxCalibration *c,const LhxUsbBinding *b,int raw,int *lever) {
  LhxCalibration trigger={1,1,0,0,32767,0,0};
  if (b->axis && !c->set && lhx_input_unipolar(in,b->input)) c=&trigger;
  *lever=b->axis && (b->behavior==USB_DIRECT || c->lever);
  int value=b->axis?lhx_calibrated(c,raw,*lever):raw*32767;
  return b->direction<0?(*lever?32767-value:-value):value;
}
void lhx_usb_poll(LhxUsbRuntime *r,const LhxUsbProfile *p) {
  memset(r->active,0,sizeof r->active);int values[LHX_USB_BINDINGS]={0},live[LHX_USB_BINDINGS]={0};
  for (int d=0;d<p->devices;++d) {
    const LhxControllerInput *in=&r->input[d];
    if (!p->device[d].enabled || !lhx_input_attached(in)) { r->initial_seeded[d]=0;continue; }
    int raw[32]={0};for (int a=0;a<in->axes;++a) raw[a]=lhx_input_axis(in,a);
    if (!r->initial_seeded[d]) {
      /* Measured SDL raw startup can mark an all-minimum snapshot "known"
       * before the first coherent hardware packet. This is a data-state guard,
       * independent of device name, GUID, driver or assumed physical roles. */
      r->initial_pending[d]=in->raw && in->axes>1;
      for (int a=0;a<in->axes;++a) if (raw[a]!=-32768) r->initial_pending[d]=0;
      r->initial_seeded[d]=1;
    } else if (r->initial_pending[d]) {
      int changed=lhx_input_axis_batch(r->initial_values[d],raw,NULL,in->axes,256,NULL);
      if (changed) {
        r->initial_pending[d]=0;
        if (changed>1) {
          for (int b=0;b<p->bindings;++b) if (p->binding[b].device==d) { r->seeded[b]=0;r->activity[b]=0; }
          if (r->throttle_device==d) r->throttle=-1;
        }
      }
    }
    memcpy(r->initial_values[d],raw,sizeof raw);
  }

  for (int n=0;n<p->bindings;++n) {
    const LhxUsbBinding *b=&p->binding[n];const LhxControllerInput *in=&r->input[b->device];
    if (!p->device[b->device].enabled || !lhx_input_attached(in) || b->input>=(b->axis?in->axes:in->buttons)) {
      r->seeded[n]=0;r->activity[n]=0;r->previous[n]=0;continue;
    }
    int lever=0,raw=b->axis?lhx_input_axis(in,b->input):lhx_input_button(in,b->input);
    int value=lhx_usb_normalize(in,&p->device[b->device].calibration[b->axis?b->input:0],b,raw,&lever);
    values[n]=value;live[n]=1;
    int pressed=b->axis?(b->behavior==USB_RELATIVE?abs(value)>b->threshold:value>b->threshold):abs(value)>0;
    int seed=!r->seeded[n];
    if (seed) { r->anchor[n]=value;r->ready[n]=!pressed;r->previous[n]=pressed;r->seeded[n]=1; }
    if (!pressed) r->ready[n]=1;
    int moved=!seed && (abs(value-r->anchor[n])>=(b->behavior==USB_DIRECT?256:1024) ||
      (b->behavior==USB_DIRECT && (value==0 || value==32767) && value!=r->anchor[n]));
    if (moved) r->anchor[n]=value;
    if (b->action<=USB_RUDDER) {
      if (b->behavior==USB_ANALOG) {
        if (moved) r->activity[n]=++r->sequence;
      } else if (pressed && r->ready[n]) {
        values[n]=b->behavior==USB_NEGATIVE?-32767:32767;
        if (!r->previous[n]) r->activity[n]=++r->sequence;
      } else { values[n]=0;live[n]=0; }
    } else if (b->action==USB_THROTTLE) {
      if (b->behavior==USB_DIRECT && moved) { r->throttle=(value*100+16383)/32767;r->throttle_device=b->device; }
      else if (b->behavior==USB_RELATIVE && r->ready[n]) {
        r->active[LHX_INCREASE]|=value>b->threshold;r->active[LHX_DECREASE]|=value<-b->threshold;
      } else if (pressed && r->ready[n]) {
        if (b->behavior==USB_INC) r->active[LHX_INCREASE]=1;
        if (b->behavior==USB_DEC) r->active[LHX_DECREASE]=1;
        if (b->behavior==USB_PRESET && !r->previous[n]) { r->throttle=b->percent;r->throttle_device=b->device; }
      }
    } else if (pressed && r->ready[n]) {
      if ((b->action==USB_HALF && b->behavior!=USB_LEGACY) || (b->action==USB_FULL && b->behavior!=USB_LEGACY)) {
        if (!r->previous[n]) { r->throttle=b->action==USB_FULL?100:50;r->throttle_device=b->device; }
      } else r->active[digital[b->action]]=1;
    }
    r->previous[n]=pressed;
  }
  for (int a=0;a<3;++a) {
    int owner=-1;
    for (int n=0;n<p->bindings;++n) if (p->binding[n].action==a && live[n] && r->activity[n] &&
      (owner<0 || r->activity[n]>r->activity[owner])) owner=n;
    r->owner[a]=owner;r->analog[a]=owner>=0?values[owner]:0;
  }
  /* Explicit relative controls win over simultaneous absolute motion. */
  if (r->active[LHX_INCREASE] || r->active[LHX_DECREASE]) r->throttle=-1;
}
int lhx_usb_take_throttle(LhxUsbRuntime *r) {
  int v=r->throttle;r->throttle=-1;
  if (r->throttle_device<0 || r->throttle_device>=LHX_USB_DEVICES || !lhx_input_attached(&r->input[r->throttle_device])) return -1;
  return v;
}
void lhx_usb_physical_live(const LhxUsbProfile *p,const LhxUsbBinding *b,const LhxControllerInput *in,char *out,size_t size) {
  const LhxUsbDevice *d=&p->device[b->device];char name[100],control[48];lhx_usb_unhex(name,sizeof name,d->name);
  if (b->axis) lhx_input_axis_label(d->mapped?NULL:in,b->input,control,sizeof control);
  else if (b->input<64 || d->mapped) snprintf(control,sizeof control,"%s %d",d->mapped?"Mapped button":"Button",b->input+1);
  else { const char *dirs[]={"up","right","down","left"};snprintf(control,sizeof control,"Hat %d %s",(b->input-64)/4+1,dirs[(b->input-64)%4]); }
  snprintf(out,size,"D%d %.32s [%.*s] / %s",b->device+1,*name?name:"Device",8,*d->serial?d->serial:d->guid,control);
}
void lhx_usb_physical(const LhxUsbProfile *p,const LhxUsbBinding *b,char *out,size_t size) {
  lhx_usb_physical_live(p,b,NULL,out,size);
}
int lhx_usb_hotas_candidate(const LhxUsbCandidate *c) {
  return !*c->error && SDL_JoystickGetDeviceInstanceID(c->index)==c->instance &&
    lhx_input_hotas_match(1,SDL_JoystickGetDeviceVendor(c->index),SDL_JoystickGetDeviceProduct(c->index),c->axes,c->buttons,c->hats);
}
static const char *preset_skip(const LhxUsbProfile *p,const LhxUsbBinding *b) {
  for (int n=0;n<p->bindings;++n) if (p->binding[n].action==b->action) return "kept existing action binding(s)";
  for (int n=0;n<p->bindings;++n) if (p->binding[n].device==b->device && p->binding[n].axis==b->axis && p->binding[n].input==b->input)
    return "skipped: control already assigned";
  if (b->axis && b->behavior==USB_ANALOG && p->device[b->device].calibration[b->input].set && p->device[b->device].calibration[b->input].lever)
    return "skipped: existing lever calibration";
  if (p->bindings==LHX_USB_BINDINGS) return "skipped: binding limit";
  return NULL;
}
int lhx_usb_hotas_preset(LhxUsbProfile *p,const LhxUsbCandidate *c,LhxHotasPreset *report,char *error,size_t size) {
  LhxControllerInput in;
  if (!lhx_usb_profile_valid(p)) { snprintf(error,size,"Existing USB draft is invalid; preset left it unchanged");return 0; }
  if (!lhx_usb_hotas_candidate(c) || !lhx_input_open_explicit(&in,c->index,0)) {
    snprintf(error,size,"Preset needs Linux native 044f:b68d with 8 axes / 15 buttons / 1 hat");return 0;
  }
  LhxUsbDevice identity;int valid=lhx_input_hotas_one(&in) && lhx_usb_identity(&identity,in.joy) && lhx_usb_same_identity(&identity,&c->identity);
  lhx_input_close(&in);
  if (!valid) { snprintf(error,size,"Device changed. Select the connected HOTAS again");return 0; }
  LhxUsbProfile draft=*p;LhxHotasPreset result={0};
  for (int d=0;d<p->devices;++d) if (lhx_usb_same_identity(&p->device[d],&c->identity) &&
      (p->device[d].occurrence<0 || p->device[d].occurrence==c->instance)) {
    if (p->device[d].mapped) { snprintf(error,size,"Existing HOTAS profile is mapped. Preserve it; select native in Devices before using this preset");return 0; }
    if (p->device[d].session_only && p->device[d].occurrence<0) { snprintf(error,size,"HOTAS needs explicit session reassignment in Devices first");return 0; }
  }
  int device=lhx_usb_enable(&draft,c,0,error,size);if (device<0) return 0;
  result.device=device;
  const int axes[]={0,1,7,2};
  for (int a=0;a<4;++a) {
    const LhxCalibration *cal=&draft.device[device].calibration[axes[a]];
    LhxUsbBinding b={device,1,axes[a],a,a==USB_THROTTLE?USB_DIRECT:USB_ANALOG,
      a==USB_THROTTLE && !(cal->set && cal->lever)?-1:1,50,16000};
    const char *reason=preset_skip(&draft,&b);
    snprintf(result.flight[a],sizeof result.flight[a],"%s / Axis %d: %s",lhx_usb_action_label(a),axes[a]+1,reason?reason:"added");
    if (reason) ++result.skipped;
    else { draft.binding[draft.bindings++]=b;++result.added; }
  }
  /* Numbered host defaults, not manufacturer button-name claims. Buttons
   * 13-15 remain unassigned. Custom actions / controls take precedence. */
  static const int buttons[]={USB_FIRE,USB_CONFIRM,USB_NEXT,USB_PREVIOUS,USB_CHAFF,USB_FLARES,USB_GEAR,
    USB_VIEW,USB_PAUSE,USB_BACK,USB_COCKPIT,USB_CHASE};
  const char *dirs[]={"up","right","down","left"};
  for (int k=0;k<16;++k) {
    int action=k<12?buttons[k]:k==12?USB_UP:k==13?USB_RIGHT:k==14?USB_DOWN:USB_LEFT;
    LhxUsbBinding b={device,0,k<12?k:64+k-12,action,USB_HOLD,1,50,16000};
    const char *reason=preset_skip(&draft,&b);char control[32];
    if (k<12) snprintf(control,sizeof control,"Button %d",k+1);
    else snprintf(control,sizeof control,"Hat 1 %s",dirs[k-12]);
    snprintf(result.button[k],sizeof result.button[k],"%s / %s: %s",control,lhx_usb_action_label(action),reason?reason:"added");
    if (reason) ++result.buttons_skipped;
    else { draft.binding[draft.bindings++]=b;++result.buttons_added; }
  }
  if (!lhx_usb_profile_valid(&draft)) { snprintf(error,size,"Preset would create an invalid profile. Existing draft retained");return 0; }
  *p=draft;*report=result;return 1;
}
int lhx_usb_import(LhxUsbProfile *p,const LhxControls *c,const LhxControllerInput *in,char *error,size_t size) {
  if (!in->joy || !lhx_input_attached(in)) { snprintf(error,size,"Connect the legacy selected device before importing");return 0; }
  LhxUsbProfile draft={.version=2,.devices=1};
  if (!lhx_usb_identity(&draft.device[0],in->joy)) { snprintf(error,size,"Cannot store identity");return 0; }
  draft.device[0].enabled=1;draft.device[0].mapped=!in->raw;
  /* Explicitly importing identifies this occurrence for this session only. */
  LhxUsbCandidate list[LHX_USB_CANDIDATES];int count=lhx_usb_candidates(list,LHX_USB_CANDIDATES),matches=0;
  for (int i=0;i<count;++i) matches+=lhx_usb_same_identity(&list[i].identity,&draft.device[0]);
  if (matches>1) { draft.device[0].session_only=1;draft.device[0].occurrence=SDL_JoystickInstanceID(in->joy); }
  for (int axis=0;axis<2;++axis) for (int i=0;i<(axis?in->axes:in->buttons);++i) {
    int action=-1,behavior=USB_HOLD,direction=1;
    if (axis) {
      LhxAxisBinding a=in->raw?c->raw_axes[i]:c->axes[i];direction=a.direction;
      if (a.action>=LHX_ROLL && a.action<=LHX_COLLECTIVE) { action=a.action-LHX_ROLL;behavior=action==USB_THROTTLE?USB_RELATIVE:USB_ANALOG;if (action==USB_THROTTLE) direction=-direction; }
      else if (a.action>=LHX_AXIS_FIRE) {
        int map[]={USB_FIRE,USB_HALF,USB_CHAFF,USB_FLARES};action=map[a.action-LHX_AXIS_FIRE];
      }
    } else { LhxControlAction a=in->raw?c->raw_buttons[i]:c->buttons[i];for (int j=4;j<USB_ACTIONS;++j) if (digital[j]==a) action=j; }
    if (action<0) continue;
    if (action==USB_HALF || action==USB_FULL) behavior=USB_LEGACY;
    if (axis) draft.device[0].calibration[i]=(LhxCalibration){1,0,-32768,0,32767,0,
      action<=USB_RUDDER?(int)c->deadzone:0};
    if (draft.bindings==128) { snprintf(error,size,"Import exceeds binding limit (128)");return 0; }
    draft.binding[draft.bindings++]=(LhxUsbBinding){0,axis,i,action,behavior,direction,50,axis && action>=USB_GEAR?(int)c->trigger_threshold:(int)c->axis_button_threshold};
    /* The legacy key-3 preset remains key-3 when imported, via a hold action. */
  }
  *p=draft;return 1;
}
