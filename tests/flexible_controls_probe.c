/* Isolated SDL virtual hardware. No physical input, desktop window or audio. */
#include "controller_setup.h"
#include "gamepad.h"
#include "lifted.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static LhxControllerSetup setup;
static CPU cpu;
static Uint64 now=1000000;
static unsigned char down[65536];
static unsigned makes[65536];
static SDL_Joystick *stick,*pedals,*box,*hotas,*pseudo;
static void key(unsigned scan,int release) { down[scan]=!release;if (!release) ++makes[scan]; }
static SDL_Joystick *attach(int product,const char *name,int axes,int buttons,int hats) {
  SDL_VirtualJoystickDesc d={0};d.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;d.type=SDL_JOYSTICK_TYPE_UNKNOWN;
  d.vendor_id=0xfeed;d.product_id=product;d.name=name;d.naxes=axes;d.nbuttons=buttons;d.nhats=hats;
  int i=SDL_JoystickAttachVirtualEx(&d);assert(i>=0);assert(!SDL_IsGameController(i));return SDL_JoystickOpen(i);
}
static void detach(SDL_Joystick *j) {
  SDL_JoystickID id=SDL_JoystickInstanceID(j);int index=-1;
  for (int i=0;i<SDL_NumJoysticks();++i) if (SDL_JoystickGetDeviceInstanceID(i)==id) index=i;
  assert(index>=0);assert(!SDL_JoystickDetachVirtual(index));SDL_JoystickClose(j);
}
static void tick(void) {
  SDL_JoystickUpdate();SDL_Event e;while (SDL_PollEvent(&e)) lhx_controller_setup_event(&setup,&e);lhx_controller_setup_tick(&setup);
}
static void press(SDL_Keycode k) { SDL_Event e;SDL_zero(e);e.type=SDL_KEYDOWN;e.key.state=SDL_PRESSED;e.key.repeat=0;e.key.keysym.sym=k;lhx_controller_setup_event(&setup,&e); }
static void click(int x,int y) { SDL_Event e;SDL_zero(e);e.type=SDL_MOUSEBUTTONDOWN;e.button.state=SDL_PRESSED;e.button.clicks=1;e.button.button=SDL_BUTTON_LEFT;e.button.x=x;e.button.y=y;lhx_controller_setup_event(&setup,&e); }
static void axis(SDL_Joystick *j,int a,int v) { assert(!SDL_JoystickSetVirtualAxis(j,a,v));tick(); }
static void button(SDL_Joystick *j,int b,int v) { assert(!SDL_JoystickSetVirtualButton(j,b,v));tick(); }
static void game(void) {
  SDL_JoystickUpdate();SDL_Event e;while (SDL_PollEvent(&e)) lhx_gamepad_event(&e);now+=1000;lhx_gamepad_poll(now);lhx_gamepad_apply(&cpu);
}
static void gaxis(SDL_Joystick *j,int a,int v) { assert(!SDL_JoystickSetVirtualAxis(j,a,v));game(); }
static void gbutton(SDL_Joystick *j,int b,int v) { assert(!SDL_JoystickSetVirtualButton(j,b,v));game(); }
static void add(int action,SDL_Joystick *j,int axis_input,int input,int value) {
  setup.row=action;press(SDLK_a);assert(setup.capture);
  if (axis_input) axis(j,input,value);else button(j,input,1);
  assert(!setup.capture && setup.flex.page==2);
}
static void save_image(const char *directory,const char *name) {
  SDL_Surface *surface=SDL_CreateRGBSurfaceWithFormat(0,1200,840,32,SDL_PIXELFORMAT_ARGB8888);assert(surface);
  SDL_Renderer *r=SDL_CreateSoftwareRenderer(surface);assert(r);
  lhx_controller_setup_draw(&setup,r,"isolated-test.ini");SDL_RenderPresent(r);
  char path[1024];snprintf(path,sizeof path,"%s/%s.bmp",directory,name);assert(!SDL_SaveBMP(surface,path));SDL_DestroyRenderer(r);SDL_FreeSurface(surface);
}
static int enable(SDL_Joystick *j) {
  press(SDLK_d);assert(setup.flex.page==1);int found=-1;
  for (int i=0;i<setup.flex.candidate_count;++i) if (setup.flex.candidates[i].instance==SDL_JoystickInstanceID(j)) found=i;
  assert(found>=0);setup.flex.device_cursor=setup.edits.usb.devices+found;press(SDLK_RETURN);int d=-1;
  for (int i=0;i<setup.edits.usb.devices;++i) if (lhx_usb_same_identity(&setup.edits.usb.device[i],&setup.flex.candidates[found].identity)) d=i;
  press(SDLK_d);return d;
}
static void configure(void) { lhx_gamepad_close();lhx_gamepad_configure(&setup.edits);lhx_gamepad_init(key);game(); }
static unsigned char *file_bytes(const char *path,size_t *size) {
  FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));long n=ftell(f);assert(n>=0);rewind(f);
  unsigned char *bytes=malloc((size_t)n+1);assert(bytes);assert(fread(bytes,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return bytes;
}
static int power(void) { return (int32_t)mem_read32(&cpu,0x4000,0x100+0x21c); }
static void power_state(void) {
  cpu.ds=0x2000;mem_write16(&cpu,cpu.ds,0xf626,0x100);mem_write16(&cpu,cpu.ds,0xf628,0x4000);
  mem_write16(&cpu,0x4000,0x340,100); /* off+0x240 */
  mem_write32(&cpu,0x4000,0x320,25600);mem_write32(&cpu,0x4000,0x324,0);mem_write32(&cpu,0x4000,0x376,1);
  mem_write32(&cpu,0x4000,0x31c,0);mem_write8(&cpu,0x4000,0x440,0);mem_write8(&cpu,cpu.ds,0xd863,0);
}
static void stable(SDL_Joystick *j,int a,int v) { axis(j,a,v);for (int i=0;i<14;++i) { SDL_Delay(20);tick(); } }
static void main_ui(const char *path,const char *evidence) {
  size_t initial_size=0;unsigned char *initial=file_bytes(path,&initial_size);
  LhxControls c;assert(lhx_controls_load(&c,path,1));lhx_controller_setup_init(&setup,&c);
  assert(!setup.flex.enabled);save_image(evidence,"legacy-entry");
  /* Bounded, separate hit rectangles; right click cannot opt in. */
  click(1000,35);assert(setup.settings && !setup.flex.enabled);click(1000,35);assert(!setup.settings);
  click(600,35);assert(setup.flex.import_prompt);press(SDLK_ESCAPE);assert(!setup.flex.enabled);
  click(600,35);press(SDLK_e);assert(setup.edits.usb.version==2 && !setup.edits.usb.devices);
  int sd=enable(stick),pd=enable(pedals),bd=enable(box),hd=enable(hotas);assert(sd==0 && pd==1 && bd==2 && hd==3);
  /* Repeated enabling uses the original profile and keeps its bindings. */
  assert(enable(stick)==sd && setup.edits.usb.devices==4);
  press(SDLK_d);save_image(evidence,"devices");press(SDLK_d);
  setup.row=USB_ROLL;press(SDLK_a);button(pseudo,0,1);assert(setup.capture);button(pseudo,0,0);press(SDLK_ESCAPE);
  /* Slow movement is visible before the first binding; actual axis count only. */
  for (int v=0;v<12000;v+=655) axis(stick,0,v);
  assert(setup.flex.signals);press(SDLK_m);save_image(evidence,"empty-device-live");
  axis(stick,0,0);add(USB_ROLL,stick,1,0,25000);press(SDLK_RETURN);assert(setup.edits.usb.bindings==1);
  add(USB_ROLL,stick,1,1,-25000);press(SDLK_RETURN);assert(setup.edits.usb.bindings==2);
  setup.flex.binding=0;press(SDLK_e);press(SDLK_i);press(SDLK_RETURN);assert(setup.edits.usb.bindings==2);
  press(SDLK_DELETE);assert(setup.edits.usb.bindings==1);
  add(USB_RUDDER,pedals,1,0,32767);press(SDLK_RETURN);
  /* Replay the measured HOTAS first packet, without physical hardware. SDL
     marks all eight initial -32768 positions known before one packet updates
     [32,32,-643,32767,32767,128,128,128]. Axis1 must not win by event order. */
  for (int a=0;a<8;++a) assert(!SDL_JoystickSetVirtualAxis(hotas,a,-32768));tick();
  setup.row=USB_THROTTLE;press(SDLK_a);assert(setup.capture);
  int first_packet[8]={32,32,-643,32767,32767,128,128,128};
  for (int a=0;a<8;++a) assert(!SDL_JoystickSetVirtualAxis(hotas,a,first_packet[a]));tick();
  assert(setup.capture && setup.flex.page==0 && strstr(setup.status,"Several axes"));
  axis(hotas,2,20000);assert(!setup.capture && setup.flex.page==2 && setup.flex.proposal.input==2);
  press(SDLK_ESCAPE);
  /* A quick button down/up pair still captures without needing a poll hold. */
  setup.row=USB_FIRE;press(SDLK_a);SDL_Event quick;SDL_zero(quick);quick.type=SDL_JOYBUTTONDOWN;
  quick.jbutton.which=SDL_JoystickInstanceID(stick);quick.jbutton.button=2;quick.jbutton.timestamp=SDL_GetTicks();lhx_flex_event(&setup,&quick);
  quick.type=SDL_JOYBUTTONUP;lhx_flex_event(&setup,&quick);assert(setup.flex.page==2 && !setup.flex.proposal.axis);press(SDLK_ESCAPE);
  /* Native raw index2 is selected by observed input, not a guessed HOTAS role. */
  add(USB_THROTTLE,hotas,1,2,-32768);save_image(evidence,"behavior-direct");press(SDLK_RETURN);
  int throttle_binding=setup.edits.usb.bindings-1;
  button(box,0,0);add(USB_THROTTLE,box,0,0,1);
  while (setup.flex.proposal.behavior!=USB_PRESET) press(SDLK_b);
  for (int n=0;n<13;++n) press(SDLK_DOWN);assert(setup.flex.proposal.percent==37);press(SDLK_RETURN);button(box,0,0);
  add(USB_FIRE,stick,0,0,1);press(SDLK_RETURN);button(stick,0,0);
  add(USB_FIRE,box,0,0,1);assert(setup.flex.collisions==1);press(SDLK_RETURN);assert(setup.flex.page==2);save_image(evidence,"collision");press(SDLK_r);press(SDLK_RETURN);button(box,0,0);
  assert(setup.edits.usb.bindings==6);
  char names[1024];lhx_flex_bindings(&setup,USB_FIRE,names,sizeof names);assert(strstr(names,"D1") && strstr(names,"D3"));
  setup.row=USB_THROTTLE;setup.flex.binding=0;press(SDLK_c);assert(setup.flex.page==3 && setup.flex.calibration.lever);
  /* OLD stable samples cannot calibrate a freshly moved endpoint. */
  stable(hotas,2,0);axis(hotas,2,32000);press(SDLK_1);assert(!(setup.flex.cal_bits&1));
  stable(hotas,2,32767);press(SDLK_1);assert(setup.flex.cal_bits&1);
  stable(hotas,2,-32768);press(SDLK_3);assert((setup.flex.cal_bits&5)==5);save_image(evidence,"calibration-lever");press(SDLK_RETURN);
  assert(setup.flex.page==0);LhxCalibration saved=setup.edits.usb.device[hd].calibration[2];assert(saved.set && saved.lever && saved.invert);
  assert(lhx_calibrated(&saved,32767,1)==0 && lhx_calibrated(&saved,-32768,1)==32767);
  press(SDLK_c);stable(hotas,2,0);press(SDLK_1);stable(hotas,2,0);press(SDLK_3);press(SDLK_RETURN);assert(setup.flex.page==3);
  assert(!memcmp(&saved,&setup.edits.usb.device[hd].calibration[2],sizeof saved));press(SDLK_ESCAPE);
  press(SDLK_c);for (int i=0;i<14;++i) { axis(hotas,2,i%2?2000:-2000);SDL_Delay(20); }press(SDLK_1);assert(!setup.flex.cal_bits);press(SDLK_ESCAPE);
  /* Calibrate centered pedal axis with explicit neutral and inversion. */
  setup.row=USB_RUDDER;setup.flex.binding=0;press(SDLK_c);assert(!setup.flex.calibration.lever);
  stable(pedals,0,-25000);press(SDLK_1);stable(pedals,0,1000);press(SDLK_2);stable(pedals,0,28000);press(SDLK_3);press(SDLK_i);
  save_image(evidence,"calibration-centered");press(SDLK_RETURN);assert(setup.flex.page==0);
  LhxCalibration centered=setup.edits.usb.device[pd].calibration[0];assert(centered.set && centered.center==1000);
  assert(lhx_calibrated(&centered,1000,0)==0 && lhx_calibrated(&centered,-25000,0)==32767 && lhx_calibrated(&centered,28000,0)==-32767);
  setup.row=USB_FIRE;setup.flex.binding=0;save_image(evidence,"bindings");
  /* Held input must release, stale events and focus loss cannot create bindings. */
  button(stick,1,1);press(SDLK_a);assert(setup.capture);tick();assert(setup.capture);button(stick,1,0);button(stick,1,1);assert(setup.flex.page==2);press(SDLK_ESCAPE);button(stick,1,0);
  press(SDLK_a);SDL_Event stale;SDL_zero(stale);stale.type=SDL_JOYBUTTONDOWN;stale.jbutton.which=SDL_JoystickInstanceID(stick);stale.jbutton.button=2;stale.jbutton.timestamp=setup.flex.capture_time-1;lhx_flex_event(&setup,&stale);assert(setup.capture);
  SDL_Event focus;SDL_zero(focus);focus.type=SDL_WINDOWEVENT;focus.window.event=SDL_WINDOWEVENT_FOCUS_LOST;lhx_flex_event(&setup,&focus);assert(!setup.capture);focus.window.event=SDL_WINDOWEVENT_FOCUS_GAINED;lhx_flex_event(&setup,&focus);
  /* Disabled pseudo input is absent until deliberate enable. */
  assert(setup.edits.usb.devices==4);assert(enable(pseudo)==4);assert(setup.flex.live.input[4].raw);press(SDLK_d);setup.flex.device_cursor=4;press(SDLK_RETURN);press(SDLK_d);assert(!setup.edits.usb.device[4].enabled);
  /* Space needs explicit footer focus; wheel actually selects one binding. */
  setup.result=LHX_SETUP_PENDING;press(SDLK_SPACE);assert(setup.result==LHX_SETUP_PENDING);
  press(SDLK_TAB);press(SDLK_SPACE);assert(setup.result==LHX_SETUP_SAVE);setup.result=LHX_SETUP_PENDING;
  setup.row=USB_FIRE;setup.flex.binding=0;setup.flex.footer_focus=0;
  SDL_Event wheel;SDL_zero(wheel);wheel.type=SDL_MOUSEWHEEL;wheel.wheel.y=-1;
#if SDL_VERSION_ATLEAST(2,26,0)
  wheel.wheel.mouseY=600;lhx_flex_event(&setup,&wheel);assert(setup.flex.binding==1 && setup.row==USB_FIRE);
#endif
  press(SDLK_e);int direction=setup.flex.proposal.direction;press(SDLK_i);assert(setup.flex.proposal.direction==direction);press(SDLK_ESCAPE);
  LhxControls out=c;setup.result=LHX_SETUP_SESSION;assert(lhx_controller_setup_accept(&setup,path,&out)==1 && out.usb.bindings==6);
  size_t session_size=0;unsigned char *session=file_bytes(path,&session_size);assert(initial_size==session_size && !memcmp(initial,session,session_size));free(initial);free(session);
  setup.result=LHX_SETUP_SAVE;assert(lhx_controller_setup_accept(&setup,"/nonexistent/lhx/settings.ini",&out)==-1 && setup.result==LHX_SETUP_PENDING);
  setup.result=LHX_SETUP_SAVE;assert(lhx_controller_setup_accept(&setup,path,&out)==1);
  LhxControls loaded;assert(lhx_controls_load(&loaded,path,1));assert(loaded.usb.bindings==6 && loaded.usb.devices==5);assert(loaded.usb.device[hd].calibration[2].invert);
  setup.result=LHX_SETUP_CANCEL;out=c;assert(!lhx_controller_setup_accept(&setup,path,&out));assert(!memcmp(&out,&c,sizeof c));
  /* Snapshot the exact expected throttle binding after the independent edits. */
  assert(setup.edits.usb.binding[throttle_binding].device==hd);
  puts("UI add/edit/remove/collision, device enable, fresh capture, explicit calibration and Save/Session/Cancel passed");
}
static void runtime(void) {
  power_state();axis(hotas,2,32767);button(box,0,0);button(stick,0,0);configure();assert(power()==0);
  /* Saved reversed lever: +32767 means0%, -32768 means100%. */
  gaxis(hotas,2,-32768);assert(power()==25600);
  gaxis(hotas,2,-32110);assert(power()==25344); /* displayed 1% step */
  gaxis(hotas,2,0);assert(power()==12800);gbutton(box,0,1);assert(power()==9472 && down[0x39]);gbutton(box,0,0);
  unsigned fire=makes[0x39];gbutton(stick,0,1);gbutton(box,0,1);gbutton(stick,0,0);assert(down[0x39]);gbutton(box,0,0);assert(!down[0x39]);assert(makes[0x39]==fire+1);
  /* Preset wins while axis idle, tiny raw noise does not reclaim it. */
  for (int i=0;i<100;++i) gaxis(hotas,2,i%2?50:-50);assert(power()==9472);
  /* Keyboard adjustment is represented by the resulting original target here;
     the full lifted-flight probe separately drives real keyboard and axis input. */
  mem_write32(&cpu,0x4000,0x31c,16384);game();assert(power()==16384);
  gaxis(hotas,2,32767);assert(power()==0);
  for (int v=32000;v>=-32768;v-=655) gaxis(hotas,2,v);gaxis(hotas,2,-32768);assert(power()==25600);
  /* Original input bounds and availability are consumed on the current request. */
  mem_write32(&cpu,0x4000,0x320,14000);gaxis(hotas,2,32767);gaxis(hotas,2,-32768);assert(power()==14000);
  mem_write8(&cpu,0x4000,0x440,1);mem_write32(&cpu,0x4000,0x334,12000);gaxis(hotas,2,32767);gaxis(hotas,2,-32768);assert(power()==12000);
  mem_write32(&cpu,0x4000,0x376,0);gaxis(hotas,2,32767);assert(power()==0);gaxis(hotas,2,-32768);assert(power()==0);
  mem_write32(&cpu,0x4000,0x376,1);mem_write8(&cpu,cpu.ds,0xd863,1);gaxis(hotas,2,32767);gaxis(hotas,2,-32768);assert(power()==0);mem_write8(&cpu,cpu.ds,0xd863,0);
  mem_write32(&cpu,0x4000,0x324,500);gaxis(hotas,2,32767);assert(power()==500);
  mem_write32(&cpu,0x4000,0x342,123456);gaxis(hotas,2,0);assert(mem_read32(&cpu,0x4000,0x342)==0);
  /* Focus reseeds position and held buttons; no snap or held press. */
  lhx_gamepad_focus(0);assert(!down[0x39]);gaxis(hotas,2,-32768);int before=power();lhx_gamepad_focus(1);game();assert(power()==before);
  gaxis(hotas,2,0);assert(power()==12000); /* alternate upper bound */
  /* Disconnect one Fire source while another is held. */
  gbutton(box,0,1);gbutton(stick,0,1);detach(box);box=NULL;game();assert(down[0x39]);gbutton(stick,0,0);assert(!down[0x39]);
  gaxis(hotas,2,32767);before=power();detach(hotas);hotas=NULL;game();hotas=attach(0xd004,"Isolated HOTAS",8,15,1);gaxis(hotas,2,-32768);assert(power()==before);
  /* Detached unique devices can return in a different SDL enumeration order. */
  game();gaxis(hotas,2,32767);gaxis(hotas,2,-32768);assert(power()==12000);
  lhx_gamepad_close();puts("Runtime multi-device union, direct throttle, guarded one-shot override, focus/reconnect and signed axes passed");
}
static void model(void) {
  LhxUsbProfile p={.version=2,.devices=2,.bindings=2};assert(lhx_usb_identity(&p.device[0],stick));assert(lhx_usb_identity(&p.device[1],pedals));p.device[0].enabled=p.device[1].enabled=1;
  p.binding[0]=(LhxUsbBinding){0,1,0,USB_ROLL,USB_ANALOG,1,50,16000};p.binding[1]=(LhxUsbBinding){1,1,0,USB_ROLL,USB_ANALOG,1,50,16000};
  LhxUsbRuntime r;lhx_usb_runtime_init(&r);lhx_usb_refresh(&r,&p);lhx_usb_poll(&r,&p);
  SDL_JoystickSetVirtualAxis(stick,0,32767);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.analog[0]==32767);
  SDL_JoystickSetVirtualAxis(pedals,0,-32768);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.analog[0]==-32767);
  for (int v=32760;v<=32767;++v) { SDL_JoystickSetVirtualAxis(stick,0,v);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.analog[0]==-32767); }
  lhx_usb_runtime_close(&r);
  /* Re-enable at capacity reuses the existing record; no 17th profile. */
  LhxUsbCandidate candidates[64];int count=lhx_usb_candidates(candidates,64);int ci=-1;
  for (int i=0;i<count;++i) if (candidates[i].instance==SDL_JoystickInstanceID(stick)) ci=i;
  assert(ci>=0);LhxUsbProfile cap=p;cap.devices=16;cap.device[0].enabled=0;char error[256];
  assert(lhx_usb_enable(&cap,&candidates[ci],0,error,sizeof error)==0 && cap.devices==16);
  /* Runtime startup: the same initial all-min -> coherent packet must NOT
     turn a stationary physical throttle into a target write on stick motion. */
  p=(LhxUsbProfile){.version=2,.devices=1,.bindings=1};lhx_usb_identity(&p.device[0],hotas);p.device[0].enabled=1;
  p.binding[0]=(LhxUsbBinding){0,1,2,USB_THROTTLE,USB_DIRECT,1,50,16000};
  for (int a=0;a<8;++a) SDL_JoystickSetVirtualAxis(hotas,a,-32768);SDL_JoystickUpdate();lhx_usb_refresh(&r,&p);lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==-1);
  int packet[8]={32,32,-643,32767,32767,128,128,128};
  for (int a=0;a<8;++a) SDL_JoystickSetVirtualAxis(hotas,a,packet[a]);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==-1);
  SDL_JoystickSetVirtualAxis(hotas,2,20000);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==81);
  /* After initialization, intentional simultaneous movement remains usable. */
  SDL_JoystickSetVirtualAxis(hotas,0,20000);SDL_JoystickSetVirtualAxis(hotas,2,32767);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==100);
  detach(hotas);hotas=attach(0xd004,"Isolated HOTAS",8,15,1);
  for (int a=0;a<8;++a) SDL_JoystickSetVirtualAxis(hotas,a,-32768);SDL_JoystickUpdate();lhx_usb_refresh(&r,&p);lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==-1);
  for (int a=0;a<8;++a) SDL_JoystickSetVirtualAxis(hotas,a,packet[a]);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==-1);
  SDL_JoystickSetVirtualAxis(hotas,2,32767);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(lhx_usb_take_throttle(&r)==100);
  lhx_usb_runtime_close(&r);
  puts("Measured eight-axis startup/reconnect batch reseeded without throttle snap; isolated movement reclaimed control");
  /* All eight native axes incl6/7, negative and positive thresholds, and hats. */
  p=(LhxUsbProfile){.version=2,.devices=1,.bindings=9};lhx_usb_identity(&p.device[0],hotas);p.device[0].enabled=1;
  for (int a=0;a<8;++a) { p.binding[a]=(LhxUsbBinding){0,1,a,USB_FIRE,USB_HOLD,a%2?-1:1,50,16000};SDL_JoystickSetVirtualAxis(hotas,a,0); }
  p.binding[8]=(LhxUsbBinding){0,0,64+3,USB_FIRE,USB_HOLD,1,50,16000};SDL_JoystickUpdate();lhx_usb_refresh(&r,&p);lhx_usb_poll(&r,&p);
  for (int a=0;a<8;++a) { SDL_JoystickSetVirtualAxis(hotas,a,a%2?-32768:32767);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.active[LHX_FIRE]);SDL_JoystickSetVirtualAxis(hotas,a,0);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(!r.active[LHX_FIRE]); }
  SDL_JoystickSetVirtualHat(hotas,0,SDL_HAT_LEFT);SDL_JoystickUpdate();lhx_usb_poll(&r,&p);assert(r.active[LHX_FIRE]);SDL_JoystickSetVirtualHat(hotas,0,0);
  /* Identical serial-less units are inactive until explicitly reassigned. */
  SDL_Joystick *twin=attach(0xd004,"Isolated HOTAS",8,15,1);lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_AMBIGUOUS && !r.input[0].joy);
  detach(twin);lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_AMBIGUOUS);
  twin=attach(0xd004,"Isolated HOTAS",8,15,1);
  /* The actual Devices UI explicitly reassigns the selected saved profile. */
  tick();press(SDLK_d);setup.flex.device_cursor=3;press(SDLK_r);assert(setup.flex.reassign==3);
  int candidate=-1;for (int n=0;n<setup.flex.candidate_count;++n) if (setup.flex.candidates[n].instance==SDL_JoystickInstanceID(hotas)) candidate=n;
  assert(candidate>=0);setup.flex.device_cursor=setup.edits.usb.devices+candidate;press(SDLK_RETURN);
  assert(setup.flex.live.status[3]==USB_CONNECTED && setup.edits.usb.device[3].session_only);press(SDLK_d);
  p.device[0].occurrence=SDL_JoystickInstanceID(hotas);lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_CONNECTED);
  detach(hotas);hotas=attach(0xd004,"Isolated HOTAS",8,15,1);lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_AMBIGUOUS && !r.input[0].joy);
  p.device[0].occurrence=SDL_JoystickInstanceID(hotas);lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_CONNECTED);lhx_usb_runtime_close(&r);detach(twin);
  /* Serial-bearing profiles never fall back to GUID alone. */
  strcpy(p.device[0].serial,"616263");p.device[0].occurrence=-1;lhx_usb_refresh(&r,&p);assert(r.status[0]==USB_OFFLINE);lhx_usb_runtime_close(&r);
  puts("Eight axes / hats, deterministic takeover, identical-device reassignment and serial mismatch passed");
}
int main(int argc,char **argv) {
  assert(argc>=3);
  if (!strcmp(argv[1],"bad")) { LhxControls c,old;memset(&c,0xa5,sizeof c);old=c;assert(!lhx_controls_load(&c,argv[2],1));assert(!memcmp(&c,&old,sizeof c));return 0; }
  if (!strcmp(argv[1],"roundtrip")) { LhxControls c;char err[256];assert(lhx_controls_load(&c,argv[2],1));assert(lhx_controls_save(&c,argv[2],err,sizeof err));return 0; }
  assert(argc==4);SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,"0xfeed/0xd001,0xfeed/0xd002,0xfeed/0xd003,0xfeed/0xd004,0xfeed/0xd005",SDL_HINT_OVERRIDE);
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");assert(!SDL_Init(SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS));
  stick=attach(0xd001,"Isolated stick",2,4,1);pedals=attach(0xd002,"Single-axis pedals",1,0,0);box=attach(0xd003,"Zero-axis button box",0,8,0);hotas=attach(0xd004,"Isolated HOTAS",8,15,1);pseudo=attach(0xd005,"Pseudo media keyboard",1,19,1);
  cpu.mem=calloc(1,MEM_SIZE);assert(cpu.mem);main_ui(argv[2],argv[3]);runtime();model();
  lhx_controller_setup_close(&setup);detach(stick);detach(pedals);detach(hotas);detach(pseudo);SDL_Quit();free(cpu.mem);puts("Flexible USB controls passed");return 0;
}
