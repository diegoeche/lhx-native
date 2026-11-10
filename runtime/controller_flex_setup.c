#include "controller_setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define color lhx_ui_color
#define rect lhx_ui_rect
#define text lhx_ui_text
#define wrapped lhx_ui_wrapped
#define button(...) lhx_ui_button(__VA_ARGS__)
/* Pages: actions, devices, behavior preview, calibration. */
static const char *const statuses[]={"Disconnected","Enabled / connected","Reassign required","Disabled","Open / capacity error"};
static void refresh(LhxControllerSetup *);
static void say(LhxControllerSetup *s,const char *message) { snprintf(s->status,sizeof s->status,"%s",message); }
static int hotas_target(const LhxControllerSetup *s,LhxUsbCandidate *target,char *error,size_t size) {
  LhxUsbCandidate list[LHX_USB_CANDIDATES];int count=lhx_usb_candidates(list,LHX_USB_CANDIDATES),known=0,found=-1;
  SDL_JoystickID chosen=-1;
  if (s->flex.enabled && s->flex.page==1) {
    int n=s->flex.device_cursor-s->edits.usb.devices;
    if (n>=0 && n<s->flex.candidate_count) chosen=s->flex.candidates[n].instance;
    else if (s->flex.device_cursor>=0 && s->flex.device_cursor<s->edits.usb.devices &&
        lhx_input_hotas_one(&s->flex.live.input[s->flex.device_cursor]))
      chosen=SDL_JoystickInstanceID(s->flex.live.input[s->flex.device_cursor].joy);
  }
  if (s->flex.enabled && s->flex.page==0) {
    int device=s->flex.monitor_device,slot=0;
    for (int n=0;n<s->edits.usb.bindings;++n) if (s->edits.usb.binding[n].action==s->row && slot++==s->flex.binding) {
      device=s->edits.usb.binding[n].device;break;
    }
    if (device>=0 && device<s->edits.usb.devices && lhx_input_hotas_one(&s->flex.live.input[device]))
      chosen=SDL_JoystickInstanceID(s->flex.live.input[device].joy);
  }
  for (int i=0;i<count;++i) if (lhx_usb_hotas_candidate(&list[i])) {
    ++known;if (found<0 || list[i].instance==chosen) found=i;
  }
  if (found<0 || (known>1 && list[found].instance!=chosen)) {
    snprintf(error,size,"%s",count<0?"Device list exceeds capacity":known>1?
      "Several HOTAS units: select / reassign the intended occurrence in Devices first":
      "Connect a Linux native HOTAS One 044f:b68d (8 axes / 15 buttons / 1 hat)");return 0;
  }
  for (int d=0;d<s->edits.usb.devices;++d) if (lhx_usb_same_identity(&s->edits.usb.device[d],&list[found].identity) && s->flex.live.blocked[d]) {
    snprintf(error,size,"HOTAS identity is ambiguous. Explicitly reassign it in Devices first");return 0;
  }
  for (int d=0;d<s->edits.usb.devices;++d) if (lhx_usb_same_identity(&s->edits.usb.device[d],&list[found].identity) &&
      (s->edits.usb.device[d].occurrence<0 || s->edits.usb.device[d].occurrence==list[found].instance) && s->edits.usb.device[d].mapped) {
    snprintf(error,size,"Existing HOTAS profile uses mapped controls. Review native / mapped in Devices before applying this preset");return 0;
  }
  /* A mapped selection is deliberately not treated as the raw physical layout. */
  if (s->input.joy && !s->input.raw && SDL_JoystickInstanceID(s->input.joy)==list[found].instance) {
    snprintf(error,size,"Selected HOTAS uses a mapped layout. Select native controls before applying this preset");return 0;
  }
  *target=list[found];return 1;
}
int lhx_flex_hotas_offer(const LhxControllerSetup *s) {
  LhxUsbCandidate c;char error[256];return hotas_target(s,&c,error,sizeof error);
}
void lhx_flex_hotas_preset(LhxControllerSetup *s) {
  LhxUsbCandidate c;if (!hotas_target(s,&c,s->status,sizeof s->status)) return;
  LhxControls draft=s->edits;LhxHotasPreset report;
  if (!s->flex.enabled) {
    if (lhx_input_attached(&s->input)) {
      if (!lhx_usb_import(&draft.usb,&s->edits,&s->input,s->status,sizeof s->status)) return;
    } else if (*s->edits.controller_guid) {
      say(s,"Reconnect the selected legacy device before applying a preset so its custom bindings can be imported");return;
    } else draft.usb=(LhxUsbProfile){.version=2};
  }
  if (!lhx_usb_hotas_preset(&draft.usb,&c,&report,s->status,sizeof s->status)) return;
  s->edits=draft;lhx_input_close(&s->input);s->instance=-1;
  LhxFlexibleSetup *f=&s->flex;lhx_usb_runtime_close(&f->live);f->enabled=1;f->import_prompt=0;
  f->page=4;f->hotas=report;f->hotas_page=0;f->binding=f->binding_scroll=f->footer_focus=0;f->monitor_device=report.device;f->monitor=1;
  s->row=s->scroll=s->capture=s->conflict=0;refresh(s);
  say(s,"HOTAS preset added to draft. Existing custom bindings / calibration retained. Enter: review actions; Save or Session applies.");
}
static int same_control(const LhxControllerSetup *s,const LhxUsbBinding *a,const LhxUsbBinding *b) {
  if (a->device!=b->device) return 0;
  if (a->axis==b->axis && a->input==b->input) return 1;
  const LhxControllerInput *in=&s->flex.live.input[a->device];
  if (in->raw || !lhx_input_attached(in)) return 0;
  SDL_GameControllerButtonBind x=lhx_input_bind(in,a->axis,a->input),y=lhx_input_bind(in,b->axis,b->input);
  if (x.bindType==SDL_CONTROLLER_BINDTYPE_NONE || x.bindType!=y.bindType) return 0;
  if (x.bindType==SDL_CONTROLLER_BINDTYPE_AXIS) return x.value.axis==y.value.axis;
  if (x.bindType==SDL_CONTROLLER_BINDTYPE_BUTTON) return x.value.button==y.value.button;
  return x.value.hat.hat==y.value.hat.hat && (x.value.hat.hat_mask & y.value.hat.hat_mask);
}
static void refresh(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;
  f->candidate_count=lhx_usb_candidates(f->candidates,LHX_USB_CANDIDATES);
  for (int d=0;d<s->edits.usb.devices;++d) {
    int matches=0;for (int i=0;i<f->candidate_count;++i) matches+=lhx_usb_same_identity(&s->edits.usb.device[d],&f->candidates[i].identity);
    if (matches>1) s->edits.usb.device[d].session_only=1;
  }
  lhx_usb_refresh(&f->live,&s->edits.usb);
}
void lhx_flex_init(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;lhx_usb_runtime_init(&f->live);
  f->enabled=s->edits.usb.version==2;f->binding=f->editing=f->reassign=-1;
  if (f->enabled) { refresh(s);say(s,"Select action, Add binding, then use an ENABLED device. D: devices / enable / reassign."); }
}
void lhx_flex_close(LhxControllerSetup *s) { lhx_usb_runtime_close(&s->flex.live); }
static int action_bindings(const LhxControllerSetup *s,int *indices) {
  int n=0;for (int i=0;i<s->edits.usb.bindings;++i) if (s->edits.usb.binding[i].action==s->row) indices[n++]=i;return n;
}
static int selected(const LhxControllerSetup *s) {
  int list[LHX_USB_BINDINGS];int n=action_bindings(s,list);
  return s->flex.binding>=0 && s->flex.binding<n?list[s->flex.binding]:-1;
}
void lhx_flex_bindings(const LhxControllerSetup *s,int row,char *out,size_t size) {
  if (!size) return;
  *out=0;int total=0;char physical[160],line[220];
  for (int i=0;i<s->edits.usb.bindings;++i) {
    const LhxUsbBinding *b=&s->edits.usb.binding[i];if (b->action!=row) continue;++total;
    lhx_usb_physical_live(&s->edits.usb,b,&s->flex.live.input[b->device],physical,sizeof physical);
    snprintf(line,sizeof line,"%s: %s%s",physical,lhx_usb_behavior_label(b->behavior),b->direction<0?" inverted":"");
    size_t used=strlen(out);
    if (used+strlen(line)+4<size) snprintf(out+used,size-used,"%s%s",used?", ":"",line);
    else { snprintf(out,size,"%d bindings - select each in the scrollable binding list",total);return; }
  }
  if (!total) snprintf(out,size,"Unbound");
}
static void record_signal(LhxControllerSetup *s,int device,int axis,int input,int value) {
  LhxFlexibleSetup *f=&s->flex;LhxUsbBinding b={.device=device,.axis=axis,.input=input};char physical[150];
  lhx_usb_physical_live(&s->edits.usb,&b,&f->live.input[device],physical,sizeof physical);
  for (int i=2;i>0;--i) memcpy(f->signal[i],f->signal[i-1],sizeof f->signal[i]);
  snprintf(f->signal[0],sizeof f->signal[0],"%.144s = %d",physical,value);if (f->signals<3) ++f->signals;
}
static void cancel_step(LhxControllerSetup *s,const char *reason) {
  s->capture=0;s->flex.page=0;s->flex.editing=-1;s->flex.cal_bits=0;s->flex.resolution=0;say(s,reason);
}
static void propose(LhxControllerSetup *s,int d,int axis,int input,int value) {
  LhxFlexibleSetup *f=&s->flex;
  f->proposal=(LhxUsbBinding){d,axis,input,s->row,USB_HOLD,axis && value<0?-1:1,50,16000};
  if (s->row<=USB_RUDDER) { f->proposal.behavior=axis?USB_ANALOG:USB_POSITIVE;f->proposal.direction=1; }
  if (s->row==USB_THROTTLE) { f->proposal.behavior=axis?USB_DIRECT:USB_INC;f->proposal.direction=1; }
  f->page=2;s->capture=0;f->collisions=f->resolution=0;f->collision_scroll=0;
  for (int n=0;n<s->edits.usb.bindings;++n) if (n!=f->editing && same_control(s,&f->proposal,&s->edits.usb.binding[n])) ++f->collisions;
  say(s,"Choose behavior and preview. Enter confirms this binding. Escape retains the previous draft.");
}
static void observe(LhxControllerSetup *s,int d,int axis,int input,int value,int capture) {
  LhxFlexibleSetup *f=&s->flex;
  if (!s->edits.usb.device[d].enabled || !lhx_input_attached(&f->live.input[d])) return;
  if (axis) {
    int old=f->axes[d][input];f->axes[d][input]=value;
    if (abs(value-f->reported[d][input])>=1024) { record_signal(s,d,1,input,value);f->reported[d][input]=value; }
    if (f->page==3 && f->proposal.device==d && f->proposal.input==input && abs(value-old)>256) f->sample_count=f->sample_next=0;
    if (!capture || !s->capture || !s->focused) return;
    if (!f->axis_ready[d][input]) {
      if (abs(value)<12000) { f->axis_ready[d][input]=1;f->baseline[d][input]=value; }
      return;
    }
    /* Axes are proposed only after the complete event/poll batch in tick. */
  } else {
    int old=f->buttons[d][input];f->buttons[d][input]=(Uint8)value;
    if (value!=old) record_signal(s,d,0,input,value);
    if (!value) f->ready[d][input]=1;
    if (capture && s->capture && s->focused && value && !old && f->ready[d][input]) propose(s,d,0,input,value);
  }
}
static void capture(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;refresh(s);int connected=0;
  if (s->edits.usb.bindings>=128 && f->editing<0) { say(s,"128 binding limit reached. Remove a binding before adding another.");return; }
  SDL_JoystickUpdate();
  for (int d=0;d<s->edits.usb.devices;++d) {
    LhxControllerInput *in=&f->live.input[d];if (!lhx_input_attached(in)) continue;++connected;
    for (int a=0;a<in->axes;++a) {
      f->axes[d][a]=f->baseline[d][a]=lhx_input_axis(in,a);
      f->axis_ready[d][a]=s->row==USB_THROTTLE || abs(f->axes[d][a])<20000;
    }
    for (int b=0;b<in->buttons;++b) { f->buttons[d][b]=lhx_input_button(in,b);f->ready[d][b]=!f->buttons[d][b]; }
  }
  if (!connected) { say(s,"No enabled connected device. D: list devices and explicitly enable one first.");return; }
  f->capture_time=SDL_GetTicks();s->capture=1;say(s,"Listening on ENABLED devices. Release held controls first, then press / move. Escape cancels.");
}
static void choose_behavior(LhxControllerSetup *s,int direction) {
  LhxUsbBinding *b=&s->flex.proposal;
  for (int n=0;n<USB_BEHAVIORS;++n) {
    b->behavior=(b->behavior+USB_BEHAVIORS+direction)%USB_BEHAVIORS;
    if (lhx_usb_behavior_allowed(b,b->behavior)) break;
  }
}
static void remove_binding(LhxUsbProfile *p,int n) {
  memmove(&p->binding[n],&p->binding[n+1],(size_t)(p->bindings-n-1)*sizeof p->binding[0]);--p->bindings;
}
static void confirm_binding(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;LhxUsbProfile draft=s->edits.usb;
  if (!lhx_input_attached(&f->live.input[f->proposal.device])) { cancel_step(s,"Binding device disconnected. No binding changed.");return; }
  if (f->collisions && !f->resolution) { say(s,"Same physical control is already used. R: explicitly reuse, or M: move only those bindings.");return; }
  if (f->editing>=0) remove_binding(&draft,f->editing);
  if (f->resolution==2) for (int i=draft.bindings-1;i>=0;--i) if (same_control(s,&draft.binding[i],&f->proposal)) remove_binding(&draft,i);
  if (draft.bindings>=128) { say(s,"Binding limit (128) reached");return; }
  draft.binding[draft.bindings++]=f->proposal;
  if (!lhx_usb_profile_valid(&draft)) { say(s,"Behavior needs a centered calibration, or profile values are invalid. Calibrate / change behavior.");return; }
  s->edits.usb=draft;lhx_usb_reseed(&f->live);f->page=0;f->editing=-1;
  int list[128];f->binding=action_bindings(s,list)-1;
  say(s,"Binding confirmed in draft. All other controls preserved. Save or Session applies the draft.");
}
static void calibrate(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;
  if (f->page==0) { int n=selected(s);if (n<0) { say(s,"Select an axis binding first");return; }f->proposal=s->edits.usb.binding[n]; }
  if (!f->proposal.axis) { say(s,"Calibration needs an axis binding");return; }
  if (f->proposal.input>=f->live.input[f->proposal.device].axes) { say(s,"Bound axis is unavailable on this device. Reconnect or change the physical control.");return; }
  if (!lhx_input_attached(&f->live.input[f->proposal.device])) { say(s,"Calibration device is disconnected / disabled");return; }
  f->cal_return=f->page;f->page=3;f->cal_bits=0;f->cal_flip=0;f->sample_count=f->sample_next=0;f->last_sample=0;
  f->calibration=s->edits.usb.device[f->proposal.device].calibration[f->proposal.input];
  if (!f->calibration.set) f->calibration=(LhxCalibration){1,f->proposal.behavior==USB_DIRECT,-32768,0,32767,0,6000};
  say(s,"Explicit calibration: select centered or lever. Hold each position steady, then capture each labelled point.");
}
static void sample_point(LhxControllerSetup *s,int point) {
  LhxFlexibleSetup *f=&s->flex;int n=f->sample_count,lo=32767,hi=-32768;Uint32 oldest=SDL_GetTicks();
  for (int i=0;i<n;++i) { if (f->samples[i]<lo) lo=f->samples[i];if (f->samples[i]>hi) hi=f->samples[i];if (f->sample_time[i]<oldest) oldest=f->sample_time[i]; }
  int current=f->axes[f->proposal.device][f->proposal.input];
  Uint32 now=SDL_GetTicks();
  if (n<12 || now-oldest<200 || now-oldest>1000 || now-f->last_sample>50 || hi-lo>256 || current<lo-256 || current>hi+256) { say(s,"Not stable: hold at this point for 0.2 seconds (12 samples), spread <=256 raw units. Retry.");return; }
  if (f->calibration.lever && point!=1) {
    f->endpoints[point==2]=(lo+hi)/2;
    if (point==0) f->calibration.minimum=f->endpoints[0];
    if (point==2) f->calibration.maximum=f->endpoints[1];
    if (((f->cal_bits | (1<<point)) & 5)==5) {
      int reverse=f->endpoints[0]>f->endpoints[1];
      f->calibration.minimum=f->endpoints[reverse?1:0];f->calibration.maximum=f->endpoints[reverse?0:1];
      f->calibration.invert=reverse ^ f->cal_flip;
    }
  } else if (point==0) f->calibration.minimum=(lo+hi)/2;
  if (point==1) f->calibration.center=(lo+hi)/2;
  if (point==2 && !f->calibration.lever) f->calibration.maximum=(lo+hi)/2;
  f->cal_bits|=1<<point;f->sample_count=f->sample_next=0;
  say(s,"Point captured in calibration draft. Capture the remaining points, then Enter to validate.");
}
static void confirm_calibration(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;int need=f->calibration.lever?5:7;
  if ((f->cal_bits & need)!=need || !lhx_calibration_valid(&f->calibration)) { say(s,"Incomplete / invalid calibration. Min/max span >=4096; centered sides >=2048 and beyond deadzone. Prior calibration retained.");return; }
  LhxUsbProfile copy=s->edits.usb;copy.device[f->proposal.device].calibration[f->proposal.input]=f->calibration;
  if (!lhx_usb_profile_valid(&copy)) { say(s,"This axis has centered analog / relative bindings. Keep centered calibration or edit those bindings first.");return; }
  s->edits.usb=copy;lhx_usb_reseed(&f->live);f->page=f->cal_return;
  say(s,"Calibration accepted into draft for this device / axis. Save or Session applies it. Escape cancels the setup.");
}
static void enable_selected(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;int n=f->device_cursor,devices=s->edits.usb.devices;
  if (n<devices) {
    s->edits.usb.device[n].enabled=!s->edits.usb.device[n].enabled;
    say(s,s->edits.usb.device[n].enabled?"Configured device enabled in draft":"Device disabled in draft. Its bindings / calibration are retained.");
  } else if (n-devices>=0 && n-devices<f->candidate_count) {
    LhxUsbCandidate *c=&f->candidates[n-devices];
    if (f->reassign>=0) {
      LhxUsbDevice *d=&s->edits.usb.device[f->reassign];
      if (!lhx_usb_same_identity(d,&c->identity)) { say(s,"Choose a candidate with the same GUID + serial as this profile");return; }
      for (int i=0;i<devices;++i) if (i!=f->reassign && s->edits.usb.device[i].occurrence==c->instance) { say(s,"Occurrence already assigned to another profile. Reassign / disable that profile first.");return; }
      d->occurrence=c->instance;d->session_only=1;d->enabled=1;f->live.blocked[f->reassign]=0;f->reassign=-1;say(s,"Occurrence assigned for this session only. Replug / restart requires reassignment when ambiguous.");
    } else if (lhx_usb_enable(&s->edits.usb,c,0,s->status,sizeof s->status)>=0) say(s,"Device explicitly enabled with native controls. No guessed bindings / calibration. G selects mapped layout before binding.");
  }
  refresh(s);lhx_usb_reseed(&f->live);
}
void lhx_flex_tick(LhxControllerSetup *s) {
  LhxFlexibleSetup *f=&s->flex;if (f->import_prompt) return;
  SDL_JoystickUpdate();
  for (int d=0;d<s->edits.usb.devices;++d) if (f->live.input[d].joy && !lhx_input_attached(&f->live.input[d])) {
    if (s->capture || ((f->page==2 || f->page==3) && f->proposal.device==d)) cancel_step(s,"Device disconnected. Capture / behavior / calibration cancelled; draft retained.");
  }
  refresh(s);
  for (int d=0;d<s->edits.usb.devices;++d) {
    LhxControllerInput *in=&f->live.input[d];if (!lhx_input_attached(in)) continue;
    for (int a=0;a<in->axes;++a) observe(s,d,1,a,lhx_input_axis(in,a),1);
    for (int b=0;b<in->buttons;++b) observe(s,d,0,b,lhx_input_button(in,b),1);
  }
  if (s->capture) {
    int count=0,device=0,axis=0;
    for (int d=0;d<s->edits.usb.devices;++d) {
      int last=0,changes=lhx_input_axis_batch(f->baseline[d],f->axes[d],f->axis_ready[d],f->live.input[d].axes,12000,&last);
      if (changes) { count+=changes;device=d;axis=last; }
    }
    if (count==1) propose(s,device,1,axis,f->axes[device][axis]);
    if (count>1) {
      /* No hardware-role guess: rearm from the observed batch and require an
       * isolated movement, even when SDL marked the old all-min state known. */
      for (int d=0;d<s->edits.usb.devices;++d) for (int a=0;a<f->live.input[d].axes;++a) {
        f->baseline[d][a]=f->axes[d][a];
        f->axis_ready[d][a]=s->row==USB_THROTTLE || abs(f->axes[d][a])<20000;
      }
      say(s,"Several axes changed together (initial positions or ambiguous movement). Baselines reset; move ONLY the intended control again.");
    }
    }
  lhx_usb_poll(&f->live,&s->edits.usb); /* Preview only. No game callback or memory access. */
  if (f->page==3 && s->focused) {
    Uint32 now=SDL_GetTicks();
    if (!f->last_sample || now-f->last_sample>=10) {
      if (f->last_sample && now-f->last_sample>50) f->sample_count=f->sample_next=0;
      f->last_sample=now;f->samples[f->sample_next]=f->axes[f->proposal.device][f->proposal.input];f->sample_time[f->sample_next]=now;
      f->sample_next=(f->sample_next+1)%32;if (f->sample_count<32) ++f->sample_count;
    }
  }
}
static void select_action(LhxControllerSetup *s,int row) {
  s->row=(row+USB_ACTIONS)%USB_ACTIONS;s->flex.binding=0;s->flex.binding_scroll=0;
  if (s->row<s->scroll) s->scroll=s->row;
  if (s->row>=s->scroll+13) s->scroll=s->row-12;
}
static void move_binding(LhxControllerSetup *s,int delta) {
  int list[128],n=action_bindings(s,list);if (!n) { s->flex.binding=-1;return; }
  s->flex.binding=(s->flex.binding+n+delta)%n;
  if (s->flex.binding<s->flex.binding_scroll) s->flex.binding_scroll=s->flex.binding;
  if (s->flex.binding>=s->flex.binding_scroll+4) s->flex.binding_scroll=s->flex.binding-3;
}
void lhx_flex_event(LhxControllerSetup *s,const SDL_Event *e) {
  LhxFlexibleSetup *f=&s->flex;
  if (e->type==SDL_QUIT || (e->type==SDL_WINDOWEVENT && e->window.event==SDL_WINDOWEVENT_CLOSE)) { s->result=LHX_SETUP_CANCEL;return; }
  if (e->type==SDL_WINDOWEVENT) {
    if (e->window.event==SDL_WINDOWEVENT_FOCUS_LOST) { s->focused=0;if (s->capture || f->page==2 || f->page==3) cancel_step(s,"Focus lost. Pending binding / calibration discarded.");lhx_usb_reseed(&f->live); }
    if (e->window.event==SDL_WINDOWEVENT_FOCUS_GAINED) { s->focused=1;lhx_usb_reseed(&f->live); }
    return;
  }
  if (!s->focused) return;
  if (e->type==SDL_JOYDEVICEADDED || e->type==SDL_JOYDEVICEREMOVED) {
    for (int d=0;d<s->edits.usb.devices;++d) if (f->live.input[d].joy && !lhx_input_attached(&f->live.input[d]) &&
      (s->capture || ((f->page==2 || f->page==3) && f->proposal.device==d))) cancel_step(s,"Device disconnected. Pending capture / calibration cancelled.");
    refresh(s);return;
  }
  if (e->type==SDL_JOYAXISMOTION || e->type==SDL_JOYBUTTONDOWN || e->type==SDL_JOYBUTTONUP || e->type==SDL_JOYHATMOTION ||
      e->type==SDL_CONTROLLERAXISMOTION || e->type==SDL_CONTROLLERBUTTONDOWN || e->type==SDL_CONTROLLERBUTTONUP) {
    if (s->capture && e->common.timestamp && (Sint32)(e->common.timestamp-f->capture_time)<0) return;
    for (int d=0;d<s->edits.usb.devices;++d) {
      LhxControllerInput *in=&f->live.input[d];if (!lhx_input_attached(in) || !s->edits.usb.device[d].enabled) continue;
      SDL_JoystickID id=SDL_JoystickInstanceID(in->joy);
      if (in->raw && e->jaxis.which==id) {
        if (e->type==SDL_JOYAXISMOTION && e->jaxis.axis<in->axes) observe(s,d,1,e->jaxis.axis,e->jaxis.value,1);
        if ((e->type==SDL_JOYBUTTONDOWN || e->type==SDL_JOYBUTTONUP) && e->jbutton.button<in->buttons) observe(s,d,0,e->jbutton.button,e->type==SDL_JOYBUTTONDOWN,1);
        if (e->type==SDL_JOYHATMOTION && e->jhat.hat<in->hats) for (int k=0;k<4;++k) observe(s,d,0,64+e->jhat.hat*4+k,!!(e->jhat.value&(1<<k)),1);
      } else if (!in->raw && e->caxis.which==id) {
        if (e->type==SDL_CONTROLLERAXISMOTION && e->caxis.axis<in->axes) observe(s,d,1,e->caxis.axis,e->caxis.value,1);
        if (e->type==SDL_CONTROLLERBUTTONDOWN || e->type==SDL_CONTROLLERBUTTONUP) observe(s,d,0,e->cbutton.button,e->type==SDL_CONTROLLERBUTTONDOWN,1);
      }
    }
    return;
  }
  SDL_Keycode key=0;int x=-1,y=-1;
  if (e->type==SDL_KEYDOWN && !e->key.repeat) key=e->key.keysym.sym;
  if (e->type==SDL_MOUSEBUTTONDOWN && e->button.button==SDL_BUTTON_LEFT) { x=e->button.x;y=e->button.y; }
  if (key==SDLK_ESCAPE) {
    if (f->page==4) { f->page=0;say(s,"Preset remains in draft. Review actions; Save or Session applies, Cancel exits without applying."); }
    else if (f->import_prompt) { f->enabled=f->import_prompt=0;say(s,"USB draft cancelled. Legacy profile remains active."); }
    else if (s->capture || f->page==2 || f->page==3) cancel_step(s,"Pending edit cancelled. Existing bindings and calibration retained.");
    else if (f->page==1) { f->page=0;f->reassign=-1; }
    else s->result=LHX_SETUP_CANCEL;
    return;
  }
  if (f->import_prompt) {
    if (key==SDLK_i || (x>=40 && x<570 && y>=250 && y<286)) {
      if (!lhx_usb_import(&s->edits.usb,&s->edits,&s->input,s->status,sizeof s->status)) return;
    } else if (key==SDLK_e || (x>=620 && x<1150 && y>=250 && y<286)) s->edits.usb=(LhxUsbProfile){.version=2};
    else if (x>=940 && x<1180 && y>=766 && y<812) { f->import_prompt=f->enabled=0;return; }
    else return;
    f->import_prompt=0;lhx_input_close(&s->input);s->instance=-1;refresh(s);s->row=s->scroll=0;
    say(s,"Independent USB controls draft. D: enable and review devices, then choose an action to add a binding.");return;
  }
  if (x>=940 && x<1180 && y>=766 && y<812) { s->result=LHX_SETUP_CANCEL;return; }
  if (s->capture) { if (x>=900 && x<1180 && y>=638 && y<674) cancel_step(s,"Capture cancelled. Bindings retained.");return; }
  if (f->page==4) {
    if (f->footer_focus && (key==SDLK_RETURN || key==SDLK_SPACE)) {
      s->result=f->selection==0?LHX_SETUP_SAVE:f->selection==1?LHX_SETUP_SESSION:LHX_SETUP_CANCEL;return;
    }
    if (key==SDLK_RETURN || (x>=40 && x<580 && y>=630 && y<666)) f->page=0;
    if (key==SDLK_b || key==SDLK_PAGEDOWN || (x>=620 && x<1150 && y>=630 && y<666)) f->hotas_page=(f->hotas_page+1)%3;
    if (key==SDLK_PAGEUP) f->hotas_page=(f->hotas_page+2)%3;
    if (key==SDLK_F5 || (x>=20 && x<360 && y>=766 && y<812)) s->result=LHX_SETUP_SAVE;
    if (key==SDLK_F6 || (x>=380 && x<920 && y>=766 && y<812)) s->result=LHX_SETUP_SESSION;
    if (key==SDLK_TAB) {
      if (!f->footer_focus) { f->footer_focus=1;f->selection=0; }
      else f->selection=(f->selection+((e->key.keysym.mod & KMOD_SHIFT)?2:1))%3;
    }
    return;
  }
  if (f->page==3) {
    if (key==SDLK_l || (x>=40 && x<580 && y>=180 && y<216)) { f->calibration.lever=!f->calibration.lever;f->cal_bits=0; }
    if (key==SDLK_i || (x>=620 && x<1150 && y>=180 && y<216)) { f->calibration.invert=!f->calibration.invert;f->cal_flip=!f->cal_flip; }
    for (int p=0;p<3;++p) if (key==SDLK_1+p || (x>=40+p*380 && x<400+p*380 && y>=300 && y<336)) { if (p!=1 || !f->calibration.lever) sample_point(s,p); }
    if (key==SDLK_LEFT || key==SDLK_RIGHT || (x>=40 && x<580 && y>=380 && y<416)) {
      int v=f->calibration.deadzone+((key==SDLK_LEFT || (x>=40 && x<300))?-500:500);f->calibration.deadzone=v<0?0:v>30000?30000:v;
    }
    if (key==SDLK_RETURN || (x>=40 && x<580 && y>=630 && y<666)) confirm_calibration(s);
    if (x>=620 && x<1150 && y>=630 && y<666) cancel_step(s,"Calibration cancelled; previous values retained.");
    return;
  }
  if (f->page==2) {
    int preset=f->proposal.behavior==USB_PRESET;
    int threshold=f->proposal.axis && f->proposal.behavior!=USB_DIRECT && f->proposal.behavior!=USB_ANALOG;
    if (key==SDLK_n || (x>=620 && x<1150 && y>=570 && y<606)) { f->page=0;capture(s);return; }
    if (key==SDLK_b || key==SDLK_LEFT || key==SDLK_RIGHT || (x>=40 && x<700 && y>=190 && y<226)) choose_behavior(s,key==SDLK_LEFT?-1:1);
    if (f->proposal.axis && (key==SDLK_i || (x>=740 && x<1150 && y>=190 && y<226))) f->proposal.direction=-f->proposal.direction;
    if (preset && (key==SDLK_UP || key==SDLK_DOWN || (x>=40 && x<580 && y>=250 && y<286))) {
      int v=f->proposal.percent+((key==SDLK_DOWN || (x>=40 && x<300))?-1:1);f->proposal.percent=v<0?0:v>100?100:v;
    }
    if (threshold && (key==SDLK_t || key==SDLK_g || (x>=620 && x<1150 && y>=250 && y<286))) {
      int v=f->proposal.threshold+((key==SDLK_g || (x>=620 && x<880))?-1024:1024);f->proposal.threshold=v<0?0:v>32766?32766:v;
    }
    if (key==SDLK_r || (x>=40 && x<580 && y>=510 && y<546)) f->resolution=1;
    if (key==SDLK_m || (x>=620 && x<1150 && y>=510 && y<546)) f->resolution=2;
    if (key==SDLK_c || (x>=40 && x<580 && y>=570 && y<606)) calibrate(s);
    if (key==SDLK_RETURN || (x>=40 && x<580 && y>=630 && y<666)) confirm_binding(s);
    if (x>=620 && x<1150 && y>=630 && y<666) cancel_step(s,"Binding edit cancelled; existing bindings retained.");
    if (key==SDLK_PAGEUP) --f->collision_scroll;
    if (key==SDLK_PAGEDOWN) ++f->collision_scroll;
    if (f->collision_scroll<0) f->collision_scroll=0;
    return;
  }
  if (key==SDLK_TAB) {
    if (!f->footer_focus) { f->footer_focus=1;f->selection=0; }
    else f->selection=(f->selection+((e->key.keysym.mod & KMOD_SHIFT)?2:1))%3;
    return;
  }
  if (f->footer_focus && (key==SDLK_SPACE || key==SDLK_RETURN)) {
    s->result=f->selection==0?LHX_SETUP_SAVE:f->selection==1?LHX_SETUP_SESSION:LHX_SETUP_CANCEL;return;
  }
  if (key || x>=0 || e->type==SDL_MOUSEWHEEL) f->footer_focus=0;
  if (key==SDLK_d || (x>=900 && x<1180 && y>=20 && y<56)) {
    if (f->page==1 && f->device_cursor<s->edits.usb.devices) f->monitor_device=f->device_cursor;
    f->page=!f->page;f->reassign=-1;return;
  }
  if (f->page==1) {
    int total=s->edits.usb.devices+(f->candidate_count>0?f->candidate_count:0);
    if (key==SDLK_DOWN) ++f->device_cursor;
    if (key==SDLK_UP) --f->device_cursor;
    if (e->type==SDL_MOUSEWHEEL) f->device_cursor-=e->wheel.y;
    int start=(f->device_cursor/13)*13;
    if (x>=20 && x<1180 && y>=150 && y<150+13*32) f->device_cursor=start+(y-150)/32;
    if (f->device_cursor<0) f->device_cursor=0;
    if (total && f->device_cursor>=total) f->device_cursor=total-1;
    if (key==SDLK_r || (x>=620 && x<1150 && y>=630 && y<666)) {
      if (f->device_cursor<s->edits.usb.devices) { f->reassign=f->device_cursor;say(s,"Select the matching detected candidate, then Enter. This assigns a session occurrence only."); }
    }
    if (key==SDLK_g || (x>=800 && x<1180 && y>=108 && y<144)) {
      if (f->device_cursor<s->edits.usb.devices) {
        int d=f->device_cursor,has=0;for (int i=0;i<s->edits.usb.bindings;++i) has|=s->edits.usb.binding[i].device==d;
        if (has) say(s,"Remove this device's bindings before switching native / mapped interpretation.");
        else { s->edits.usb.device[d].mapped=!s->edits.usb.device[d].mapped;lhx_input_close(&f->live.input[d]);refresh(s); }
      }
    }
    if (key==SDLK_RETURN || (x>=40 && x<580 && y>=630 && y<666)) enable_selected(s);
  } else {
    if (key==SDLK_UP) select_action(s,s->row-1);
    if (key==SDLK_DOWN) select_action(s,s->row+1);
    if (key==SDLK_PAGEUP) select_action(s,s->row-13);
    if (key==SDLK_PAGEDOWN) select_action(s,s->row+13);
    if (e->type==SDL_MOUSEWHEEL) {
      int mouse_y=0;
#if SDL_VERSION_ATLEAST(2,26,0)
      mouse_y=e->wheel.mouseY;
#else
      SDL_GetMouseState(NULL,&mouse_y);
#endif
      if (mouse_y>=520 && mouse_y<680) move_binding(s,-e->wheel.y);else select_action(s,s->row-e->wheel.y);
    }
    if (x>=20 && x<740 && y>=140 && y<478) select_action(s,s->scroll+(y-140)/26);
    if (key==SDLK_LEFTBRACKET || key==SDLK_RIGHTBRACKET) move_binding(s,key==SDLK_LEFTBRACKET?-1:1);
    if (x>=20 && x<1180 && y>=550 && y<678) { f->binding=f->binding_scroll+(y-550)/32; }
    if (key==SDLK_a || (x>=20 && x<260 && y>=480 && y<516)) { f->editing=-1;capture(s); }
    if (key==SDLK_e || key==SDLK_RETURN || (x>=280 && x<520 && y>=480 && y<516)) {
      int n=selected(s);if (n>=0) { f->editing=n;LhxUsbBinding b=s->edits.usb.binding[n];propose(s,b.device,b.axis,b.input,1);f->proposal=b; }
    }
    if (key==SDLK_DELETE || (x>=540 && x<780 && y>=480 && y<516)) {
      int n=selected(s);if (n>=0) { remove_binding(&s->edits.usb,n);lhx_usb_reseed(&f->live);move_binding(s,0);say(s,"Only the selected binding removed from draft."); }
    }
    if (key==SDLK_c || (x>=800 && x<1180 && y>=480 && y<516)) calibrate(s);
    if (key==SDLK_m || (x>=780 && x<1180 && y>=442 && y<478)) {
      int d=f->monitor_device,n=selected(s);if (n>=0) d=s->edits.usb.binding[n].device;
      const LhxControllerInput *in=&f->live.input[d];
      int pages=(in->axes+7)/8+(in->buttons+7)/8;
      f->monitor=(f->monitor+1)%(pages+1);
    }
  }
  if (key==SDLK_F5 || (x>=20 && x<360 && y>=766 && y<812)) s->result=LHX_SETUP_SAVE;
  if (key==SDLK_F6 || (x>=380 && x<920 && y>=766 && y<812)) s->result=LHX_SETUP_SESSION;

}
static void live_value(const LhxControllerSetup *s,SDL_Renderer *r,const LhxUsbBinding *b,int y,const LhxCalibration *override) {
  const LhxFlexibleSetup *f=&s->flex;char line[256];int connected=lhx_input_attached(&f->live.input[b->device]);
  if (!connected) { text(r,40,y,"No signal: device disconnected / disabled / needs reassignment",92);return; }
  if (b->input>=(b->axis?f->live.input[b->device].axes:f->live.input[b->device].buttons)) { text(r,40,y,"No signal: bound control is unavailable on this device",92);return; }
  int raw=b->axis?f->axes[b->device][b->input]:f->buttons[b->device][b->input];
  const LhxCalibration *c=override?override:&s->edits.usb.device[b->device].calibration[b->input];
  int lever=0;
  int value=lhx_usb_normalize(&f->live.input[b->device],c,b,raw,&lever);
  snprintf(line,sizeof line,"LIVE raw %d   calibrated %d   range %s",raw,value,lever?"0..32767":"-32767..32767");text(r,40,y,line,92);
  if (b->behavior==USB_DIRECT) snprintf(line,sizeof line,"Result: lift %d%% on meaningful movement. Stationary lever leaves presets / keyboard intact.",(value*100+16383)/32767);
  else if (b->behavior==USB_ANALOG) snprintf(line,sizeof line,"Result: signed flight input %d%%. Latest meaningful activity takes control.",value*100/32767);
  else if (b->behavior==USB_RELATIVE) snprintf(line,sizeof line,"Result: %s lift (held 10-unit steps)",value>b->threshold?"INCREASE":value<-b->threshold?"DECREASE":"NEUTRAL");
  else if (b->behavior==USB_PRESET) snprintf(line,sizeof line,"Result: on fresh press set nominal lift %d%%",b->percent);
  else snprintf(line,sizeof line,"Result: %s %s",lhx_usb_behavior_label(b->behavior),(b->axis?value>b->threshold:abs(value)>0)?"ACTIVE":"inactive");
  wrapped(r,40,y+26,line,92,2);
}
void lhx_flex_draw(const LhxControllerSetup *s,SDL_Renderer *r,const char *path) {
  const LhxFlexibleSetup *f=&s->flex;char line[400];color(r,12,19,23);SDL_RenderClear(r);
  color(r,129,224,165);text(r,20,20,"LHX / MULTI-USB CONTROLS",65);button(r,900,20,280,f->page==1?"D: ACTIONS":"D: DEVICES",f->page==1);
  if (!f->import_prompt && f->page<2 && lhx_flex_hotas_offer(s)) button(r,500,20,380,"H: HOTAS ONE + PEDALS",0);
  color(r,210,220,215);text(r,20,65,"Keyboard stays usable. Draft applies only with Save or Session. Escape cancels pending edits.",96);
  snprintf(line,sizeof line,"%s / %d devices (max16) / %d bindings (max128)",path,s->edits.usb.devices,s->edits.usb.bindings);text(r,20,90,line,96);
  if (f->import_prompt) {
    color(r,129,224,165);text(r,40,150,"OPT INTO INDEPENDENT USB BINDINGS",92);
    color(r,210,220,215);wrapped(r,40,190,"Import the selected legacy device's effective layout, or begin with an empty USB draft. Other devices remain disabled until explicitly enabled.",92,3);
    button(r,40,250,530,"I: IMPORT LEGACY SELECTED DEVICE",0);button(r,620,250,530,"E: EMPTY USB DRAFT",0);
    text(r,40,320,"Your legacy settings are kept. Apply this draft to use independent USB bindings.",92);
    text(r,40,350,"Import keeps the original lift preset. New Set lift 50% uses nominal maximum / 2.",92);
    text(r,40,400,"Escape: return to legacy setup. Cancel / Session never writes the file.",92);
  } else if (f->page==1) {
    color(r,129,224,165);text(r,20,120,"PROFILES / CANDIDATES - UP/DOWN SELECTS",60);
    int total=s->edits.usb.devices+(f->candidate_count>0?f->candidate_count:0),start=f->device_cursor/13*13;
    for (int row=0;row<13 && start+row<total;++row) {
      int i=start+row,y=150+row*32;if (i==f->device_cursor) { color(r,35,66,56);rect(r,20,y,1160,30); }
      color(r,220,231,219);
      if (i<s->edits.usb.devices) {
        const LhxUsbDevice *d=&s->edits.usb.device[i];char name[100];lhx_usb_unhex(name,sizeof name,d->name);
        snprintf(line,sizeof line,"D%d %.32s / %s / %s%s",i+1,name,statuses[f->live.status[i]],d->mapped?"mapped":"native",d->occurrence>=0?" / session occurrence":"");
      } else {
        const LhxUsbCandidate *c=&f->candidates[i-s->edits.usb.devices];
        int configured=-1;
        for (int d=0;d<s->edits.usb.devices;++d) if (lhx_usb_same_identity(&s->edits.usb.device[d],&c->identity) &&
          (s->edits.usb.device[d].occurrence<0 || s->edits.usb.device[d].occurrence==c->instance)) configured=d;
        char state[90];if (configured>=0) snprintf(state,sizeof state,"Configured D%d - Enter reuses",configured+1);
        else snprintf(state,sizeof state,"%s",*c->error?c->error:"Enter to explicitly enable");
        snprintf(line,sizeof line,"Candidate %.30s / %dA %dB %dH / %.70s",c->name,c->axes,c->buttons,c->hats,state);
      }
      text(r,28,y+8,line,95);
    }
    if (!total) { color(r,220,231,219);text(r,40,170,"No configured devices or detected candidates. Keyboard and mouse remain available.",92); }
    if (f->candidate_count<0) text(r,20,620,"More than 64 candidates: list blocked, no silent truncation. Disconnect unused devices.",96);
    if (f->device_cursor<s->edits.usb.devices && total) {
      const LhxUsbDevice *d=&s->edits.usb.device[f->device_cursor];
      snprintf(line,sizeof line,"GUID %s / serial %s",d->guid,*d->serial?d->serial:"none - identical units need session reassignment");text(r,20,590,line,96);
    } else if (f->candidate_count>0 && total) {
      const LhxUsbCandidate *c=&f->candidates[f->device_cursor-s->edits.usb.devices];
      snprintf(line,sizeof line,"GUID %s / serial %s / session occurrence %d",c->identity.guid,*c->identity.serial?c->identity.serial:"none",c->instance);text(r,20,590,line,96);
    }
    button(r,40,630,540,"ENTER: ENABLE / DISABLE SELECTED",0);button(r,620,630,530,"R: REASSIGN SELECTED PROFILE",f->reassign>=0);
    button(r,800,108,380,"G: NATIVE / MAPPED",0);
  } else if (f->page==4) {
    color(r,129,224,165);snprintf(line,sizeof line,"HOTAS ONE + TFRP PEDALS / D%d / DRAFT",f->hotas.device+1);text(r,40,140,line,92);
    color(r,220,231,219);
    if (f->hotas_page) {
      int start=(f->hotas_page-1)*8;
      for (int b=start;b<start+8;++b) wrapped(r,40,185+(b-start)*48,f->hotas.button[b],92,2);
    } else {
    for (int a=0;a<4;++a) wrapped(r,40,185+a*54,f->hotas.flight[a],92,2);
    snprintf(line,sizeof line,"Flight: %d added / %d kept or skipped. Buttons / hat: %d added / %d kept or skipped.",
      f->hotas.added,f->hotas.skipped,f->hotas.buttons_added,f->hotas.buttons_skipped);wrapped(r,40,410,line,92,2);
    text(r,40,465,"Defaults: stick roll / pitch, Axis 3 direct lift, Axis 8 pedal yaw. No measured calibration.",92);
    text(r,40,495,"Throttle: forward = more lift by default. E: edit / invert. C: calibrate your own endpoints.",92);
    text(r,40,530,"Pedals need independent 5/8 mode: GUIDE white LED OFF. GUIDE toggles the hardware mode.",92);
    text(r,40,560,"Toe brakes, twist and rocker receive no new bindings. Existing custom assignments stay.",92);
    text(r,40,590,"Defaults use buttons 1-12 + hat, skip 13-15. B: review each button result.",92);
    }
    button(r,40,630,540,"ENTER: REVIEW ACTIONS",1);
    button(r,620,630,530,"B / PGDN: PRESET RESULTS",0);
  } else if (f->page==2 || f->page==3) {
    color(r,129,224,165);snprintf(line,sizeof line,"%s / %s",f->page==3?"CALIBRATE AXIS":"BINDING BEHAVIOR / PREVIEW",lhx_usb_action_label(f->proposal.action));text(r,40,125,line,92);
    lhx_usb_physical_live(&s->edits.usb,&f->proposal,&f->live.input[f->proposal.device],line,sizeof line);color(r,220,231,219);text(r,40,155,line,92);
    if (f->page==2) {
      snprintf(line,sizeof line,"B / LEFT / RIGHT: %s",lhx_usb_behavior_label(f->proposal.behavior));button(r,40,190,660,line,1);
      if (f->proposal.axis) button(r,740,190,410,f->proposal.direction<0?"I: INVERTED":"I: NORMAL",0);
      if (f->proposal.behavior==USB_PRESET) { snprintf(line,sizeof line,"UP/DOWN: PRESET %d%%",f->proposal.percent);button(r,40,250,540,line,0); }
      if (f->proposal.axis && f->proposal.behavior!=USB_DIRECT && f->proposal.behavior!=USB_ANALOG) {
        snprintf(line,sizeof line,"G-/T+: THRESHOLD %d",f->proposal.threshold);button(r,620,250,530,line,0);
      }
      if (f->proposal.behavior==USB_DIRECT) text(r,40,260,"Movement takeover: 256 calibrated units, plus exact endpoints. C: calibrate your lever.",92);
      color(r,129,224,165);live_value(s,r,&f->proposal,310,NULL);
      snprintf(line,sizeof line,"%d existing binding(s) share this physical control. Resolution: %s",f->collisions,f->resolution==1?"intentional reuse":f->resolution==2?"move this control":"choose if collision");text(r,40,395,line,92);
      int n=0,shown=0;for (int i=0;i<s->edits.usb.bindings;++i) if (i!=f->editing && same_control(s,&f->proposal,&s->edits.usb.binding[i])) {
        if (n++<f->collision_scroll) continue;
        if (shown<3) { snprintf(line,sizeof line,"Binding %d: %s / %s",i+1,lhx_usb_action_label(s->edits.usb.binding[i].action),lhx_usb_behavior_name(s->edits.usb.binding[i].behavior));text(r,40,424+shown++*23,line,92); }
      }
      if (f->collisions>3) text(r,820,476,"PgUp/PgDn: conflicts",28);
      button(r,40,510,540,"R: ALLOW INTENTIONAL REUSE",f->resolution==1);button(r,620,510,530,"M: MOVE ONLY THIS CONTROL",f->resolution==2);
      button(r,40,570,540,"C: CALIBRATE THIS DEVICE / AXIS",0);button(r,620,570,530,"N: CHANGE PHYSICAL CONTROL",0);
      button(r,40,630,540,"ENTER: CONFIRM BINDING",1);button(r,620,630,530,"ESC: CANCEL EDIT",0);
    } else {
      button(r,40,180,540,f->calibration.lever?"L: LEVER (ENDPOINTS ONLY)":"L: CENTERED (NEUTRAL + ENDPOINTS)",0);
      button(r,620,180,530,f->calibration.invert?"I: INVERTED CALIBRATION":"I: NORMAL CALIBRATION",0);
      color(r,220,231,219);text(r,40,240,f->calibration.lever?"Capture your desired 0% and 100% ends. Reversed raw travel is handled automatically.":"Hold minimum, neutral, maximum steady; explicitly capture each. No automatic learning.",92);
      snprintf(line,sizeof line,"1: %s %d %s",f->calibration.lever?"0% END":"MIN",f->calibration.lever && (f->cal_bits&1)?f->endpoints[0]:f->calibration.minimum,f->cal_bits&1?"OK":"required");button(r,40,300,360,line,0);
      snprintf(line,sizeof line,"2: ZERO %d %s",f->calibration.center,f->calibration.lever?"unused":f->cal_bits&2?"OK":"required");button(r,420,300,360,line,0);
      snprintf(line,sizeof line,"3: %s %d %s",f->calibration.lever?"100% END":"MAX",f->calibration.lever && (f->cal_bits&4)?f->endpoints[1]:f->calibration.maximum,f->cal_bits&4?"OK":"required");button(r,800,300,360,line,0);
      snprintf(line,sizeof line,"LEFT/RIGHT: DEADZONE %d",f->calibration.deadzone);button(r,40,380,540,line,0);
      color(r,129,224,165);live_value(s,r,&f->proposal,435,&f->calibration);
      color(r,210,220,215);text(r,40,530,"Stable: >=12 samples / 0.2s / <=256 spread. Span >=4096. Centered sides >=2048.",92);
      text(r,40,560,"Shared by this device / axis only. Existing calibration stays until validation succeeds.",92);
      button(r,40,630,540,"ENTER: VALIDATE CALIBRATION",1);button(r,620,630,530,"ESC: KEEP PREVIOUS CALIBRATION",0);
    }
  } else {
    color(r,129,224,165);text(r,20,115,"ACTION - ARROWS / PGUP / PGDN",58);text(r,780,115,"LIVE INPUT / M: NEXT PAGE",33);
    for (int row=0;row<13 && s->scroll+row<USB_ACTIONS;++row) {
      int a=s->scroll+row,y=140+row*26,n=0;for (int i=0;i<s->edits.usb.bindings;++i) n+=s->edits.usb.binding[i].action==a;
      if (s->row==a) { color(r,35,66,56);rect(r,20,y,720,25); }
      color(r,220,231,219);text(r,28,y+5,lhx_usb_action_label(a),46);snprintf(line,sizeof line,"%d",n);text(r,650,y+5,line,6);
    }
    if (f->monitor==0) {
      color(r,210,220,215);
      if (!f->signals) wrapped(r,780,150,"No signal yet. Use an enabled device. D lists connected / disabled / missing devices.",33,4);
      for (int i=0;i<f->signals;++i) wrapped(r,780,150+i*76,f->signal[i],33,3);
    } else {
      int n=selected(s),d=f->monitor_device;
      if (n>=0) d=s->edits.usb.binding[n].device;
      if (d>=s->edits.usb.devices || !s->edits.usb.device[d].enabled) {
        d=-1;for (int i=0;i<s->edits.usb.devices;++i) if (s->edits.usb.device[i].enabled) { d=i;break; }
      }
      color(r,210,220,215);
      if (d>=0) {
        const LhxControllerInput *in=&f->live.input[d];
        snprintf(line,sizeof line,"D%d %s",d+1,statuses[f->live.status[d]]);text(r,780,150,line,33);
        int axis_pages=(in->axes+7)/8,page=f->monitor-1,axis=page<axis_pages;
        int start=(axis?page:page-axis_pages)*8,count=axis?in->axes:in->buttons;
        for (int k=start;k<count && k<start+8;++k) {
          if (axis) {
            lhx_input_axis_label(in,k,line,sizeof line);text(r,780,180+(k-start)*32,line,33);
            snprintf(line,sizeof line,"raw %d",f->axes[d][k]);text(r,792,196+(k-start)*32,line,32);continue;
          }
          else if (k<64 || !in->raw) snprintf(line,sizeof line,"Button %d %s",k+1,f->buttons[d][k]?"DOWN":"UP");
          else { const char *dirs[]={"up","right","down","left"};snprintf(line,sizeof line,"Hat %d %s %s",(k-64)/4+1,dirs[(k-64)%4],f->buttons[d][k]?"DOWN":"UP"); }
          text(r,780,180+(k-start)*25,line,33);
        }
      } else wrapped(r,780,150,"No enabled device. D: enable one to preview its controls before binding.",33,4);
    }
    button(r,780,442,400,"M: LIVE / EVENTS PAGE",0);
    button(r,20,480,240,"A: ADD BINDING",0);button(r,280,480,240,"E: EDIT SELECTED",0);button(r,540,480,240,"DEL: REMOVE ONE",0);button(r,800,480,380,"C: CALIBRATE SELECTED",0);
    color(r,129,224,165);snprintf(line,sizeof line,"BINDINGS / %s / [ ] OR WHEEL TO SELECT",lhx_usb_action_label(s->row));text(r,20,525,line,96);
    int list[128],n=action_bindings(s,list);
    for (int row=0;row<4 && f->binding_scroll+row<n;++row) {
      int index=f->binding_scroll+row;const LhxUsbBinding *b=&s->edits.usb.binding[list[index]];
      if (index==f->binding) { color(r,35,66,56);rect(r,20,550+row*32,1160,30); }
      char physical[170];lhx_usb_physical_live(&s->edits.usb,b,&f->live.input[b->device],physical,sizeof physical);
      snprintf(line,sizeof line,"%d/%d %s",index+1,n,physical);
      color(r,220,231,219);text(r,28,551+row*32,line,95);
      snprintf(line,sizeof line,"%s / %s%s",lhx_usb_behavior_label(b->behavior),b->direction<0?"inverted / ":"",statuses[f->live.status[b->device]]);
      color(r,129,224,165);text(r,40,567+row*32,line,94);
    }
    if (!n) { color(r,210,220,215);text(r,28,560,"Unbound. Add one binding; every other action and device keeps its own bindings.",94); }
    if (s->capture) { color(r,48,59,37);rect(r,20,550,1160,132);color(r,255,213,109);text(r,32,570,"LISTENING: PRESS / MOVE ON AN ENABLED DEVICE",94);text(r,32,604,"Held controls must be released first. Stale / disabled input is ignored.",94);button(r,900,638,280,"CANCEL BIND",1); }
  }
  color(r,255,213,109);wrapped(r,20,690,s->status,96,3);
  if (!s->capture && (f->page<2 || f->page==4) && !f->import_prompt) { color(r,170,190,183);text(r,20,745,"F5 Save / F6 Session. Tab then Space also selects a footer action. Controllers never accept.",96); }
  button(r,20,766,340,"F5: SAVE AND START",f->footer_focus && f->selection==0);button(r,380,766,540,"F6: START WITHOUT SAVING",f->footer_focus && f->selection==1);button(r,940,766,240,"CANCEL",f->footer_focus && f->selection==2);
  color(r,170,190,183);text(r,20,820,"GUID + serial identity. Ambiguous units: explicit session reassignment; replug needs review.",96);
}
