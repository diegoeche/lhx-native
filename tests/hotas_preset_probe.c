/* Isolated SDL virtual input. Only metadata is substituted for the known model;
 * the allowlist excludes real 044f devices. Axis/button/hat reports are real SDL. */
#include "controller_setup.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LhxControllerSetup setup;
static const char *platform="Linux";
static unsigned vendor=0x044f,product=0xb68d;
const char *__wrap_SDL_GetPlatform(void) { return platform; }
Uint16 __real_SDL_JoystickGetVendor(SDL_Joystick *);
Uint16 __real_SDL_JoystickGetProduct(SDL_Joystick *);
Uint16 __real_SDL_JoystickGetDeviceVendor(int);
Uint16 __real_SDL_JoystickGetDeviceProduct(int);
static int fixture(SDL_Joystick *j) { return __real_SDL_JoystickGetVendor(j)==0xfeed && __real_SDL_JoystickGetProduct(j)==0xd100; }
static int device_fixture(int i) { return __real_SDL_JoystickGetDeviceVendor(i)==0xfeed && __real_SDL_JoystickGetDeviceProduct(i)==0xd100; }
Uint16 __wrap_SDL_JoystickGetVendor(SDL_Joystick *j) { return fixture(j)?vendor:__real_SDL_JoystickGetVendor(j); }
Uint16 __wrap_SDL_JoystickGetProduct(SDL_Joystick *j) { return fixture(j)?product:__real_SDL_JoystickGetProduct(j); }
Uint16 __wrap_SDL_JoystickGetDeviceVendor(int i) { return device_fixture(i)?vendor:__real_SDL_JoystickGetDeviceVendor(i); }
Uint16 __wrap_SDL_JoystickGetDeviceProduct(int i) { return device_fixture(i)?product:__real_SDL_JoystickGetDeviceProduct(i); }

