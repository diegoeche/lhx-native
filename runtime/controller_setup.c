#include "controller_setup.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { LIST_Y=156, ROW_H=26, VISIBLE=13, LIST_W=790 };
typedef struct { const char *name; LhxControlAction button; LhxAxisAction axis; } Action;
static const Action actions[LHX_SETUP_ACTIONS]={
  {"Roll",LHX_NONE,LHX_ROLL}, {"Pitch",LHX_NONE,LHX_PITCH},
  {"Rudder",LHX_NONE,LHX_RUDDER}, {"Lift +/-10-unit steps",LHX_NONE,LHX_COLLECTIVE},
  {"Increase lift",LHX_INCREASE,LHX_AXIS_NONE},
  {"Decrease lift",LHX_DECREASE,LHX_AXIS_NONE},
  {"Maximum lift 100%",LHX_FULL,LHX_AXIS_NONE}, {"Original lift preset",LHX_HOVER,LHX_AXIS_HOVER},
  {"Landing gear",LHX_GEAR,LHX_AXIS_NONE}, {"Fire",LHX_FIRE,LHX_AXIS_FIRE},
  {"Previous weapon",LHX_PREVIOUS,LHX_AXIS_NONE}, {"Next weapon",LHX_NEXT,LHX_AXIS_NONE},
  {"Chaff",LHX_CHAFF,LHX_AXIS_CHAFF}, {"Flares",LHX_FLARES,LHX_AXIS_FLARES},
  {"Cockpit view",LHX_COCKPIT,LHX_AXIS_NONE}, {"Chase view",LHX_CHASE,LHX_AXIS_NONE},
  {"Toggle view",LHX_VIEW,LHX_AXIS_NONE}, {"Pause",LHX_PAUSE,LHX_AXIS_NONE},
  {"Confirm / next target",LHX_CONFIRM,LHX_AXIS_NONE}, {"Back / Escape",LHX_CANCEL,LHX_AXIS_NONE},
  {"Menu up",LHX_UP,LHX_AXIS_NONE}, {"Menu down",LHX_DOWN,LHX_AXIS_NONE},
  {"Menu left",LHX_LEFT,LHX_AXIS_NONE}, {"Menu right",LHX_RIGHT,LHX_AXIS_NONE}
};
/* Small built-in 5x7 code font. Columns, low bit at the top. */
static const struct { char ch; Uint8 col[5]; } font[]={
  {'A',{126,17,17,17,126}},{'B',{127,73,73,73,54}},{'C',{62,65,65,65,34}},
  {'D',{127,65,65,34,28}},{'E',{127,73,73,73,65}},{'F',{127,9,9,9,1}},
  {'G',{62,65,73,73,122}},{'H',{127,8,8,8,127}},{'I',{0,65,127,65,0}},
  {'J',{32,64,65,63,1}},{'K',{127,8,20,34,65}},{'L',{127,64,64,64,64}},
  {'M',{127,2,12,2,127}},{'N',{127,4,8,16,127}},{'O',{62,65,65,65,62}},
  {'P',{127,9,9,9,6}},{'Q',{62,65,81,33,94}},{'R',{127,9,25,41,70}},
  {'S',{70,73,73,73,49}},{'T',{1,1,127,1,1}},{'U',{63,64,64,64,63}},
  {'V',{31,32,64,32,31}},{'W',{63,64,56,64,63}},{'X',{99,20,8,20,99}},
  {'Y',{7,8,112,8,7}},{'Z',{97,81,73,69,67}},
  {'0',{62,81,73,69,62}},{'1',{0,66,127,64,0}},{'2',{98,81,73,73,70}},
  {'3',{34,65,73,73,54}},{'4',{24,20,18,127,16}},{'5',{39,69,69,69,57}},
  {'6',{60,74,73,73,48}},{'7',{1,113,9,5,3}},{'8',{54,73,73,73,54}},
  {'9',{6,73,73,41,30}},{'-',{8,8,8,8,8}},{'+',{8,8,62,8,8}},
  {'/',{32,16,8,4,2}},{':',{0,54,54,0,0}},{'.',{0,96,96,0,0}},
  {',',{0,64,32,0,0}},{'(',{0,28,34,65,0}},{')',{0,65,34,28,0}},
  {'[',{0,127,65,65,0}},{']',{0,65,65,127,0}},{'=',{20,20,20,20,20}},
  {'<',{8,20,34,65,0}},{'>',{0,65,34,20,8}},{'%',{99,19,8,100,99}},
  {'_',{64,64,64,64,64}},{'?',{2,1,81,9,6}},{'!',{0,0,95,0,0}},
  {'#',{20,127,20,127,20}},{'|',{0,0,127,0,0}}
};
static void color(SDL_Renderer *r, int red,int green,int blue) {
  SDL_SetRenderDrawColor(r,(Uint8)red,(Uint8)green,(Uint8)blue,255);
}
static void rect(SDL_Renderer *r,int x,int y,int w,int h) {
  SDL_Rect box={x,y,w,h}; SDL_RenderFillRect(r,&box);
}
static void text(SDL_Renderer *r,int x,int y,const char *str,int limit) {
  for (int n=0;str[n] && n<limit;++n,x+=12) {
    unsigned char ch=(unsigned char)str[n]; if (ch>='a' && ch<='z') ch-=32;
    for (unsigned i=0;i<sizeof font/sizeof *font;++i) if (font[i].ch==ch) {
      for (int col=0;col<5;++col) for (int bit=0;bit<7;++bit)
        if (font[i].col[col] & (1<<bit)) rect(r,x+col*2,y+bit*2,2,2);
      break;
    }
  }
}

