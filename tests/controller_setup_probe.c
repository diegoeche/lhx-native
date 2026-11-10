/* Integration fixture: real SDL virtual devices, UI events, INI and runtime. */
#include "controller_setup.h"
#include "gamepad.h"
#include "lifted.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Uint8 down[65536];
static unsigned fire_makes;
static uint64_t now=1000000;
static void key(unsigned scan,int release) {
  down[scan]=!release;
  if (scan==0x39 && !release) ++fire_makes;
}
static void step(void) {
  SDL_JoystickUpdate();
  SDL_Event event;
  while (SDL_PollEvent(&event)) lhx_gamepad_event(&event);
  lhx_gamepad_poll(now+=1000);
}
static void press(LhxControllerSetup *s,SDL_Keycode key) {
  SDL_Event e; SDL_zero(e); e.type=SDL_KEYDOWN; e.key.timestamp=SDL_GetTicks(); e.key.state=SDL_PRESSED; e.key.repeat=0; e.key.keysym.sym=key;
  lhx_controller_setup_event(s,&e);
}
static void click(LhxControllerSetup *s,int x,int y) {
  SDL_Event e; SDL_zero(e); e.type=SDL_MOUSEBUTTONDOWN;
  e.button.timestamp=SDL_GetTicks(); e.button.button=SDL_BUTTON_LEFT; e.button.state=SDL_PRESSED;
  e.button.clicks=1; e.button.x=x; e.button.y=y;
  lhx_controller_setup_event(s,&e);
}
static SDL_Joystick *attach(int product,const char *name) {
  SDL_VirtualJoystickDesc desc={0};
  desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  desc.naxes=6; desc.nbuttons=4; desc.nhats=1;
  desc.vendor_id=0xfeed; desc.product_id=(Uint16)product; desc.name=name;
  int index=SDL_JoystickAttachVirtualEx(&desc); assert(index>=0);
  char guid[33],mapping[1024];
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof guid);
  snprintf(mapping,sizeof mapping,"%s,%s,a:b0,b:b1,x:b2,y:b3,"
    "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:a4,righttrigger:a5,"
    "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,",guid,name);
  assert(SDL_GameControllerAddMapping(mapping)>=0);
  SDL_Joystick *joy=SDL_JoystickOpen(index); assert(joy);
  assert(SDL_JoystickSetVirtualAxis(joy,4,-32768)==0);
  assert(SDL_JoystickSetVirtualAxis(joy,5,-32768)==0);
  SDL_JoystickUpdate(); return joy;
}
static void detach(SDL_Joystick *joy) {
  SDL_JoystickID instance=SDL_JoystickInstanceID(joy);
  for (int i=0;i<SDL_NumJoysticks();++i) if (SDL_JoystickGetDeviceInstanceID(i)==instance) {
    assert(SDL_JoystickDetachVirtual(i)==0); break;
  }
  SDL_JoystickClose(joy);
}
static char *contents(const char *path) {
  FILE *file=fopen(path,"rb"); assert(file);
  assert(!fseek(file,0,SEEK_END)); long length=ftell(file); assert(length>=0);
  rewind(file); char *data=calloc((size_t)length+1,1); assert(data);
  assert(fread(data,1,(size_t)length,file)==(size_t)length); assert(!fclose(file)); return data;
}
static void unchanged(const char *path,const char *before) {
  char *after=contents(path); assert(!strcmp(after,before)); free(after);
}
static void screenshot(const LhxControllerSetup *s,const char *path) {
  SDL_Window *win=SDL_CreateWindow("LHX setup test",0,0,LHX_SETUP_WIDTH,LHX_SETUP_HEIGHT,SDL_WINDOW_HIDDEN); assert(win);
  SDL_Renderer *r=SDL_CreateRenderer(win,-1,SDL_RENDERER_SOFTWARE); assert(r);
  assert(!SDL_RenderSetLogicalSize(r,LHX_SETUP_WIDTH,LHX_SETUP_HEIGHT));
  lhx_controller_setup_draw(s,r,"/tmp/pilot-controls.ini");
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,LHX_SETUP_WIDTH,LHX_SETUP_HEIGHT,32,SDL_PIXELFORMAT_ARGB8888); assert(surface);
  assert(!SDL_RenderReadPixels(r,NULL,surface->format->format,surface->pixels,surface->pitch));
  assert(!SDL_SaveBMP(surface,path));
  SDL_FreeSurface(surface); SDL_DestroyRenderer(r); SDL_DestroyWindow(win);
}
static void ui_step(LhxControllerSetup *s) {
  SDL_JoystickUpdate();
  SDL_Event event;
  while (SDL_PollEvent(&event)) lhx_controller_setup_event(s,&event);
  lhx_controller_setup_tick(s);
}
static void select_action(LhxControllerSetup *s,int row) {
  if (s->settings) press(s,SDLK_s);
  int guard=40;
  while (s->row!=row && guard--) press(s,SDLK_DOWN);
  assert(s->row==row);
}
static void arm(LhxControllerSetup *s,int row) {
  select_action(s,row); press(s,SDLK_RETURN); assert(s->capture && !s->conflict);
}
static void axis(SDL_Joystick *joy,int input,int value,LhxControllerSetup *s) {
  assert(!SDL_JoystickSetVirtualAxis(joy,input,(Sint16)value)); ui_step(s);
}
static void button(SDL_Joystick *joy,int input,int value,LhxControllerSetup *s) {
  assert(!SDL_JoystickSetVirtualButton(joy,input,(Uint8)value)); ui_step(s);
}
static void shot_state(const LhxControllerSetup *s,const char *base,const char *state) {
  char path[1024]; snprintf(path,sizeof path,"%s-%s.bmp",base,state); screenshot(s,path);
}
static void confirm(LhxControllerSetup *s) {
  assert(s->conflict && !s->capture); press(s,SDLK_RETURN); assert(!s->conflict);
}
static void no_change(const LhxControllerSetup *s,const LhxControls *c,const char *path,const char *before) {
  assert(!memcmp(&s->edits,c,sizeof *c)); unchanged(path,before);
}
static void integration(const char *path,const char *shot) {
  LhxControls config,output,saved,baseline;
  assert(lhx_controls_load(&config,path,1));
  /* An inherited multi-input action must survive entry, and be fully visible. */
  config.buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]=LHX_FIRE;
  config.axes[4].action=LHX_AXIS_FIRE;
  char *before=contents(path);
  LhxControllerSetup s;
  lhx_controller_setup_init(&s,&config); assert(!s.input.pad);
  assert(!memcmp(&s.edits,&config,sizeof config));
  press(&s,SDLK_DOWN); assert(s.row==LHX_SETUP_PITCH_ROW); press(&s,SDLK_UP);
  SDL_Joystick *alpha=attach(0xb010,"Virtual HOTAS Alpha");
  SDL_Joystick *beta=attach(0xb011,"Virtual HOTAS Bravo");
  ui_step(&s); assert(s.input.pad);
  assert(SDL_JoystickGetProduct(SDL_GameControllerGetJoystick(s.input.pad))==0xb010);
  press(&s,SDLK_s); assert(s.settings && s.row==LHX_SETUP_DEVICE_ROW);
  press(&s,SDLK_RIGHT); press(&s,SDLK_RIGHT); /* Automatic -> Alpha -> Bravo */
  assert(SDL_JoystickGetProduct(SDL_GameControllerGetJoystick(s.input.pad))==0xb011);
  assert(*s.edits.controller_guid); press(&s,SDLK_s);
  /* Actual action-row mouse selection changes no binding. */
  click(&s,80,156+LHX_SETUP_FIRE_ROW*26+8); assert(s.row==LHX_SETUP_FIRE_ROW);
  baseline=s.edits;
  char names[1024]; lhx_controller_setup_bindings(&s,s.row,names,sizeof names);
  assert(strstr(names,"leftshoulder") && strstr(names,"Axis 5") && strstr(names,"Axis 6"));
  shot_state(&s,shot,"actions");
  press(&s,SDLK_RETURN); assert(s.capture && strstr(s.status,"button or trigger"));
  shot_state(&s,shot,"listening");
  press(&s,SDLK_RETURN); click(&s,50,780); assert(!s.result && s.capture);
  button(alpha,0,1,&s); axis(alpha,0,32767,&s);
  assert(s.capture && !*s.buttons[0].input && !*s.motion[0].input); /* selected device only */
  button(beta,0,1,&s); assert(s.conflict && !s.capture);
  assert(s.displaced & (1u<<LHX_SETUP_CONFIRM_ROW));
  assert(strstr(s.buttons[0].input,"Button 1 DOWN") && strstr(s.buttons[0].action,"Confirm"));
  assert(s.pending.buttons[SDL_CONTROLLER_BUTTON_A]==LHX_FIRE);
  assert(s.pending.axes[4].action==LHX_AXIS_NONE && s.pending.axes[5].action==LHX_AXIS_NONE);
  assert(s.pending.buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]==LHX_NONE);
  no_change(&s,&baseline,path,before); shot_state(&s,shot,"conflict");
  press(&s,SDLK_ESCAPE); assert(!s.conflict); no_change(&s,&baseline,path,before);
  /* Held button at arming is ignored until release and a fresh press. */
  click(&s,80,520); assert(s.capture); ui_step(&s); assert(s.capture);
  click(&s,1020,655); assert(!s.capture && !s.result); no_change(&s,&baseline,path,before);
  button(beta,0,0,&s);
  click(&s,430,156+LHX_SETUP_FIRE_ROW*26+8); assert(s.capture); /* single binding-cell click */
  button(beta,0,1,&s); assert(s.conflict);
  click(&s,800,645); assert(!s.conflict); no_change(&s,&baseline,path,before);
  /* Focus loss cancels listening AND an uncommitted replacement. */
  arm(&s,LHX_SETUP_FIRE_ROW);
  SDL_Event focus; SDL_zero(focus); focus.type=SDL_WINDOWEVENT; focus.window.timestamp=SDL_GetTicks(); focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
  lhx_controller_setup_event(&s,&focus); assert(!s.capture && !s.conflict); no_change(&s,&baseline,path,before);
  focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED; lhx_controller_setup_event(&s,&focus);
  button(beta,0,0,&s); arm(&s,LHX_SETUP_FIRE_ROW); button(beta,0,1,&s); assert(s.conflict);
  focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST; lhx_controller_setup_event(&s,&focus);
  assert(!s.conflict); no_change(&s,&baseline,path,before);
  focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED; lhx_controller_setup_event(&s,&focus);
  button(beta,0,0,&s); arm(&s,LHX_SETUP_FIRE_ROW); button(beta,0,1,&s); confirm(&s);
  LhxControls expected=baseline;
  expected.buttons[SDL_CONTROLLER_BUTTON_A]=LHX_FIRE;
  expected.buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]=LHX_NONE;
  expected.axes[4].action=expected.axes[5].action=LHX_AXIS_NONE;
  assert(!memcmp(&expected,&s.edits,sizeof expected)); unchanged(path,before);
  button(beta,0,0,&s); button(beta,0,1,&s);
  assert(strstr(s.buttons[0].action,"Fire") && s.active[LHX_SETUP_FIRE_ROW]);
  int selected=s.row,scroll=s.scroll;
  /* Meaningful axes do not erase button history, select rows, or start/save. */
  char recent[64]; strcpy(recent,s.buttons[0].input);
  axis(beta,3,22000,&s);
  for (int v=23000;v<30000;v+=1000) axis(beta,3,v,&s);
  assert(!strcmp(recent,s.buttons[0].input)); assert(s.row==selected && s.scroll==scroll && !s.result);
  assert(s.active[LHX_SETUP_COLLECTIVE_ROW] && s.active[LHX_SETUP_DECREASE_ROW]);
  shot_state(&s,shot,"feedback");
  /* Stable snapshot ignores the stationary off-center lever, while a fresh
   * movement immediately after Enter binds in ONE move, with no delay. */
  arm(&s,LHX_SETUP_ROLL_ROW); ui_step(&s); assert(s.capture);
  SDL_Event stale; SDL_zero(stale); stale.type=SDL_CONTROLLERAXISMOTION;
  assert(s.armed_at>1); stale.caxis.timestamp=s.armed_at-1; stale.caxis.which=s.instance;
  stale.caxis.axis=SDL_CONTROLLER_AXIS_LEFTX; stale.caxis.value=27000;
  lhx_controller_setup_event(&s,&stale); assert(s.capture && !s.conflict);
  axis(alpha,2,32767,&s); assert(s.capture);
  axis(beta,2,27000,&s); assert(s.conflict);
  assert(s.displaced & (1u<<LHX_SETUP_RUDDER_ROW)); confirm(&s);
  assert(s.edits.axes[0].action==LHX_AXIS_NONE && s.edits.axes[2].action==LHX_ROLL);
  assert(s.edits.axes[3].action==LHX_COLLECTIVE);
  /* Positive-only SDL triggers remain usable after Invert. Held triggers do
   * not bind from release; subsequent immediate press is captured. */
  axis(beta,4,32767,&s); arm(&s,LHX_SETUP_CHAFF_ROW); ui_step(&s); assert(s.capture);
  axis(beta,4,-32768,&s); assert(s.capture);
  axis(beta,4,32767,&s); assert(s.conflict); confirm(&s);
  assert(s.edits.axes[4].action==LHX_AXIS_CHAFF && s.edits.axes[4].direction==1);
  press(&s,SDLK_i); assert(s.edits.axes[4].direction==1 && strstr(s.status,"positive-only"));
  assert(s.edits.buttons[SDL_CONTROLLER_BUTTON_X]==LHX_NONE);
  /* Unsupported combinations say what to do, and keep listening. */
  axis(beta,5,-32768,&s); arm(&s,LHX_SETUP_NEXT_ROW); assert(strstr(s.status,"button or hat"));
  axis(beta,5,32767,&s); assert(s.capture && strstr(s.status,"needs a button"));
  press(&s,SDLK_ESCAPE); axis(beta,5,-32768,&s);
  arm(&s,LHX_SETUP_HOVER_ROW); axis(beta,5,32767,&s);
  assert(!s.capture && !s.conflict && s.edits.axes[5].action==LHX_AXIS_HOVER);
  /* A signed stick axis binds a discrete action on its moved side. */
  button(beta,0,0,&s); axis(beta,1,0,&s);
  arm(&s,LHX_SETUP_FIRE_ROW); axis(beta,1,-27000,&s); assert(s.conflict);
  assert(s.displaced & (1u<<LHX_SETUP_PITCH_ROW)); confirm(&s);
  assert(s.edits.axes[1].action==LHX_AXIS_FIRE && s.edits.axes[1].direction==-1);
  press(&s,SDLK_i); assert(s.edits.axes[1].direction==1);
  press(&s,SDLK_i); assert(s.edits.axes[1].direction==-1);
  lhx_gamepad_configure(&s.edits); lhx_gamepad_init(key); step(); assert(down[0x39]);
  axis(beta,1,0,&s); step(); assert(!down[0x39]); lhx_gamepad_close();
  arm(&s,LHX_SETUP_FIRE_ROW); button(beta,0,1,&s); confirm(&s);
  assert(s.edits.axes[1].action==LHX_AXIS_NONE);
  arm(&s,LHX_SETUP_PITCH_ROW); axis(beta,1,27000,&s);
  assert(!s.capture && !s.conflict && s.edits.axes[1].action==LHX_PITCH);
  press(&s,SDLK_i); assert(s.edits.axes[1].direction==-1);
  /* A hat binds the selected existing button action, with transparent displacement. */
  arm(&s,LHX_SETUP_NEXT_ROW);
  assert(!SDL_JoystickSetVirtualHat(beta,0,SDL_HAT_LEFT)); ui_step(&s);
  assert(s.conflict && (s.displaced & (1u<<LHX_SETUP_LEFT_ROW))); confirm(&s);
  assert(s.edits.buttons[SDL_CONTROLLER_BUTTON_DPAD_LEFT]==LHX_NEXT);
  assert(s.edits.buttons[SDL_CONTROLLER_BUTTON_RIGHTSTICK]==LHX_NONE);
  /* Clear and restore defaults are explicit draft operations. */
  LhxControls draft=s.edits; select_action(&s,LHX_SETUP_FIRE_ROW); press(&s,SDLK_DELETE);
  assert(s.edits.buttons[SDL_CONTROLLER_BUTTON_A]==LHX_NONE);
  press(&s,SDLK_s); select_action(&s,LHX_SETUP_FIRE_ROW); /* S and selection never reset. */
  assert(s.edits.buttons[SDL_CONTROLLER_BUTTON_A]==LHX_NONE);
  s.edits=draft;
  /* Start-once, failed Save, cancel/close leave file/caller intact. */
  click(&s,500,780); assert(s.result==LHX_SETUP_SESSION);
  output=config; assert(lhx_controller_setup_accept(&s,path,&output)==1); unchanged(path,before);
  assert(!memcmp(&output,&s.edits,sizeof output)); s.result=0;
  arm(&s,LHX_SETUP_FIRE_ROW); click(&s,1050,780); assert(s.result==LHX_SETUP_CANCEL && !s.capture);
  output=config; assert(!lhx_controller_setup_accept(&s,path,&output));
  assert(!memcmp(&output,&config,sizeof config)); unchanged(path,before); s.result=0;
  arm(&s,LHX_SETUP_FIRE_ROW); SDL_Event quit; SDL_zero(quit); quit.type=SDL_QUIT; quit.common.timestamp=SDL_GetTicks(); lhx_controller_setup_event(&s,&quit);
  assert(s.result==LHX_SETUP_CANCEL && !s.capture); unchanged(path,before); s.result=0;
  click(&s,50,780); assert(s.result==LHX_SETUP_SAVE);
  assert(lhx_controller_setup_accept(&s,"/proc/lhx-controls-no-such-file",&output)==-1);
  assert(!s.result && *s.status); assert(!memcmp(&output,&config,sizeof config)); unchanged(path,before);
  click(&s,50,780); assert(lhx_controller_setup_accept(&s,path,&output)==1);
  assert(lhx_controls_load(&saved,path,1));
  assert(!memcmp(saved.buttons,output.buttons,sizeof saved.buttons));
  assert(!memcmp(saved.axes,output.axes,sizeof saved.axes));
  assert(!strcmp(saved.controller_guid,output.controller_guid));
  assert(saved.improved_sounds==config.improved_sounds);
  assert(!memcmp(&saved.rendering,&config.rendering,sizeof saved.rendering));
  /* Saved bindings reach the unchanged runtime adapter. */
  button(beta,0,0,&s); axis(beta,1,0,&s); axis(beta,2,0,&s); axis(beta,3,0,&s);
  axis(beta,4,-32768,&s); axis(beta,5,-32768,&s);
  assert(!SDL_JoystickSetVirtualHat(beta,0,SDL_HAT_CENTERED)); ui_step(&s);
  fire_makes=0; lhx_gamepad_configure(&saved); lhx_gamepad_init(key); step(); assert(!down[0x39]);
  assert(!SDL_JoystickSetVirtualButton(beta,0,1)); step(); assert(fire_makes==1 && down[0x39]);
  CPU cpu={0}; cpu.ds=0x2000; cpu.mem=calloc(1,MEM_SIZE); assert(cpu.mem);
  assert(!SDL_JoystickSetVirtualAxis(beta,1,32767)); assert(!SDL_JoystickSetVirtualAxis(beta,2,32767));
  step(); lhx_gamepad_apply(&cpu);
  assert((int16_t)mem_read16(&cpu,cpu.ds,0xead6)==100);
  assert((int16_t)mem_read16(&cpu,cpu.ds,0xead8)==-100);
  assert(!SDL_JoystickSetVirtualAxis(beta,3,-32768)); step(); assert(down[0x08]);
  assert(!SDL_JoystickSetVirtualAxis(beta,4,32767)); step(); assert(down[0x2e]);
  assert(!SDL_JoystickSetVirtualAxis(beta,5,32767)); step(); assert(down[0x04]);
  /* Chosen-device disconnect cancels a pending replacement without mutation;
   * runtime releases keys and monitoring resumes only for the selected GUID. */
  s.result=0; axis(beta,0,0,&s); arm(&s,LHX_SETUP_ROLL_ROW); axis(beta,0,27000,&s); assert(s.conflict);
  draft=s.edits; detach(beta); step(); lhx_controller_setup_tick(&s);
  assert(!s.input.pad && !s.capture && !s.conflict && !down[0x39] && !down[0x08]);
  assert(!memcmp(&draft,&s.edits,sizeof draft)); assert(strstr(s.status,"disconnected"));
  unsigned shots=fire_makes; step(); assert(fire_makes==shots);
  beta=attach(0xb011,"Virtual HOTAS Bravo"); step(); ui_step(&s);
  assert(s.input.pad && strstr(s.status,"connected and ready"));
  button(beta,0,1,&s); assert(strstr(s.buttons[0].action,"Fire"));
  step(); assert(fire_makes==shots+1);
  /* Listening disconnect also cancels and reconnect clears stale status. */
  arm(&s,LHX_SETUP_FIRE_ROW); detach(beta); ui_step(&s); assert(!s.capture && !s.input.pad);
  beta=attach(0xb011,"Virtual HOTAS Bravo"); ui_step(&s); assert(s.input.pad && strstr(s.status,"connected and ready"));
  lhx_gamepad_close();
  /* Default restoration retains graphics, audio and selected-device identity. */
  draft=s.edits; press(&s,SDLK_s);
  while (s.row!=LHX_SETUP_DEFAULTS_ROW) press(&s,SDLK_DOWN);
  press(&s,SDLK_RETURN); LhxControls defaults; lhx_controls_defaults(&defaults);
  assert(!memcmp(s.edits.buttons,defaults.buttons,sizeof defaults.buttons));
  assert(!memcmp(s.edits.axes,defaults.axes,sizeof defaults.axes));
  assert(!memcmp(&s.edits.rendering,&draft.rendering,sizeof draft.rendering));
  assert(!strcmp(s.edits.controller_guid,draft.controller_guid));
  lhx_controller_setup_close(&s);
  lhx_controls_defaults(&config); lhx_gamepad_configure(&config); lhx_gamepad_init(key);
  step(); assert(down[0x1c]); lhx_gamepad_close();
  detach(beta); detach(alpha); free(cpu.mem); free(before);
  puts("Action-first immediate capture, conflicts, feedback, runtime actions and hotplug passed");
}
static SDL_Joystick *attach_raw(int product,int axes,int buttons,int hats) {
  SDL_VirtualJoystickDesc d={0}; d.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type=SDL_JOYSTICK_TYPE_UNKNOWN; d.naxes=(Uint16)axes; d.nbuttons=(Uint16)buttons; d.nhats=(Uint16)hats;
  d.vendor_id=0xfeed; d.product_id=(Uint16)product; d.name="Unmapped native flight test";
  int index=SDL_JoystickAttachVirtualEx(&d); assert(index>=0);
  SDL_Joystick *joy=SDL_JoystickOpen(index); assert(joy); return joy;
}
static int index_of(SDL_Joystick *joy) {
  for (int i=0;i<SDL_NumJoysticks();++i)
    if (SDL_JoystickGetDeviceInstanceID(i)==SDL_JoystickInstanceID(joy)) return i;
  assert(0); return -1;
}
static void native(const char *path,const char *shot) {
  LhxControls c,loaded,out;
  assert(lhx_controls_load(&c,path,1));
  char *before=contents(path),names[1024];
  LhxControllerSetup s; lhx_controller_setup_init(&s,&c); assert(!s.input.joy);
  SDL_Joystick *pseudo=attach_raw(0xb012,1,19,1);
  ui_step(&s); assert(!s.input.joy && !lhx_input_eligible(index_of(pseudo)));
  lhx_gamepad_configure(&c); lhx_gamepad_init(key); step();
  assert(!SDL_JoystickSetVirtualButton(pseudo,0,1)); step(); assert(!down[0x1c]); lhx_gamepad_close();
  SDL_Joystick *alpha=attach_raw(0xb013,8,15,1);
  assert(!SDL_IsGameController(index_of(alpha)));
  ui_step(&s); assert(s.input.raw && !s.input.pad && s.input.axes==8 && s.input.hats==1);
  assert(s.input.joy && s.instance==SDL_JoystickInstanceID(alpha));
  assert(s.edits.input_mode==LHX_INPUT_JOYSTICK && *s.edits.controller_guid && !*s.edits.controller_serial);
  assert(!memcmp(c.buttons,s.edits.buttons,sizeof c.buttons));
  assert(!memcmp(c.axes,s.edits.axes,sizeof c.axes));
  lhx_controller_setup_bindings(&s,LHX_SETUP_COLLECTIVE_ROW,names,sizeof names); assert(!strcmp(names,"Unbound"));
  shot_state(&s,shot,"native-actions");
  lhx_gamepad_list();
  /* Conservative filter plus legacy first-mapped priority; explicit raw stays selected. */
  SDL_Joystick *mapped=attach(0xb010,"Virtual HOTAS Alpha");
  LhxControllerInput in={0}; assert(lhx_input_open_selected(&in,&c)); assert(in.pad && !in.raw);
  lhx_input_close(&in); detach(mapped);
  SDL_Joystick *beta=attach_raw(0xb014,10,17,2); assert(!SDL_IsGameController(index_of(beta))); ui_step(&s); assert(s.instance==SDL_JoystickInstanceID(alpha));
  axis(alpha,2,-22000,&s); arm(&s,LHX_SETUP_COLLECTIVE_ROW); ui_step(&s); assert(s.capture);
  shot_state(&s,shot,"native-listening");
  SDL_Event stale; SDL_zero(stale); stale.type=SDL_JOYAXISMOTION;
  stale.jaxis.which=s.instance; stale.jaxis.axis=2; stale.jaxis.value=30000;
  assert(s.armed_at>0); stale.jaxis.timestamp=s.armed_at-1;
  lhx_controller_setup_event(&s,&stale); assert(s.capture);
  axis(beta,2,32767,&s); assert(s.capture);
  axis(alpha,2,32767,&s); assert(!s.capture && !s.conflict);
  assert(s.edits.raw_axes[2].action==LHX_COLLECTIVE); unchanged(path,before);
  assert(strstr(s.motion[0].input,"Axis 3") && strstr(s.motion[0].action,"Unbound"));
  /* Every raw index 4..7 has signed polarity, even where mapped triggers live. */
  const int rows[]={LHX_SETUP_FLARES_ROW,LHX_SETUP_HOVER_ROW,LHX_SETUP_FIRE_ROW,LHX_SETUP_CHAFF_ROW};
  for (int i=4;i<8;++i) {
    axis(alpha,i,0,&s); arm(&s,rows[i-4]); axis(alpha,i,-30000,&s);
    assert(!s.capture && !s.conflict && s.edits.raw_axes[i].direction==-1);
    press(&s,SDLK_i); assert(s.edits.raw_axes[i].direction==1);
    press(&s,SDLK_i); assert(s.edits.raw_axes[i].direction==-1);
    axis(alpha,i,0,&s);
  }
  /* Release of a held signed action axis cannot bind; next press can. */
  axis(alpha,6,-30000,&s); arm(&s,LHX_SETUP_FIRE_ROW); axis(alpha,6,0,&s); assert(s.capture);
  axis(alpha,6,-30000,&s); assert(s.conflict); press(&s,SDLK_ESCAPE); assert(!s.conflict);
  axis(alpha,6,0,&s);
  arm(&s,LHX_SETUP_ROLL_ROW); axis(alpha,7,32767,&s); assert(s.conflict);
  assert(s.displaced & (1u<<LHX_SETUP_CHAFF_ROW)); LhxControls draft=s.edits;
  shot_state(&s,shot,"native-conflict"); press(&s,SDLK_ESCAPE); no_change(&s,&draft,path,before);
  axis(alpha,7,0,&s); arm(&s,LHX_SETUP_ROLL_ROW); axis(alpha,7,32767,&s); confirm(&s);
  assert(s.edits.raw_axes[7].action==LHX_ROLL);
  arm(&s,LHX_SETUP_PITCH_ROW); axis(alpha,0,-32768,&s); assert(!s.capture && !s.conflict);
  button(alpha,14,1,&s); arm(&s,LHX_SETUP_GEAR_ROW); ui_step(&s); assert(s.capture);
  button(alpha,14,0,&s); assert(s.capture); button(alpha,14,1,&s); assert(!s.capture);
  button(alpha,14,0,&s); button(alpha,14,1,&s);
  assert(strstr(s.buttons[0].input,"Button 15 DOWN") && strstr(s.buttons[0].action,"Landing gear"));
  /* All four hat directions, held/release/rising and simultaneous diagonal. */
  const int hats[]={LHX_SETUP_UP_ROW,LHX_SETUP_RIGHT_ROW,LHX_SETUP_DOWN_ROW,LHX_SETUP_LEFT_ROW};
  assert(!SDL_JoystickSetVirtualHat(alpha,0,SDL_HAT_UP)); ui_step(&s);
  arm(&s,hats[0]); ui_step(&s); assert(s.capture);
  assert(!SDL_JoystickSetVirtualHat(alpha,0,SDL_HAT_CENTERED)); ui_step(&s); assert(s.capture);
  for (int d=0;d<4;++d) {
    if (d) arm(&s,hats[d]);
    assert(!SDL_JoystickSetVirtualHat(alpha,0,(Uint8)(1<<d))); ui_step(&s); assert(!s.capture);
    assert(s.edits.raw_buttons[LHX_RAW_BUTTONS+d]!=LHX_NONE);
    assert(!SDL_JoystickSetVirtualHat(alpha,0,SDL_HAT_CENTERED)); ui_step(&s);
  }
  /* Keyboard/mouse cancel and focus loss never change the draft or file. */
  draft=s.edits; arm(&s,LHX_SETUP_FIRE_ROW);
  SDL_Event focus; SDL_zero(focus); focus.type=SDL_WINDOWEVENT; focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;
  lhx_controller_setup_event(&s,&focus); assert(!s.capture);
  axis(alpha,6,-32768,&s); press(&s,SDLK_RETURN); assert(!s.capture); no_change(&s,&draft,path,before);
  focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED; lhx_controller_setup_event(&s,&focus);
  arm(&s,LHX_SETUP_FIRE_ROW); click(&s,1020,655); assert(!s.capture); no_change(&s,&draft,path,before);
  arm(&s,LHX_SETUP_FIRE_ROW); click(&s,1050,780); assert(s.result==LHX_SETUP_CANCEL);
  out=c; assert(!lhx_controller_setup_accept(&s,path,&out)); assert(!memcmp(&out,&c,sizeof c)); unchanged(path,before); s.result=0;
  /* Paging exposes all 15 buttons and hat controls; raw axes show all eight. */
  s.monitor_page=1; shot_state(&s,shot,"native-live");
  press(&s,SDLK_m); press(&s,SDLK_m); assert(s.monitor_page==3); shot_state(&s,shot,"native-buttons");
  press(&s,SDLK_m); assert(s.monitor_page==4); shot_state(&s,shot,"native-hat");
  /* Session-only then atomic save/reload through the existing INI. */
  click(&s,500,780); assert(lhx_controller_setup_accept(&s,path,&out)==1); unchanged(path,before); s.result=0;
  click(&s,50,780); assert(lhx_controller_setup_accept(&s,path,&out)==1);
  assert(lhx_controls_load(&loaded,path,1));
  assert(!memcmp(&out,&loaded,sizeof out));
  LhxControllerSetup reopened; lhx_controller_setup_init(&reopened,&loaded);
  assert(reopened.input.raw && reopened.input.axes==8);
  lhx_controller_setup_bindings(&reopened,LHX_SETUP_COLLECTIVE_ROW,names,sizeof names);
  assert(strstr(names,"Axis 3")); lhx_controller_setup_close(&reopened);
  /* Repeated save and native duplicate keys use the same atomic replacement. */
  char *once=contents(path),error[256];
  assert(lhx_controls_save(&loaded,path,error,sizeof error)); unchanged(path,once); free(once);
  assert(!memcmp(loaded.buttons,c.buttons,sizeof c.buttons) && !memcmp(loaded.axes,c.axes,sizeof c.axes));
  assert(loaded.raw_axes[2].action==LHX_COLLECTIVE && loaded.raw_axes[6].action==LHX_AXIS_FIRE && loaded.raw_axes[7].action==LHX_ROLL);
  /* Adding an SDL mapping later cannot reinterpret persisted native indices. */
  char guid[33],map[512]; SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(alpha),guid,sizeof guid);
  snprintf(map,sizeof map,"%s,Later mapping,a:b0,leftx:a1,lefttrigger:a6,righttrigger:a7,",guid);
  assert(SDL_GameControllerAddMapping(map)>=0 && SDL_IsGameController(index_of(alpha)));
  assert(lhx_input_open_selected(&in,&loaded) && in.raw && in.axes==8); lhx_input_close(&in);
  for (int i=0;i<8;++i) axis(alpha,i,0,&s);
  button(alpha,14,0,&s); lhx_gamepad_configure(&loaded); lhx_gamepad_init(key); step(); assert(!down[0x39]);
  /* Other joystick and pseudo-keyboard cannot drive actions. */
  assert(!SDL_JoystickSetVirtualAxis(beta,6,-32768)); assert(!SDL_JoystickSetVirtualButton(beta,14,1)); step(); assert(!down[0x39] && !down[0x22]);
  assert(!SDL_JoystickSetVirtualAxis(alpha,6,-32768)); step(); assert(down[0x39]);
  assert(!SDL_JoystickSetVirtualAxis(alpha,2,-32768)); step(); assert(down[0x08] && !down[0x07]);
  assert(!SDL_JoystickSetVirtualAxis(alpha,2,32767)); step(); assert(down[0x07] && !down[0x08]);
  assert(!SDL_JoystickSetVirtualButton(alpha,14,1)); step(); assert(down[0x22]);
  assert(!SDL_JoystickSetVirtualHat(alpha,0,SDL_HAT_RIGHTUP)); step(); assert(down[0xe048] && down[0xe04d]);
  CPU cpu={0}; cpu.ds=0x2000; cpu.mem=calloc(1,MEM_SIZE); assert(cpu.mem);
  assert(!SDL_JoystickSetVirtualAxis(alpha,7,-32768)); assert(!SDL_JoystickSetVirtualAxis(alpha,0,32767)); step(); lhx_gamepad_apply(&cpu);
  assert((int16_t)mem_read16(&cpu,cpu.ds,0xead6)==-100 && (int16_t)mem_read16(&cpu,cpu.ds,0xead8)==100);
  lhx_gamepad_focus(0); step(); assert(!down[0x39] && !down[0x22] && !down[0x07]);
  lhx_gamepad_apply(&cpu); assert(!mem_read16(&cpu,cpu.ds,0xead6)); lhx_gamepad_focus(1);
  s.result=0; axis(alpha,6,0,&s); arm(&s,LHX_SETUP_FIRE_ROW); axis(alpha,6,32767,&s); assert(s.conflict);
  draft=s.edits; detach(alpha); step(); ui_step(&s);
  assert(!s.input.joy && !s.conflict && !s.capture && !down[0x39]);
  assert(!memcmp(&draft,&s.edits,sizeof draft)); shot_state(&s,shot,"native-disconnected");
  alpha=attach_raw(0xb013,8,15,1); step(); ui_step(&s); assert(s.input.joy && s.input.raw && s.instance==SDL_JoystickInstanceID(alpha));
  assert(strstr(s.status,"connected and ready"));
  assert(!SDL_JoystickSetVirtualAxis(alpha,6,-32768)); step(); assert(down[0x39]); lhx_gamepad_close();
  /* Missing/mismatched serial never falls back to another device. */
  LhxControls serial=loaded; strcpy(serial.controller_serial,"61");
  assert(!lhx_input_open_selected(&in,&serial));
  lhx_controller_setup_close(&s); detach(alpha);
  /* Higher capabilities also remain available, including axis 9 and button17/hat2. */
  lhx_controls_defaults(&c); lhx_controls_select_joystick(&c,beta); c.input_mode=LHX_INPUT_JOYSTICK;
  lhx_controller_setup_init(&s,&c); assert(s.input.axes==10);
  axis(beta,8,0,&s); arm(&s,LHX_SETUP_RUDDER_ROW); axis(beta,8,-32768,&s); assert(!s.capture);
  button(beta,16,0,&s); arm(&s,LHX_SETUP_FIRE_ROW); button(beta,16,1,&s); assert(!s.capture);
  assert(s.edits.raw_axes[8].action==LHX_RUDDER && s.edits.raw_buttons[16]==LHX_FIRE);
  arm(&s,LHX_SETUP_NEXT_ROW); assert(!SDL_JoystickSetVirtualHat(beta,1,SDL_HAT_DOWN)); ui_step(&s);
  assert(s.edits.raw_buttons[LHX_RAW_BUTTONS+6]==LHX_NEXT);
  /* Bounds fail visibly instead of silently truncating controls. */
  SDL_Joystick *large=attach_raw(0xb015,LHX_RAW_AXES+1,15,1);
  assert(!lhx_input_open(&in,index_of(large),LHX_INPUT_AUTO)); assert(strstr(SDL_GetError(),"limits")); detach(large);
  /* Native defaults clear only the raw layout and retain the mapped profile. */
  LhxControls native_draft=s.edits;
  press(&s,SDLK_s); while (s.row!=LHX_SETUP_DEFAULTS_ROW) press(&s,SDLK_DOWN); press(&s,SDLK_RETURN);
  for (int i=0;i<LHX_RAW_AXES;++i) assert(s.edits.raw_axes[i].action==LHX_AXIS_NONE);
  for (int i=0;i<LHX_INPUT_BUTTONS;++i) assert(s.edits.raw_buttons[i]==LHX_NONE);
  assert(!memcmp(s.edits.buttons,native_draft.buttons,sizeof c.buttons));
  assert(!memcmp(s.edits.axes,native_draft.axes,sizeof c.axes));
  lhx_controller_setup_close(&s); detach(beta); detach(pseudo); free(cpu.mem); free(before);
  puts("Unmapped native joystick UI/capture/INI/runtime/polarity/filter/hotplug passed");
}
static void preserve(const char *path) {
  LhxControls c,out,reloaded; assert(lhx_controls_load(&c,path,1));
  LhxControllerSetup s; lhx_controller_setup_init(&s,&c);
  char list[1024]; lhx_controller_setup_bindings(&s,LHX_SETUP_FIRE_ROW,list,sizeof list);
  assert(strstr(list,"SDL a") && strstr(list,"SDL x") && strstr(list,"lefttrigger") && strstr(list,"righttrigger"));
  click(&s,50,780); assert(lhx_controller_setup_accept(&s,path,&out)==1);
  assert(!memcmp(&c,&out,sizeof c)); assert(lhx_controls_load(&reloaded,path,1));
  assert(!memcmp(c.buttons,reloaded.buttons,sizeof c.buttons) && !memcmp(c.axes,reloaded.axes,sizeof c.axes));
  SDL_Joystick *joy=attach(0xb010,"Virtual HOTAS Alpha"); ui_step(&s); s.result=0;
  arm(&s,LHX_SETUP_ROLL_ROW); axis(joy,3,27000,&s); confirm(&s);
  assert(s.edits.axes[0].action==LHX_AXIS_NONE && s.edits.axes[2].action==LHX_AXIS_NONE);
  assert(s.edits.axes[3].action==LHX_ROLL); /* No duplicate analog role left active. */
  assert(!memcmp(c.buttons,s.edits.buttons,sizeof c.buttons));
  assert(s.edits.axes[4].action==LHX_AXIS_FIRE && s.edits.axes[5].action==LHX_AXIS_FIRE);
  lhx_controller_setup_close(&s); detach(joy);
}
static void save_only(const char *path,int success) {
  LhxControls c; lhx_controls_defaults(&c);
  c.buttons[SDL_CONTROLLER_BUTTON_A]=LHX_FIRE;
  c.axes[SDL_CONTROLLER_AXIS_LEFTY].direction=-1;
  c.deadzone=7500;
  strcpy(c.controller_guid,"00112233445566778899aabbccddeeff");
  /* Maximum serialized identity still fits the parser's line buffer. */
  memset(c.controller_serial,'a',sizeof c.controller_serial-1);
  c.show_controller_setup=1;
  char error[256]; assert(lhx_controls_save(&c,path,error,sizeof error)==success);
  if (success) {
    LhxControls out; assert(lhx_controls_load(&out,path,1));
    assert(out.buttons[SDL_CONTROLLER_BUTTON_A]==LHX_FIRE && out.deadzone==7500);
    assert(out.axes[SDL_CONTROLLER_AXIS_LEFTY].direction==-1 && out.show_controller_setup);
    assert(!strcmp(out.controller_serial,c.controller_serial));
  } else assert(*error);
}
static Uint32 drive_window(Uint32 interval,void *data) {
  (void)interval;
  const char *mode=data;
  SDL_Event event; SDL_zero(event); event.type=SDL_KEYDOWN; event.key.keysym.sym=SDLK_s;
  event.key.timestamp=SDL_GetTicks(); event.key.state=SDL_PRESSED; event.key.repeat=0;
  assert(SDL_PushEvent(&event)==1);
  event.key.keysym.sym=SDLK_DOWN; assert(SDL_PushEvent(&event)==1); assert(SDL_PushEvent(&event)==1);
  event.key.keysym.sym=SDLK_RIGHT; assert(SDL_PushEvent(&event)==1);
  SDL_zero(event); event.common.timestamp=SDL_GetTicks();
  if (!strcmp(mode,"window-cancel")) event.type=SDL_QUIT;
  else {
    event.type=SDL_MOUSEBUTTONDOWN; event.button.button=SDL_BUTTON_LEFT;
    event.button.state=SDL_PRESSED; event.button.clicks=1;
    event.button.x=!strcmp(mode,"window-save")?50:500; event.button.y=780;
  }
  assert(SDL_PushEvent(&event)==1); return 0;
}
static void window_loop(const char *mode,const char *path) {
  assert(!SDL_WasInit(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS));
  LhxControls config,original,loaded; assert(lhx_controls_load(&config,path,1)); original=config;
  char *before=contents(path);
  assert(SDL_AddTimer(150,drive_window,(void *)mode));
  int result=lhx_controller_setup_run(&config,path);
  assert(result==(!strcmp(mode,"window-cancel")?0:1));
  if (!strcmp(mode,"window-cancel")) {
    assert(!memcmp(&config,&original,sizeof config)); unchanged(path,before);
  } else {
    assert(config.deadzone==original.deadzone+500);
    if (!strcmp(mode,"window-save")) {
      assert(lhx_controls_load(&loaded,path,1)); assert(loaded.deadzone==config.deadzone);
    } else unchanged(path,before);
  }
  assert(!SDL_WasInit(SDL_INIT_AUDIO));
  assert(!SDL_WasInit(SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS));
  free(before);
}
int main(int argc,char **argv) {
  assert(argc>=3);
  assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
    "0xfeed/0xb010,0xfeed/0xb011,0xfeed/0xb012,0xfeed/0xb013,0xfeed/0xb014,0xfeed/0xb015",SDL_HINT_OVERRIDE));
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  int window_mode=!strncmp(argv[1],"window-",7);
  assert(!SDL_Init(window_mode?SDL_INIT_TIMER:
    SDL_INIT_VIDEO|SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS|SDL_INIT_TIMER));
  if (!strcmp(argv[1],"ui")) { assert(argc==4); integration(argv[2],argv[3]); }
  else if (!strcmp(argv[1],"native")) { assert(argc==4); native(argv[2],argv[3]); }
  else if (!strcmp(argv[1],"preserve")) preserve(argv[2]);
  else if (!strcmp(argv[1],"save")) save_only(argv[2],1);
  else if (!strcmp(argv[1],"bad-save")) save_only(argv[2],0);
  else if (!strncmp(argv[1],"window-",7)) window_loop(argv[1],argv[2]);
  else assert(0);
  SDL_Quit(); return 0;
}