static SDL_Joystick *attach(int pid,const char *name,int axes,int buttons,int hats) {
  SDL_VirtualJoystickDesc d={0};d.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  d.type=SDL_JOYSTICK_TYPE_UNKNOWN;d.vendor_id=0xfeed;d.product_id=pid;
  d.name=name;d.naxes=axes;d.nbuttons=buttons;d.nhats=hats;
  int i=SDL_JoystickAttachVirtualEx(&d);assert(i>=0);assert(!SDL_IsGameController(i));
  return SDL_JoystickOpen(i);
}
static int index_of(SDL_Joystick *j) {
  for (int i=0;i<SDL_NumJoysticks();++i) if (SDL_JoystickGetDeviceInstanceID(i)==SDL_JoystickInstanceID(j)) return i;
  assert(0);return -1;
}
static void detach(SDL_Joystick *j) { assert(!SDL_JoystickDetachVirtual(index_of(j)));SDL_JoystickClose(j); }
static void press(SDL_Keycode key) {
  SDL_Event e;SDL_zero(e);e.type=SDL_KEYDOWN;e.key.keysym.sym=key;lhx_controller_setup_event(&setup,&e);
}
static void click(int x,int y) {
  SDL_Event e;SDL_zero(e);e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;
  e.button.x=x;e.button.y=y;lhx_controller_setup_event(&setup,&e);
}
static void tick(void) {
  SDL_JoystickUpdate();SDL_Event e;
  while (SDL_PollEvent(&e)) lhx_controller_setup_event(&setup,&e);
  lhx_controller_setup_tick(&setup);
}
static void axis(SDL_Joystick *j,int a,int value) { assert(!SDL_JoystickSetVirtualAxis(j,a,value));tick(); }
static char *bytes(const char *path) {
  FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>=0);rewind(f);
  char *out=calloc((size_t)n+1,1);assert(out);assert(fread(out,1,(size_t)n,f)==(size_t)n);fclose(f);return out;
}
static void unchanged(const char *path,const char *before) { char *after=bytes(path);assert(!strcmp(after,before));free(after); }
static void shot(const char *directory,const char *name) {
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,1200,840,32,SDL_PIXELFORMAT_ARGB8888);assert(surface);
  SDL_Renderer *r=SDL_CreateSoftwareRenderer(surface);assert(r);lhx_controller_setup_draw(&setup,r,"isolated-test.ini");SDL_RenderPresent(r);
  char path[1024];snprintf(path,sizeof path,"%s/%s.bmp",directory,name);assert(!SDL_SaveBMP(surface,path));
  SDL_DestroyRenderer(r);SDL_FreeSurface(surface);
}
static LhxUsbCandidate candidate(SDL_Joystick *j) {
  LhxUsbCandidate list[LHX_USB_CANDIDATES];int n=lhx_usb_candidates(list,LHX_USB_CANDIDATES);assert(n>=0);
  for (int i=0;i<n;++i) if (list[i].instance==SDL_JoystickInstanceID(j)) return list[i];
  assert(0);return list[0];
}
static void names(SDL_Joystick *j,SDL_Joystick *unknown,const char *directory) {
  LhxControllerInput in;assert(lhx_input_open_explicit(&in,index_of(j),0));assert(lhx_input_hotas_one(&in));
  const char *names[]={"Stick left/right","Stick forward/back","Throttle lever","Right toe brake",
    "Left toe brake","Stick twist","Throttle rocker","Pedal rudder"};char label[80];
  for (int a=0;a<8;++a) { lhx_input_axis_label(&in,a,label,sizeof label);assert(strstr(label,names[a]));assert(label[5]=='1'+a); }
  lhx_input_axis_label(&in,8,label,sizeof label);assert(!strcmp(label,"Axis 9"));
  platform="Windows";assert(!lhx_input_hotas_one(&in));lhx_input_axis_label(&in,3,label,sizeof label);assert(!strcmp(label,"Axis 4"));platform="Linux";
  product=0xb68c;assert(!lhx_input_hotas_one(&in));product=0xb68d;
  vendor=0x1234;assert(!lhx_input_hotas_one(&in));vendor=0x044f;
  in.axes=7;assert(!lhx_input_hotas_one(&in));in.axes=8;
  in.raw=0;assert(!lhx_input_hotas_one(&in));lhx_input_axis_label(&in,7,label,sizeof label);assert(!strcmp(label,"Axis 8"));in.raw=1;
  lhx_input_close(&in);
  assert(!lhx_input_hotas_match(1,0x044f,0xb68d,8,14,1));assert(!lhx_input_hotas_match(1,0x044f,0xb68d,8,15,0));
  assert(!lhx_input_hotas_match(1,0x044f,0xb68d,9,15,1));
  assert(lhx_input_open_explicit(&in,index_of(unknown),0));assert(!lhx_input_hotas_one(&in));
  lhx_input_axis_label(&in,0,label,sizeof label);assert(!strcmp(label,"Axis 1"));lhx_input_close(&in);
  LhxControls c;lhx_controls_defaults(&c);lhx_controls_select_joystick(&c,j);c.input_mode=LHX_INPUT_JOYSTICK;
  c.raw_axes[7]=(LhxAxisBinding){LHX_RUDDER,1};lhx_controller_setup_init(&setup,&c);
  lhx_controller_setup_bindings(&setup,LHX_SETUP_RUDDER_ROW,label,sizeof label);assert(strstr(label,"Axis 8 Pedal rudder"));
  shot(directory,"legacy-names");setup.row=LHX_SETUP_RUDDER_ROW;press(SDLK_b);axis(j,7,25000);
  assert(setup.conflict);shot(directory,"legacy-capture-names");press(SDLK_ESCAPE);
  setup.monitor_page=0;assert(strstr(setup.motion[0].input,"Axis 8 Pedal rudder"));shot(directory,"legacy-event-names");
  axis(j,7,0);lhx_controller_setup_close(&setup);
  LhxUsbProfile p={.version=2,.devices=1};assert(lhx_usb_identity(&p.device[0],j));
  LhxUsbBinding b={.device=0,.axis=1,.input=3};lhx_usb_physical(&p,&b,label,sizeof label);
  assert(strstr(label,"Axis 4") && !strstr(label,"toe")); /* offline: do not guess capabilities */
  puts("Exact physical names, right/left brakes, platform/mode/VID/PID/capability/unknown/offline guards passed");
}
static void fresh(SDL_Joystick *j,const char *path,const char *directory) {
  char *before=bytes(path);LhxControls c,out,reloaded;assert(lhx_controls_load(&c,path,1));
  lhx_controller_setup_init(&setup,&c);assert(lhx_flex_hotas_offer(&setup));shot(directory,"initial-offer");
  SDL_Event e;SDL_zero(e);e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_RIGHT;e.button.x=600;e.button.y=35;
  lhx_controller_setup_event(&setup,&e);assert(!setup.flex.enabled);
  SDL_zero(e);e.type=SDL_KEYDOWN;e.key.keysym.sym=SDLK_h;e.key.repeat=1;lhx_controller_setup_event(&setup,&e);assert(!setup.flex.enabled);
  click(600,35);assert(setup.flex.enabled && setup.flex.page==4 && setup.edits.usb.devices==1 && setup.edits.usb.bindings==20);
  const int inputs[]={0,1,7,2};
  for (int a=0;a<4;++a) {
    LhxUsbBinding b=setup.edits.usb.binding[a];assert(b.device==0 && b.axis && b.action==a && b.input==inputs[a]);
    assert(b.behavior==(a==3?USB_DIRECT:USB_ANALOG) && b.direction==(a==3?-1:1));
  }
  for (int a=0;a<32;++a) assert(!setup.edits.usb.device[0].calibration[a].set);
  for (int n=4;n<20;++n) { LhxUsbBinding b=setup.edits.usb.binding[n];assert(!b.axis && (b.input<12 || b.input>=64)); }
  assert(!memcmp(c.raw_axes,setup.edits.raw_axes,sizeof c.raw_axes));assert(!memcmp(c.buttons,setup.edits.buttons,sizeof c.buttons));
  shot(directory,"preset-summary");press(SDLK_b);shot(directory,"preset-buttons");press(SDLK_RETURN);assert(!setup.flex.page);
  LhxUsbProfile once=setup.edits.usb;press(SDLK_h);assert(setup.flex.page==4 && !memcmp(&once,&setup.edits.usb,sizeof once));
  assert(setup.flex.hotas.skipped==4 && setup.flex.hotas.buttons_skipped==16);shot(directory,"preset-preserved");
  press(SDLK_RETURN);press(SDLK_DOWN);press(SDLK_DOWN);press(SDLK_DOWN);assert(setup.row==USB_THROTTLE);
  axis(j,2,12345);shot(directory,"preset-action-monitor");
  /* Both known names and signed values survive the two-line live monitor. */
  click(900,450);assert(setup.flex.monitor==2);click(900,450);
  setup.row=USB_RUDDER;setup.flex.binding=0;press(SDLK_e);assert(setup.flex.page==2);shot(directory,"preset-axis-detail");press(SDLK_ESCAPE);
  setup.row=USB_RUDDER;press(SDLK_a);axis(j,7,-25000);assert(setup.flex.page==2);shot(directory,"flex-capture-names");press(SDLK_ESCAPE);
  press(SDLK_h);click(1000,780);out=c;assert(lhx_controller_setup_accept(&setup,path,&out)==0);assert(!memcmp(&out,&c,sizeof c));unchanged(path,before);
  lhx_controller_setup_close(&setup);
  /* Direct page4 acceptance: both keyboard and mouse Save / Session. */
  for (int mode=0;mode<5;++mode) {
    lhx_controller_setup_init(&setup,&c);press(SDLK_h);assert(setup.flex.page==4);
    if (mode==0) press(SDLK_F6);else if (mode==1) click(500,780);else if (mode==2) press(SDLK_F5);
    else if (mode==3) click(50,780);else { press(SDLK_TAB);press(SDLK_RETURN); }
    assert(lhx_controller_setup_accept(&setup,path,&out)==1 && out.usb.version==2 && out.usb.bindings==20);
    if (mode<2) unchanged(path,before);else { assert(lhx_controls_load(&reloaded,path,1));assert(reloaded.usb.bindings==20); }
    lhx_controller_setup_close(&setup);
  }
  assert(lhx_controls_load(&reloaded,path,1));char *saved=bytes(path);lhx_controller_setup_init(&setup,&reloaded);
  press(SDLK_h);assert(setup.flex.page==4 && setup.edits.usb.devices==1 && setup.edits.usb.bindings==20);
  click(50,780);assert(lhx_controller_setup_accept(&setup,path,&out)==1);unchanged(path,saved);
  lhx_controller_setup_close(&setup);free(saved);free(before);
  puts("Initial mouse/keyboard preset, four flight axes, idempotence, cancel, page4 keyboard/mouse Save/Session and reload passed");
}
static void custom(SDL_Joystick *j,SDL_Joystick *other,const char *path,const char *directory) {
  LhxUsbCandidate c=candidate(j);LhxHotasPreset report;char error[256];
  LhxUsbProfile p={.version=2,.devices=2,.bindings=3};assert(lhx_usb_identity(&p.device[0],j));assert(lhx_usb_identity(&p.device[1],other));
  p.device[0].enabled=p.device[1].enabled=1;
  p.device[0].calibration[2]=(LhxCalibration){1,1,-30000,0,31000,1,0};
  p.device[0].calibration[0]=(LhxCalibration){1,0,-29000,100,30000,1,5000};
  p.binding[0]=(LhxUsbBinding){1,1,0,USB_ROLL,USB_ANALOG,-1,50,16000};
  p.binding[1]=(LhxUsbBinding){0,1,7,USB_FIRE,USB_HOLD,-1,50,17000};
  p.binding[2]=(LhxUsbBinding){0,0,1,USB_PAUSE,USB_HOLD,1,50,16000};
  LhxUsbProfile original=p;assert(lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error));
  assert(!memcmp(p.binding,original.binding,3*sizeof p.binding[0]));assert(!memcmp(p.device,original.device,sizeof p.device));
  assert(report.added==2 && report.skipped==2);assert(strstr(report.flight[0],"kept") && strstr(report.flight[2],"control already"));
  assert(strstr(report.button[1],"control already"));
  int found=0;for (int n=0;n<p.bindings;++n) if (p.binding[n].action==USB_THROTTLE) { assert(p.binding[n].direction==1);found=1; }assert(found);
  LhxUsbProfile once=p;assert(lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error));assert(!memcmp(&once,&p,sizeof p));
  LhxControls config;lhx_controls_defaults(&config);config.usb=original;lhx_controller_setup_init(&setup,&config);press(SDLK_h);
  assert(setup.flex.page==4);shot(directory,"custom-preservation");press(SDLK_b);shot(directory,"custom-button-skips");
  char copy[1024];snprintf(copy,sizeof copy,"%s/custom-profile.ini",directory);
  char *body=bytes(path);FILE *file=fopen(copy,"wb");assert(file);assert(fwrite(body,1,strlen(body),file)==strlen(body));assert(!fclose(file));free(body);
  press(SDLK_F5);LhxControls applied,reloaded;assert(lhx_controller_setup_accept(&setup,copy,&applied)==1);
  assert(lhx_controls_load(&reloaded,copy,1));assert(reloaded.usb.devices==2 && reloaded.usb.bindings==applied.usb.bindings);
  assert(!memcmp(reloaded.usb.device[0].calibration,original.device[0].calibration,sizeof original.device[0].calibration));
  assert(!memcmp(reloaded.usb.binding,original.binding,3*sizeof original.binding[0]));lhx_controller_setup_close(&setup);
  p=original;p.device[0].mapped=1;p.binding[1].input=1;once=p;assert(lhx_usb_profile_valid(&p));
  assert(!lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error) && !memcmp(&p,&once,sizeof p));
  p=original;p.device[0].calibration[1]=(LhxCalibration){1,1,-30000,0,31000,0,0};once=p;
  assert(lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error));assert(strstr(report.flight[1],"lever calibration"));
  assert(!memcmp(p.device,once.device,sizeof p.device));
  LhxUsbCandidate invalid=c;invalid.axes=7;once=p;
  assert(!lhx_usb_hotas_preset(&p,&invalid,&report,error,sizeof error) && !memcmp(&p,&once,sizeof p));
  p=(LhxUsbProfile){.version=2,.devices=LHX_USB_DEVICES};
  for (int d=0;d<p.devices;++d) { assert(lhx_usb_identity(&p.device[d],other));p.device[d].guid[0]="0123456789abcdef"[d]; }
  once=p;assert(!lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error) && !memcmp(&p,&once,sizeof p));
  p=original;p.device[0].session_only=1;p.device[0].occurrence=-1;once=p;
  assert(!lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error) && !memcmp(&p,&once,sizeof p));
  lhx_controls_defaults(&config);lhx_controls_select_joystick(&config,j);config.input_mode=LHX_INPUT_JOYSTICK;
  config.raw_axes[0]=(LhxAxisBinding){LHX_ROLL,-1};config.raw_axes[2]=(LhxAxisBinding){LHX_COLLECTIVE,1};config.raw_axes[5]=(LhxAxisBinding){LHX_RUDDER,1};
  config.raw_buttons[0]=LHX_NEXT;config.raw_buttons[64]=LHX_VIEW;
  lhx_controller_setup_init(&setup,&config);press(SDLK_h);assert(setup.flex.page==4);
  assert(!memcmp(config.raw_axes,setup.edits.raw_axes,sizeof config.raw_axes));assert(!memcmp(config.raw_buttons,setup.edits.raw_buttons,sizeof config.raw_buttons));
  assert(setup.flex.hotas.skipped==3);
  int rudder=0,throttle=0;for (int n=0;n<setup.edits.usb.bindings;++n) {
    LhxUsbBinding b=setup.edits.usb.binding[n];if (b.action==USB_RUDDER) { assert(b.input==5);++rudder; }
    if (b.action==USB_THROTTLE) { assert(b.input==2 && b.behavior==USB_RELATIVE);++throttle; }
  }
  assert(rudder==1 && throttle==1);shot(directory,"legacy-custom-preserved");lhx_controller_setup_close(&setup);
  puts("Cross-device custom actions / conflicting controls / calibration / native-mapped and session guards / legacy import preservation passed");
}
static void runtime(SDL_Joystick *j) {
  LhxUsbProfile p={0};LhxHotasPreset report;LhxUsbCandidate c=candidate(j);char error[256];assert(lhx_usb_hotas_preset(&p,&c,&report,error,sizeof error));
  for (int a=0;a<8;++a) assert(!SDL_JoystickSetVirtualAxis(j,a,0));
  assert(!SDL_JoystickSetVirtualAxis(j,2,32767));SDL_JoystickUpdate();
  LhxUsbRuntime r;lhx_usb_runtime_init(&r);lhx_usb_refresh(&r,&p);lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==-1);
  const int raw[]={0,-32768,32767},expected[]={50,100,0};
  for (int n=0;n<3;++n) { assert(!SDL_JoystickSetVirtualAxis(j,2,raw[n]));SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==expected[n]); }
  assert(!SDL_JoystickSetVirtualAxis(j,7,32767));SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.analog[2]==32767);
  assert(!SDL_JoystickSetVirtualAxis(j,7,0));assert(!SDL_JoystickSetVirtualAxis(j,5,-32768));
  assert(!SDL_JoystickSetVirtualAxis(j,3,32767));assert(!SDL_JoystickSetVirtualAxis(j,4,-32768));assert(!SDL_JoystickSetVirtualAxis(j,6,32767));
  SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(!r.analog[2] && lhx_usb_take_throttle(&r)==-1);
  assert(!SDL_JoystickSetVirtualAxis(j,0,-32768));assert(!SDL_JoystickSetVirtualAxis(j,1,32767));
  SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.analog[0]==-32767 && r.analog[1]==32767);
  assert(!SDL_JoystickSetVirtualButton(j,0,1));SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.active[LHX_FIRE]);
  assert(!SDL_JoystickSetVirtualButton(j,0,0));assert(!SDL_JoystickSetVirtualHat(j,0,SDL_HAT_LEFT));
  SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(!r.active[LHX_FIRE] && r.active[LHX_LEFT]);
  lhx_usb_runtime_close(&r);assert(!SDL_JoystickSetVirtualHat(j,0,0));
  puts("Actual virtual-axis runtime: throttle 0/50/100, pedal yaw rather than twist, roll/pitch, fire and hat passed");
}
static void identity(SDL_Joystick *j,SDL_Joystick *unknown,const char *path) {
  LhxControls c;lhx_controls_defaults(&c);SDL_Joystick *twin=attach(0xd100,"Exact known fixture",8,15,1);
  lhx_controller_setup_init(&setup,&c);assert(!lhx_flex_hotas_offer(&setup));press(SDLK_h);
  assert(!setup.flex.enabled && strstr(setup.status,"Several HOTAS"));
  press(SDLK_F2);press(SDLK_e);press(SDLK_d);LhxUsbCandidate chosen=candidate(j);
  int n=-1;for (int i=0;i<setup.flex.candidate_count;++i) if (setup.flex.candidates[i].instance==chosen.instance) n=i;
  assert(n>=0);setup.flex.device_cursor=n;press(SDLK_h);assert(setup.flex.page==4 && setup.edits.usb.devices==1);
  assert(setup.edits.usb.device[0].session_only && setup.edits.usb.device[0].occurrence==chosen.instance);
  lhx_controller_setup_close(&setup);detach(twin);
  product=0xb68c;lhx_controls_select_joystick(&c,unknown);c.input_mode=LHX_INPUT_JOYSTICK;
  lhx_controller_setup_init(&setup,&c);LhxControls before=setup.edits;press(SDLK_h);
  assert(!setup.flex.enabled && !memcmp(&before,&setup.edits,sizeof before));
  setup.result=LHX_SETUP_CANCEL;LhxControls out=c;assert(!lhx_controller_setup_accept(&setup,path,&out));
  lhx_controller_setup_close(&setup);product=0xb68d;
  puts("Ambiguous identical devices require explicit selection; unknown/legacy defaults stay unchanged");
}
int main(int argc,char **argv) {
  assert(argc==4);
  SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0xfeed/0xd100,0xfeed/0xd101",SDL_HINT_OVERRIDE);
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");assert(!SDL_Init(SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS));
  SDL_Joystick *j=attach(0xd100,"Exact known fixture",8,15,1),*unknown=attach(0xd101,"Thrustmaster T.Flight Hotas One",8,15,1);
  if (!strcmp(argv[1],"names")) names(j,unknown,argv[3]);
  else if (!strcmp(argv[1],"fresh")) fresh(j,argv[2],argv[3]);
  else if (!strcmp(argv[1],"custom")) custom(j,unknown,argv[2],argv[3]);
  else if (!strcmp(argv[1],"runtime")) runtime(j);
  else if (!strcmp(argv[1],"identity")) identity(j,unknown,argv[2]);
  else assert(0);
  detach(j);detach(unknown);SDL_Quit();return 0;
}