static void wrapped(SDL_Renderer *r,int x,int y,const char *str,int width,int lines) {
  while (*str && lines--) {
    int n=(int)strlen(str);
    if (n>width) {
      n=width;
      while (n>0 && str[n]!=' ' && str[n]!=',') --n;
      if (!n) n=width;
    }
    char line[128]; snprintf(line,sizeof line,"%.*s",n,str); text(r,x,y,line,width);
    str+=n; while (*str==' ') ++str; y+=20;
  }
}
static void append(char *out,size_t size,const char *item) {
  size_t n=strlen(out);
  if (n<size) snprintf(out+n,size-n,"%s%s",n?", ":"",item);
}
static int button_row(LhxControlAction a) {
  if (a!=LHX_NONE) for (int i=0;i<LHX_SETUP_ACTIONS;++i) if (actions[i].button==a) return i;
  return -1;
}
static int axis_row(LhxAxisAction a) {
  if (a!=LHX_AXIS_NONE) for (int i=0;i<LHX_SETUP_ACTIONS;++i) if (actions[i].axis==a) return i;
  return -1;
}
static int axis_count(const LhxControllerSetup *s) { return s->input.raw?LHX_RAW_AXES:SDL_CONTROLLER_AXIS_MAX; }
static int button_count(const LhxControllerSetup *s) { return s->input.raw?LHX_INPUT_BUTTONS:SDL_CONTROLLER_BUTTON_MAX; }
#define BUTTONS(c) (s->input.raw?(c)->raw_buttons:(c)->buttons)
#define AXES(c) (s->input.raw?(c)->raw_axes:(c)->axes)
static SDL_GameControllerButtonBind input_bind(const LhxControllerSetup *s,int axis,int input) {
  return lhx_input_bind(&s->input,axis,input);
}
static void physical(const LhxControllerSetup *s,int axis,int input,char *out,size_t size) {
  SDL_GameControllerButtonBind b=input_bind(s,axis,input);
  if (b.bindType==SDL_CONTROLLER_BINDTYPE_BUTTON) snprintf(out,size,"Button %d",b.value.button+1);
  else if (b.bindType==SDL_CONTROLLER_BINDTYPE_AXIS) lhx_input_axis_label(&s->input,b.value.axis,out,size);
  else if (b.bindType==SDL_CONTROLLER_BINDTYPE_HAT) {
    const char *dir=b.value.hat.hat_mask==SDL_HAT_UP?"up":b.value.hat.hat_mask==SDL_HAT_DOWN?"down":
      b.value.hat.hat_mask==SDL_HAT_LEFT?"left":"right";
    snprintf(out,size,"Hat %d %s",b.value.hat.hat+1,dir);
  } else snprintf(out,size,"SDL %s",axis?SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)input):
      SDL_GameControllerGetStringForButton((SDL_GameControllerButton)input));
}
static void bindings(const LhxControllerSetup *s,const LhxControls *c,int row,char *out,size_t size) {
  *out=0;
  if (row<0 || row>=LHX_SETUP_ACTIONS) return;
  char item[64],input[48];
  if (actions[row].button) for (int i=0;i<button_count(s);++i)
    if (BUTTONS(c)[i]==actions[row].button) { physical(s,0,i,item,sizeof item); append(out,size,item); }
  if (actions[row].axis) for (int i=0;i<axis_count(s);++i)
    if (AXES(c)[i].action==actions[row].axis) {
      physical(s,1,i,input,sizeof input);
      snprintf(item,sizeof item,"%s %s",input,AXES(c)[i].direction<0?"- (inverted)":"+"); append(out,size,item);
    }
  if (!*out) snprintf(out,size,"Unbound");
}
void lhx_controller_setup_bindings(const LhxControllerSetup *s,int row,char *out,size_t size) {
  if (s->flex.enabled) { lhx_flex_bindings(s,row,out,size);return; }
  if (size) bindings(s,&s->edits,row,out,size);
}
static void reveal(LhxControllerSetup *s) {
  if (s->row<LHX_SETUP_ACTIONS) {
    if (s->row<s->scroll) s->scroll=s->row;
    if (s->row>=s->scroll+VISIBLE) s->scroll=s->row-VISIBLE+1;
  }
}
static void cancel_capture(LhxControllerSetup *s,const char *reason) {
  s->capture=s->conflict=0;
  snprintf(s->status,sizeof s->status,"%s",reason);
}
static void close_pad(LhxControllerSetup *s) {
  lhx_input_close(&s->input);
  s->input.raw=s->edits.input_mode==LHX_INPUT_JOYSTICK; s->instance=-1; s->capture=s->conflict=0;
  memset(s->active,0,sizeof s->active);
  memset(s->highlight,0,sizeof s->highlight);
  memset(s->buttons,0,sizeof s->buttons); memset(s->motion,0,sizeof s->motion);
}
void lhx_controller_setup_close(LhxControllerSetup *s) { close_pad(s);lhx_flex_close(s); }
static void seed_signals(LhxControllerSetup *s) {
  SDL_JoystickUpdate();
  memset(s->values,0,sizeof s->values);
  memset(s->previous,0,sizeof s->previous);
  for (int i=0;i<s->input.axes;++i)
    s->values[i]=s->axis_reported[i]=lhx_input_axis(&s->input,i);
  for (int i=0;i<s->input.buttons;++i)
    s->previous[i]=lhx_input_button(&s->input,i);
}
static void open_selected(LhxControllerSetup *s) {
  if (s->input.joy) return;
  if (lhx_input_open_selected(&s->input,&s->edits)) {
    s->instance=SDL_JoystickInstanceID(s->input.joy);
    /* Pin raw identity and interpretation before saving, even in auto mode. */
    if (s->input.raw && s->edits.input_mode==LHX_INPUT_AUTO) lhx_input_select(&s->input,&s->edits);
    seed_signals(s);
    s->monitor_page=s->input.raw?1:0;
    snprintf(s->status,sizeof s->status,"Controller connected and ready. Select an action, then Enter / Bind.");
  }
}
void lhx_controller_setup_init(LhxControllerSetup *s,const LhxControls *config) {
  memset(s,0,sizeof *s); s->edits=*config; s->instance=-1; s->focused=1;
  s->input.raw=config->input_mode==LHX_INPUT_JOYSTICK;
  snprintf(s->status,sizeof s->status,"Select an action, then Enter / Bind. Connect a supported SDL controller to bind.");
  lhx_flex_init(s);
  if (!s->flex.enabled) open_selected(s);
}
static void choose_device(LhxControllerSetup *s,int direction) {
  int current=-1, next=-1;
  if (*s->edits.controller_guid && s->input.joy) for (int i=0;i<SDL_NumJoysticks();++i)
    if (SDL_JoystickGetDeviceInstanceID(i)==s->instance) current=i;
  if (direction>0) {
    for (int i=current+1;i<SDL_NumJoysticks();++i) if (lhx_input_eligible(i)) { next=i; break; }
  } else {
    for (int i=current<0?SDL_NumJoysticks()-1:current-1;i>=0;--i)
      if (lhx_input_eligible(i)) { next=i; break; }
  }
  close_pad(s); lhx_controls_select_joystick(&s->edits,NULL); s->edits.input_mode=LHX_INPUT_AUTO;
  if (next>=0) {
    if (lhx_input_open(&s->input,next,LHX_INPUT_AUTO)) {
      s->instance=SDL_JoystickInstanceID(s->input.joy); seed_signals(s);
      lhx_input_select(&s->input,&s->edits); s->monitor_page=s->input.raw?1:0;
      snprintf(s->status,sizeof s->status,"Controller selected. S returns to actions.");
    } else snprintf(s->status,sizeof s->status,"Cannot open selected controller: %s",SDL_GetError());
  } else open_selected(s);
}
static void identify(LhxControllerSetup *s) {
  if (!s->input.joy || !lhx_input_attached(&s->input)) {
    snprintf(s->status,sizeof s->status,"Connect and select a supported SDL controller first (S: device / settings)."); return;
  }
  if (!s->focused) return;
  seed_signals(s);
  for (int i=0;i<axis_count(s);++i) {
    s->baseline[i]=s->values[i];
    s->trigger_ready[i]=lhx_input_unipolar(&s->input,i)?s->values[i]<(int)s->edits.trigger_threshold:
      actions[s->row].axis<LHX_AXIS_FIRE || abs(s->values[i])<(int)s->edits.trigger_threshold;
  }
  for (int i=0;i<LHX_INPUT_BUTTONS;++i) s->ready[i]=!s->previous[i];
  s->capture=1; s->armed_at=SDL_GetTicks();
  snprintf(s->status,sizeof s->status,!actions[s->row].button?
    "Move one axis. I inverts after binding. Escape or Cancel Bind cancels.":actions[s->row].axis?
    "Press a button or trigger. Release held inputs first. Escape or Cancel Bind cancels.":
    "Press a button or hat direction. Escape or Cancel Bind cancels.");
}
static int same_input(SDL_GameControllerButtonBind a,SDL_GameControllerButtonBind b) {
  if (a.bindType==SDL_CONTROLLER_BINDTYPE_NONE || a.bindType!=b.bindType) return 0;
  if (a.bindType==SDL_CONTROLLER_BINDTYPE_BUTTON) return a.value.button==b.value.button;
  if (a.bindType==SDL_CONTROLLER_BINDTYPE_AXIS) return a.value.axis==b.value.axis;
  return a.value.hat.hat==b.value.hat.hat && (a.value.hat.hat_mask & b.value.hat.hat_mask);
}
static void clear_action(const LhxControllerSetup *s,LhxControls *c,int row) {
  for (int i=0;i<button_count(s);++i)
    if (actions[row].button && BUTTONS(c)[i]==actions[row].button) BUTTONS(c)[i]=LHX_NONE;
  for (int i=0;i<axis_count(s);++i)
    if (actions[row].axis && AXES(c)[i].action==actions[row].axis) AXES(c)[i].action=LHX_AXIS_NONE;
}
static void commit_binding(LhxControllerSetup *s) {
  s->edits=s->pending; s->capture=s->conflict=0;
  char input[64]; physical(s,s->candidate_axis,s->candidate_input,input,sizeof input);
  snprintf(s->status,sizeof s->status,"%s bound to %s. Other bindings preserved; Save or Start applies the draft.",actions[s->row].name,input);
}
static void candidate(LhxControllerSetup *s,int axis,int input,int value) {
  const Action *a=&actions[s->row];
  if ((axis && !a->axis) || (!axis && !a->button)) {
    snprintf(s->status,sizeof s->status,axis?
      "This action needs a button / hat. Press one, or Escape to cancel.":
      "This action needs an axis. Move one, or Escape to cancel."); return;
  }
  s->candidate_axis=axis; s->candidate_input=input;
  s->candidate_direction=axis && a->axis>=LHX_AXIS_FIRE && !lhx_input_unipolar(&s->input,input) && value<0?-1:1;
  /* Moving an analog role preserves its explicit inversion. Discrete actions
   * use the moved polarity; SDL trigger channels are inherently positive-only. */
  if (axis && a->axis<=LHX_COLLECTIVE) for (int i=0;i<axis_count(s);++i)
    if (AXES(&s->edits)[i].action==a->axis) { s->candidate_direction=AXES(&s->edits)[i].direction; break; }
  s->pending=s->edits; clear_action(s,&s->pending,s->row); s->displaced=0;
  SDL_GameControllerButtonBind bind=input_bind(s,axis,input);
  for (int i=0;i<button_count(s);++i) if (same_input(bind,input_bind(s,0,i))) {
    int row=button_row(BUTTONS(&s->edits)[i]);
    if (row>=0 && row!=s->row) s->displaced|=1u<<row;
    BUTTONS(&s->pending)[i]=LHX_NONE;
  }
  for (int i=0;i<axis_count(s);++i) if (same_input(bind,input_bind(s,1,i))) {
    int row=axis_row(AXES(&s->edits)[i].action);
    if (row>=0 && row!=s->row) s->displaced|=1u<<row;
    AXES(&s->pending)[i].action=LHX_AXIS_NONE;
  }
  if (axis) AXES(&s->pending)[input]=(LhxAxisBinding){a->axis,s->candidate_direction};
  else BUTTONS(&s->pending)[input]=a->button;
  char old[1024]; bindings(s,&s->edits,s->row,old,sizeof old);
  s->capture=0;
  if (s->displaced || strcmp(old,"Unbound")) {
    s->conflict=1;
    snprintf(s->status,sizeof s->status,"Review replacement. Enter / Move-Replace confirms; Escape keeps every current binding.");
  } else commit_binding(s);
}
static void feedback(LhxControllerSetup *s,int axis,int input,int value) {
  char physical_name[48]; physical(s,axis,input,physical_name,sizeof physical_name);
  int row=axis?axis_row(AXES(&s->edits)[input].action):button_row(BUTTONS(&s->edits)[input]);
  LhxSetupSignal signal={0};
  if (axis) snprintf(signal.input,sizeof signal.input,"%s %d",physical_name,value);
  else snprintf(signal.input,sizeof signal.input,"%s %s",physical_name,value?"DOWN":"UP");
  snprintf(signal.action,sizeof signal.action,"%s",row>=0?actions[row].name:"Unbound");
  if (row>=0) s->highlight[row]=SDL_GetTicks()+650;
  if (axis) { s->motion[1]=s->motion[0]; s->motion[0]=signal; }
  else { s->buttons[2]=s->buttons[1]; s->buttons[1]=s->buttons[0]; s->buttons[0]=signal; }
}
static void observe_button(LhxControllerSetup *s,int input,int value) {
  if (input<0 || input>=button_count(s)) return;
  int changed=s->previous[input]!=value; s->previous[input]=(Uint8)value;
  if (changed) feedback(s,0,input,value);
  if (!value) s->ready[input]=1;
  if (s->focused && s->capture && value && changed && s->ready[input]) candidate(s,0,input,value);
}
static void observe_axis(LhxControllerSetup *s,int input,int value) {
  if (input<0 || input>=s->input.axes) return;
  s->values[input]=value;
  Uint32 now=SDL_GetTicks();
  if ((abs(value-s->axis_reported[input])>=4096 && (!s->axis_time[input] || now-s->axis_time[input]>=100)) ||
      (abs(value)<1000 && abs(s->axis_reported[input])>=4096)) {
    feedback(s,1,input,value); s->axis_time[input]=now; s->axis_reported[input]=value;
  }
  if (!s->focused || !s->capture) return;
  if (lhx_input_unipolar(&s->input,input) && value<(int)s->edits.trigger_threshold) {
    if (!s->trigger_ready[input]) s->baseline[input]=value;
    s->trigger_ready[input]=1;
  }
  if (!lhx_input_unipolar(&s->input,input) && !s->trigger_ready[input]) {
    if (abs(value)<(int)s->edits.trigger_threshold) {
      s->baseline[input]=value; s->trigger_ready[input]=1;
    }
    return; /* Release an already held discrete axis before a fresh press. */
  }
  if (s->trigger_ready[input] && abs(value-s->baseline[input])>12000) {
    /* A release of a held trigger must never become a new press. */
    if (lhx_input_unipolar(&s->input,input) && value<=(int)s->edits.trigger_threshold) return;
    candidate(s,1,input,value);
  }
}
static void active_signals(LhxControllerSetup *s) {
  memset(s->active,0,sizeof s->active);
  for (int i=0;i<button_count(s);++i) {
    int row=button_row(BUTTONS(&s->edits)[i]); if (row>=0 && s->previous[i]) s->active[row]=1;
  }
  for (int i=0;i<axis_count(s);++i) {
    LhxAxisBinding b=AXES(&s->edits)[i]; int row=axis_row(b.action), value=s->values[i]*b.direction;
    int active=b.action>=LHX_AXIS_FIRE?value>(int)s->edits.trigger_threshold:
      abs(value)>(int)(b.action==LHX_COLLECTIVE?s->edits.axis_button_threshold:s->edits.deadzone);
    if (row>=0 && active) s->active[row]=1;
    if (b.action==LHX_COLLECTIVE) {
      s->active[LHX_SETUP_INCREASE_ROW]|=value<-(int)s->edits.axis_button_threshold;
      s->active[LHX_SETUP_DECREASE_ROW]|=value>(int)s->edits.axis_button_threshold;
    }
  }
}
void lhx_controller_setup_tick(LhxControllerSetup *s) {
  if (s->flex.enabled) { lhx_flex_tick(s);return; }
  SDL_JoystickUpdate();
  if (s->input.joy && !lhx_input_attached(&s->input)) {
    close_pad(s);
    snprintf(s->status,sizeof s->status,"Controller disconnected; capture / replacement cancelled. Keyboard and mouse remain available.");
  }
  open_selected(s);
  if (!s->input.joy) return;
  for (int i=0;i<s->input.buttons;++i)
    observe_button(s,i,lhx_input_button(&s->input,i));
  for (int i=0;i<s->input.axes;++i)
    observe_axis(s,i,lhx_input_axis(&s->input,i));
  active_signals(s);
}
static void invert(LhxControllerSetup *s) {
  int changed=0,unipolar=0;
  if (s->row>=LHX_SETUP_ACTIONS || !actions[s->row].axis) {
    snprintf(s->status,sizeof s->status,"Invert needs an axis binding. Select a flight axis or an action with a bound axis."); return;
  }
  for (int i=0;i<axis_count(s);++i) if (AXES(&s->edits)[i].action==actions[s->row].axis) {
    if (lhx_input_unipolar(&s->input,i) && actions[s->row].axis>=LHX_AXIS_FIRE) {
      AXES(&s->edits)[i].direction=1; unipolar=1;
    } else { AXES(&s->edits)[i].direction*=-1; changed=1; }
  }
  snprintf(s->status,sizeof s->status,unipolar?
    "SDL trigger channels are positive-only: keep normal press polarity. Use a stick axis for negative-side activation.":
    changed?"Selected action's axis binding(s) inverted in the draft.":"Bind an axis to this action first; then I / Invert changes polarity.");
}
static void change(LhxControllerSetup *s,int amount) {
  if (s->row==LHX_SETUP_DEVICE_ROW) choose_device(s,amount);
  else if (s->row==LHX_SETUP_STARTUP_ROW) s->edits.show_controller_setup=!s->edits.show_controller_setup;
  else if (s->row>=LHX_SETUP_DEADZONE_ROW && s->row<=LHX_SETUP_THRESHOLD_ROW) {
    unsigned *value=s->row==LHX_SETUP_DEADZONE_ROW?&s->edits.deadzone:
      s->row==LHX_SETUP_TRIGGER_ROW?&s->edits.trigger_threshold:&s->edits.axis_button_threshold;
    int n=(int)*value+amount*500, max=s->row==LHX_SETUP_DEADZONE_ROW?30000:32766;
    *value=(unsigned)(n<0?0:n>max?max:n);
  }
}
static void activate(LhxControllerSetup *s) {
  if (s->row<LHX_SETUP_ACTIONS) identify(s);
  else if (s->row==LHX_SETUP_DEFAULTS_ROW) {
    LhxControls d; lhx_controls_defaults(&d);
    memcpy(BUTTONS(&s->edits),BUTTONS(&d),button_count(s)*sizeof(LhxControlAction));
    memcpy(AXES(&s->edits),AXES(&d),axis_count(s)*sizeof(LhxAxisBinding));
    s->edits.deadzone=d.deadzone; s->edits.trigger_threshold=d.trigger_threshold;
    s->edits.axis_button_threshold=d.axis_button_threshold;
    s->edits.repeat_delay_ms=d.repeat_delay_ms; s->edits.repeat_interval_ms=d.repeat_interval_ms;
    s->edits.throttle_repeat_ms=d.throttle_repeat_ms; s->edits.fire_repeat_ms=d.fire_repeat_ms;
    snprintf(s->status,sizeof s->status,"Controller defaults restored in this draft. S returns to actions. Save or Start applies them.");
  } else if (s->row>=LHX_SETUP_ROWS)
    s->result=s->row==LHX_SETUP_ROWS?LHX_SETUP_SAVE:s->row==LHX_SETUP_ROWS+1?LHX_SETUP_SESSION:LHX_SETUP_CANCEL;
  else change(s,1);
}
static void navigate(LhxControllerSetup *s,int direction) {
  int first=s->settings?LHX_SETUP_ACTIONS:0, end=s->settings?LHX_SETUP_ROWS:LHX_SETUP_ACTIONS;
  if (s->row>=LHX_SETUP_ROWS) {
    s->row+=direction;
    if (s->row<LHX_SETUP_ROWS) s->row=end-1;
    if (s->row>LHX_SETUP_ROWS+2) s->row=first;
  } else {
    s->row+=direction;
    if (s->row<first) s->row=LHX_SETUP_ROWS+2;
    else if (s->row>=end) s->row=LHX_SETUP_ROWS;
  }
  reveal(s);
}
void lhx_controller_setup_event(LhxControllerSetup *s,const SDL_Event *event) {
  if (s->focused && !s->result && !s->capture && !s->conflict &&
      (!s->flex.enabled || (s->flex.page<2 && !s->flex.import_prompt)) &&
      ((event->type==SDL_KEYDOWN && !event->key.repeat && event->key.keysym.sym==SDLK_h) ||
       (event->type==SDL_MOUSEBUTTONDOWN && event->button.button==SDL_BUTTON_LEFT &&
        event->button.x>=500 && event->button.x<880 && event->button.y>=20 && event->button.y<56 && lhx_flex_hotas_offer(s)))) {
    lhx_flex_hotas_preset(s);return;
  }
  if (!s->flex.enabled && s->focused && !s->capture && !s->conflict && ((event->type==SDL_KEYDOWN && event->key.keysym.sym==SDLK_F2) ||
      (event->type==SDL_MOUSEBUTTONDOWN && event->button.button==SDL_BUTTON_LEFT && event->button.y>=20 && event->button.y<56 &&
       ((event->button.x>=310 && event->button.x<490) ||
        (event->button.x>=500 && event->button.x<880 && !lhx_flex_hotas_offer(s)))))) {
    s->capture=s->conflict=0;s->flex.enabled=1;s->flex.import_prompt=1;
    snprintf(s->status,sizeof s->status,"Choose import or empty USB draft. Cancel leaves the original profile unchanged.");return;
  }
  if (s->flex.enabled) { lhx_flex_event(s,event);return; }
  if (event->type==SDL_QUIT || (event->type==SDL_WINDOWEVENT && event->window.event==SDL_WINDOWEVENT_CLOSE)) {
    s->capture=s->conflict=0; s->result=LHX_SETUP_CANCEL; return;
  }
  if (event->type==SDL_WINDOWEVENT && event->window.event==SDL_WINDOWEVENT_FOCUS_LOST) {
    s->focused=0;
    if (s->capture || s->conflict) cancel_capture(s,"Capture / replacement cancelled when window lost focus. No bindings changed.");
    return;
  }
  if (event->type==SDL_WINDOWEVENT && event->window.event==SDL_WINDOWEVENT_FOCUS_GAINED) { s->focused=1; return; }
  /* Events never navigate, save or start. Preserve quick down/up pairs between
   * polling frames; tick polling also supports steady live values and hotplug. */
  if (s->input.joy && lhx_input_attached(&s->input)) {
    if (!s->input.raw && (event->type==SDL_CONTROLLERBUTTONDOWN || event->type==SDL_CONTROLLERBUTTONUP) && event->cbutton.which==s->instance) {
      if (s->capture && event->common.timestamp && (Sint32)(event->common.timestamp-s->armed_at)<0) return;
      observe_button(s,event->cbutton.button,event->type==SDL_CONTROLLERBUTTONDOWN); active_signals(s); return;
    }
    if (!s->input.raw && event->type==SDL_CONTROLLERAXISMOTION && event->caxis.which==s->instance) {
      if (s->capture && event->common.timestamp && (Sint32)(event->common.timestamp-s->armed_at)<0) return;
      observe_axis(s,event->caxis.axis,event->caxis.value); active_signals(s); return;
    }
    if (s->input.raw && event->jaxis.which==s->instance &&
        (event->type==SDL_JOYAXISMOTION || event->type==SDL_JOYBUTTONDOWN ||
         event->type==SDL_JOYBUTTONUP || event->type==SDL_JOYHATMOTION)) {
      if (s->capture && event->common.timestamp && (Sint32)(event->common.timestamp-s->armed_at)<0) return;
      if (event->type==SDL_JOYAXISMOTION) observe_axis(s,event->jaxis.axis,event->jaxis.value);
      else if (event->type==SDL_JOYHATMOTION) {
        if (event->jhat.hat<s->input.hats) for (int d=0;d<4;++d)
          observe_button(s,LHX_RAW_BUTTONS+4*event->jhat.hat+d,(event->jhat.value & (1<<d))!=0);
      } else observe_button(s,event->jbutton.button,event->type==SDL_JOYBUTTONDOWN);
      active_signals(s); return;
    }
  }
  if (event->type==SDL_KEYDOWN && event->key.keysym.sym==SDLK_ESCAPE) {
    if (s->capture || s->conflict) cancel_capture(s,"Binding cancelled. Every current binding is unchanged.");
    else s->result=LHX_SETUP_CANCEL;
    return;
  }
  if (event->type==SDL_MOUSEBUTTONDOWN && event->button.button==SDL_BUTTON_LEFT) {
    int x=event->button.x,y=event->button.y;
    if (x>=940 && x<1180 && y>=766 && y<812) {
      s->capture=s->conflict=0; s->result=LHX_SETUP_CANCEL; return;
    }
    if (s->capture && x>=900 && x<1180 && y>=642 && y<678) {
      cancel_capture(s,"Binding cancelled. Every current binding is unchanged."); return;
    }
  }
  if (s->conflict) {
    if (!s->input.joy || !lhx_input_attached(&s->input)) { cancel_capture(s,"Controller disconnected; replacement cancelled."); return; }
    if (event->type==SDL_KEYDOWN && event->key.keysym.sym==SDLK_RETURN && !event->key.repeat) commit_binding(s);
    else if (event->type==SDL_MOUSEBUTTONDOWN && event->button.button==SDL_BUTTON_LEFT && event->button.y>=628 && event->button.y<674) {
      if (event->button.x>=100 && event->button.x<570) commit_binding(s);
      else if (event->button.x>=620 && event->button.x<1100) cancel_capture(s,"Replacement cancelled. Every current binding is unchanged.");
    }
    return;
  }
  if (s->capture || s->result) return;
  if (event->type==SDL_KEYDOWN) {
    SDL_Keycode key=event->key.keysym.sym;
    if (key==SDLK_m && !event->key.repeat) {
      int nb=s->input.joy?(s->input.raw?SDL_JoystickNumButtons(s->input.joy)+4*s->input.hats:s->input.buttons):0;
      int pages=1+(s->input.axes+7)/8+(nb+7)/8;
      s->monitor_page=(s->monitor_page+1)%pages;
    } else if (key==SDLK_s && !event->key.repeat) { s->settings=!s->settings; s->row=s->settings?LHX_SETUP_DEVICE_ROW:0; }
    else if (key==SDLK_DOWN || key==SDLK_TAB) navigate(s,key==SDLK_TAB && (event->key.keysym.mod & KMOD_SHIFT)?-1:1);
    else if (key==SDLK_UP) navigate(s,-1);
    else if ((key==SDLK_PAGEUP || key==SDLK_PAGEDOWN) && !s->settings) {
      s->row+=key==SDLK_PAGEUP?-VISIBLE:VISIBLE;
      if (s->row<0) s->row=0;
      if (s->row>=LHX_SETUP_ACTIONS) s->row=LHX_SETUP_ACTIONS-1;
    } else if (key==SDLK_LEFT || key==SDLK_RIGHT) {
      if (s->row>=LHX_SETUP_ROWS) s->row=LHX_SETUP_ROWS+(s->row-LHX_SETUP_ROWS+(key==SDLK_LEFT?2:1))%3;
      else change(s,key==SDLK_LEFT?-1:1);
    } else if ((key==SDLK_RETURN || key==SDLK_SPACE || key==SDLK_b) && !event->key.repeat) activate(s);
    else if (key==SDLK_i && !event->key.repeat) invert(s);
    else if ((key==SDLK_DELETE || key==SDLK_BACKSPACE) && s->row<LHX_SETUP_ACTIONS) {
      clear_action(s,&s->edits,s->row); snprintf(s->status,sizeof s->status,"Selected action unbound in this draft. Other actions preserved.");
    }
    reveal(s);
  } else if (event->type==SDL_MOUSEWHEEL && !s->settings) {
    s->scroll-=event->wheel.y*3;
    if (s->scroll<0) s->scroll=0;
    if (s->scroll>LHX_SETUP_ACTIONS-VISIBLE) s->scroll=LHX_SETUP_ACTIONS-VISIBLE;
  } else if (event->type==SDL_MOUSEBUTTONDOWN && event->button.button==SDL_BUTTON_LEFT) {
    int x=event->button.x,y=event->button.y;
    if (x>=900 && x<1180 && y>=20 && y<56) {
      s->settings=!s->settings; s->row=s->settings?LHX_SETUP_DEVICE_ROW:0;
    } else if (x>=20 && x<LIST_W && y>=LIST_Y && y<LIST_Y+VISIBLE*ROW_H) {
      int row=s->settings?LHX_SETUP_ACTIONS+(y-LIST_Y)/ROW_H:s->scroll+(y-LIST_Y)/ROW_H;
      if (row>=(s->settings?LHX_SETUP_ROWS:LHX_SETUP_ACTIONS)) return;
      s->row=row;
      if (s->settings) { if (row==LHX_SETUP_DEFAULTS_ROW) activate(s); else change(s,x<400?-1:1); }
      else if (x>=360 || event->button.clicks>=2) identify(s);
    } else if (!s->settings && y>=508 && y<544 && s->row<LHX_SETUP_ACTIONS) {
      if (x>=20 && x<230) identify(s);
      else if (x>=246 && x<456) { clear_action(s,&s->edits,s->row); snprintf(s->status,sizeof s->status,"Selected action unbound in this draft."); }
      else if (x>=472 && x<790) invert(s);
    } else if (y>=766 && y<812) {
      if (x>=20 && x<360) s->row=LHX_SETUP_ROWS;
      else if (x>=380 && x<920) s->row=LHX_SETUP_ROWS+1;
      else if (x>=940 && x<1180) s->row=LHX_SETUP_ROWS+2;
      else return;
      activate(s);
    }
  }
}
static void ui_button(SDL_Renderer *r,int x,int y,int w,const char *label,int selected) {
  color(r,selected?56:30,selected?104:51,selected?77:49); rect(r,x,y,w,36);
  color(r,224,235,223); text(r,x+12,y+11,label,(w-20)/12);
}
static void setting_label(const LhxControllerSetup *s,int row,char *out,size_t size) {
  if (row==LHX_SETUP_DEVICE_ROW) snprintf(out,size,"Controller: %s  < >",*s->edits.controller_guid?"Selected device":"Automatic / first");
  else if (row==LHX_SETUP_STARTUP_ROW) snprintf(out,size,"Show setup on startup: %s",s->edits.show_controller_setup?"YES":"NO");
  else if (row==LHX_SETUP_DEADZONE_ROW) snprintf(out,size,"Center deadzone: %u  < >",s->edits.deadzone);
  else if (row==LHX_SETUP_TRIGGER_ROW) snprintf(out,size,"Trigger threshold: %u  < >",s->edits.trigger_threshold);
  else if (row==LHX_SETUP_THRESHOLD_ROW) snprintf(out,size,"Collective +/- threshold: %u  < >",s->edits.axis_button_threshold);
  else snprintf(out,size,"Restore controller defaults (draft)");
}
static void monitor(const LhxControllerSetup *s,SDL_Renderer *r) {
  color(r,22,33,38); rect(r,810,LIST_Y,370,388);
  color(r,129,224,165); text(r,824,166,s->focused?"LIVE / CHOSEN DEVICE":"UNFOCUSED / CAPTURE OFF",28);
  if (!s->input.joy) {
    color(r,220,231,219); wrapped(r,824,202,"No signal: selected device unavailable. Connect an eligible controller.",28,4); return;
  }
  char line[128];
  snprintf(line,sizeof line,"%s: %dA %dB %dH",s->input.raw?"RAW":"MAPPED",s->input.axes,
    SDL_JoystickNumButtons(s->input.joy),SDL_JoystickNumHats(s->input.joy));
  color(r,170,190,183); text(r,824,190,line,28);
  if (s->monitor_page) {
    int axis_pages=(s->input.axes+7)/8;
    int axes=s->monitor_page<=axis_pages;
    int page=s->monitor_page-1-(axes?0:axis_pages);
    int nbuttons=s->input.raw?SDL_JoystickNumButtons(s->input.joy):s->input.buttons;
    int count=axes?s->input.axes:nbuttons+4*s->input.hats;
    snprintf(line,sizeof line,"%s %d-%d / %d",axes?"AXES":"BUTTONS/HAT",page*8+1,
      (page+1)*8<count?(page+1)*8:count,count);
    color(r,129,224,165); text(r,824,214,line,28);
    for (int i=page*8;i<count && i<(page+1)*8;++i) {
      int input=axes || i<nbuttons?i:LHX_RAW_BUTTONS+i-nbuttons;
      char name[48]; physical(s,axes,input,name,sizeof name);
      int row=axes?axis_row(AXES(&s->edits)[input].action):button_row(BUTTONS(&s->edits)[input]);
      int y=240+(i%8)*34;
      color(r,220,231,219);text(r,824,y,name,28);
      if (axes) snprintf(line,sizeof line,"%d / %s",s->values[input],row>=0?actions[row].name:"Unbound");
      else snprintf(line,sizeof line,"%s / %s",s->previous[input]?"DOWN":"UP",row>=0?actions[row].name:"Unbound");
      color(r,129,224,165);text(r,836,y+16,line,27);
    }
    color(r,170,190,183); text(r,824,522,"M: NEXT LIVE / EVENTS PAGE",28); return;
  }
  color(r,170,190,183); text(r,824,214,"RECENT BUTTON / HAT",28);
  for (int i=0;i<3;++i) {
    color(r,220,231,219); text(r,824,234+i*36,s->buttons[i].input,28);
    color(r,129,224,165); text(r,836,250+i*36,s->buttons[i].action,27);
  }
  color(r,170,190,183); text(r,824,346,"RECENT AXIS MOVEMENT",28);
  for (int i=0;i<2;++i) {
    color(r,220,231,219);wrapped(r,824,368+i*64,s->motion[i].input,28,2);
    color(r,129,224,165);text(r,836,406+i*64,s->motion[i].action,27);
  }
  color(r,170,190,183); text(r,824,500,"ACTIVE / M: LIVE VALUES",28);
  char active[1024]="";
  for (int i=0;i<LHX_SETUP_ACTIONS;++i) if (s->active[i]) append(active,sizeof active,actions[i].name);
  color(r,129,224,165);wrapped(r,824,522,*active?active:s->input.joy?"None":"Controller unavailable",28,1);
}
void lhx_controller_setup_draw(const LhxControllerSetup *s,SDL_Renderer *r,const char *path) {
  if (s->flex.enabled) { lhx_flex_draw(s,r,path);return; }
  color(r,12,19,23); SDL_RenderClear(r);
  color(r,129,224,165); text(r,20,20,"LHX / CONTROLLER ACTIONS",70);
  ui_button(r,900,20,280,s->settings?"S: BACK TO ACTIONS":"S: DEVICE / SETTINGS",s->settings);
  color(r,210,220,215);text(r,20,56,lhx_flex_hotas_offer(s)?
    "H / HOTAS preset: stick, direct lift, pedals. Custom bindings stay. Review before saving.":
    "Click a binding to change it. Or select an action, Enter / Bind, then operate its control.",96);
  text(r,20,80,"Save profile:",14); text(r,200,80,path,81);
  char line[1024];
  if (s->input.joy) {
    SDL_Joystick *joy=s->input.joy;
    snprintf(line,sizeof line,"%s [%04x:%04x] / %s",SDL_JoystickName(s->input.joy),SDL_JoystickGetVendor(joy),
      SDL_JoystickGetProduct(joy),*s->edits.controller_guid?"Selected device":"Automatic / first");
  } else snprintf(line,sizeof line,"%s - keyboard / mouse remain available",*s->edits.controller_guid?"Selected controller unavailable":"No supported controller");
  color(r,129,224,165); text(r,20,108,line,96);
  text(r,28,134,s->settings?"DEVICE / SETTINGS (SECONDARY)":"ACTION",63);
  if (!s->settings) {
    text(r,360,134,"CURRENT BINDING",35);
    snprintf(line,sizeof line,"%d-%d / %d",s->scroll+1,s->scroll+VISIBLE,LHX_SETUP_ACTIONS);
    text(r,624,134,line,13);
  }
  Uint32 now=SDL_GetTicks();
  for (int slot=0;slot<VISIBLE;++slot) {
    int row=s->settings?LHX_SETUP_ACTIONS+slot:s->scroll+slot, y=LIST_Y+slot*ROW_H;
    if (row>=(s->settings?LHX_SETUP_ROWS:LHX_SETUP_ACTIONS)) break;
    int lit=!s->settings && (s->active[row] || (s->highlight[row] && (Sint32)(s->highlight[row]-now)>0));
    color(r,row==s->row?42:lit?35:22,row==s->row?72:lit?78:33,row==s->row?61:lit?50:38); rect(r,20,y,LIST_W-20,ROW_H-2);
    color(r,220,231,219);
    if (s->settings) { setting_label(s,row,line,sizeof line); text(r,28,y+5,line,62); }
    else {
      text(r,28,y+5,actions[row].name,27);
      bindings(s,&s->edits,row,line,sizeof line);
      if (strlen(line)>35) { memcpy(line+21,"... (select)",13); line[34]=0; }
      color(r,129,224,165); text(r,360,y+5,line,35);
      if (lit) { color(r,129,224,165); rect(r,20,y,4,ROW_H-2); }
    }
  }
  if (!s->settings) {
    ui_button(r,20,508,210,"BIND (ENTER)",s->capture);
    ui_button(r,246,508,210,"DEL: CLEAR",0); ui_button(r,472,508,318,"I: INVERT AXIS",0);
  } else {
    color(r,170,190,183); text(r,28,350,"Left/Right or click halves to adjust settings.",62);
    text(r,28,376,"Enter restores defaults on its selected row.",62);
    text(r,28,402,"Mapped and raw layouts kept separately. M: live pages.",62);
  }
  monitor(s,r);
  color(r,22,33,38); rect(r,20,556,1160,132);
  color(r,129,224,165);
  if (s->row<LHX_SETUP_ACTIONS) {
    snprintf(line,sizeof line,"ALL CURRENT BINDINGS / %s",actions[s->row].name); text(r,32,568,line,94);
    bindings(s,&s->edits,s->row,line,sizeof line);
    color(r,220,231,219); wrapped(r,32,592,line,94,5);
  } else {
    text(r,32,568,"KEYBOARD / MOUSE ALWAYS NAVIGATE",94);
    color(r,220,231,219); text(r,32,596,"Bindings open from the current INI / defaults. Saving preserves unrelated options and comments.",94);
    text(r,32,622,"Controller input only monitors or binds. It never selects rows, saves or starts the game.",94);
  }
  color(r,s->capture || s->conflict?255:210,s->capture || s->conflict?213:220,s->capture || s->conflict?109:215);
  wrapped(r,20,702,s->status,96,2);
  color(r,170,190,183); text(r,20,744,"Arrows/Tab: select   PgUp/PgDn/wheel: scroll   Esc: cancel   Lift: +/-10-unit steps",96);
  ui_button(r,310,20,180,"F2: MULTI-USB",0);
  if (lhx_flex_hotas_offer(s)) ui_button(r,500,20,380,"H: HOTAS ONE + PEDALS",0);
  else ui_button(r,500,20,380,"F2: MULTI-USB BINDINGS",0);
  const int xs[]={20,380,940}, widths[]={340,540,240};
  const char *labels[]={"SAVE AND START","START WITHOUT SAVING","CANCEL"};
  for (int i=0;i<3;++i) ui_button(r,xs[i],766,widths[i],labels[i],s->row==LHX_SETUP_ROWS+i);
  color(r,170,190,183); text(r,20,820,"Session start keeps edits in memory. Cancel / close exits without saving or starting.",96);
  if (s->capture) {
    color(r,48,59,37); rect(r,20,556,1160,132);
    color(r,255,213,109); snprintf(line,sizeof line,"BIND / %s",actions[s->row].name); text(r,32,568,line,94);
    color(r,230,232,216); text(r,32,596,!actions[s->row].button?"Move one axis now. Stationary controls are ignored.":actions[s->row].axis?
      "Press a button or trigger. Stick axes also support this action.":"Press a button or hat direction. This action does not support axis triggers.",94);
    text(r,32,620,"Release buttons / triggers already held before binding. Escape cancels; no changes yet.",94);
    bindings(s,&s->edits,s->row,line,sizeof line);
    color(r,170,190,183); text(r,32,648,"Current bindings remain until you confirm.",70);
    ui_button(r,900,642,280,"CANCEL BIND",1);
  }
  if (s->conflict) {
    color(r,5,10,12); rect(r,76,142,1048,552); color(r,35,49,47); rect(r,80,146,1040,544);
    color(r,255,213,109); snprintf(line,sizeof line,"REPLACE BINDING / %s",actions[s->row].name); text(r,100,166,line,83);
    char input[64]; physical(s,s->candidate_axis,s->candidate_input,input,sizeof input);
    snprintf(line,sizeof line,"New binding: %s%s",input,s->candidate_axis?(s->candidate_direction<0?" - (inverted)":" +"):"");
    color(r,220,231,219); text(r,100,198,line,83);
    color(r,170,190,183); text(r,100,230,"Replace ALL these bindings for the selected action:",83);
    bindings(s,&s->edits,s->row,line,sizeof line); color(r,220,231,219); wrapped(r,100,246,line,83,5);
    color(r,255,213,109); text(r,100,348,"This input will be removed from these old actions:",83);
    int n=0;
    for (int row=0;row<LHX_SETUP_ACTIONS;++row) if (s->displaced & (1u<<row)) {
      color(r,220,231,219); text(r,100+(n%2)*500,376+(n/2)*20,actions[row].name,40); ++n;
    }
    if (!n) { color(r,220,231,219); text(r,100,376,"None. Other actions keep their bindings.",83); }
    ui_button(r,100,628,470,"ENTER: MOVE / REPLACE",1);
    ui_button(r,620,628,480,"ESC: KEEP CURRENT BINDINGS",0);
    color(r,170,190,183); text(r,100,672,"Nothing changes until you confirm.",83);
  }
}
int lhx_controller_setup_accept(LhxControllerSetup *s,const char *path,LhxControls *output) {
  if (s->result==LHX_SETUP_CANCEL) return 0;
  if (s->flex.enabled && (s->capture || s->flex.page==2 || s->flex.page==3 || s->flex.import_prompt)) { s->result=LHX_SETUP_PENDING;return -1; }
  if (!lhx_usb_profile_valid(&s->edits.usb)) { snprintf(s->status,sizeof s->status,"Invalid USB draft; no settings applied");s->result=LHX_SETUP_PENDING;return -1; }
  if (s->result!=LHX_SETUP_SESSION && s->result!=LHX_SETUP_SAVE) return -1;
  if (s->result==LHX_SETUP_SAVE && !lhx_controls_save(&s->edits,path,s->status,sizeof s->status)) {
    s->result=LHX_SETUP_PENDING; return -1;
  }
  *output=s->edits; return 1;
}
int lhx_controller_setup_run(LhxControls *config,const char *path) {
  Uint32 flags=SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS;
  if (SDL_InitSubSystem(flags)<0) { fprintf(stderr,"Controller setup: %s\n",SDL_GetError()); return -1; }
  SDL_Window *window=SDL_CreateWindow("LHX - Controller setup",SDL_WINDOWPOS_CENTERED,SDL_WINDOWPOS_CENTERED,
    LHX_SETUP_WIDTH,LHX_SETUP_HEIGHT,SDL_WINDOW_RESIZABLE);
  SDL_Renderer *renderer=window?SDL_CreateRenderer(window,-1,SDL_RENDERER_SOFTWARE):NULL;
  if (!renderer) {
    fprintf(stderr,"Controller setup: %s\n",SDL_GetError()); SDL_DestroyWindow(window); SDL_QuitSubSystem(flags); return -1;
  }
  SDL_SetWindowMinimumSize(window,960,672);
  SDL_RenderSetLogicalSize(renderer,LHX_SETUP_WIDTH,LHX_SETUP_HEIGHT);
  LhxControllerSetup setup; lhx_controller_setup_init(&setup,config);
  int result=0, done=0;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) lhx_controller_setup_event(&setup,&event);
    lhx_controller_setup_tick(&setup);
    if (setup.result) {
      result=lhx_controller_setup_accept(&setup,path,config);
      done=result>=0;
    }
    lhx_controller_setup_draw(&setup,renderer,path); SDL_RenderPresent(renderer);
    if (!done) SDL_Delay(16);
  }
  lhx_controller_setup_close(&setup);
  SDL_DestroyRenderer(renderer); SDL_DestroyWindow(window); SDL_QuitSubSystem(flags);
  /* Do not pass setup input on to the newly initialized game. */
  SDL_FlushEvents(SDL_FIRSTEVENT,SDL_LASTEVENT);
  return result;
}

void lhx_ui_color(SDL_Renderer *r,int a,int b,int c) { color(r,a,b,c); }
void lhx_ui_rect(SDL_Renderer *r,int x,int y,int w,int h) { rect(r,x,y,w,h); }
void lhx_ui_text(SDL_Renderer *r,int x,int y,const char *s,int n) { text(r,x,y,s,n); }
void lhx_ui_wrapped(SDL_Renderer *r,int x,int y,const char *s,int n,int l) { wrapped(r,x,y,s,n,l); }
void lhx_ui_button(SDL_Renderer *r,int x,int y,int w,const char *s,int selected) { ui_button(r,x,y,w,s,selected); }
