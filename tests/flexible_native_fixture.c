/* Test-only virtual device and wrappers. No simulation fields are modified. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <SDL.h>
#include <dlfcn.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "gamepad.h"
#include "lifted.h"
static SDL_Joystick *virtual_input;
static int initialized,stage,seen[12];
static Uint64 last_now;
int SDL_InitSubSystem(Uint32 flags) {
  int (*real_init)(Uint32)=dlsym(RTLD_NEXT,"SDL_InitSubSystem");assert(real_init);
  int result=real_init(flags);
  if (!result && !initialized && (flags & SDL_INIT_GAMECONTROLLER)) {
    initialized=1;SDL_VirtualJoystickDesc d={0};d.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    d.type=SDL_JOYSTICK_TYPE_UNKNOWN;d.naxes=1;d.nbuttons=3;d.vendor_id=0xfeed;d.product_id=0xd100;d.name="Lifted native USB fixture";
    int index=SDL_JoystickAttachVirtualEx(&d);assert(index>=0 && !SDL_IsGameController(index));virtual_input=SDL_JoystickOpen(index);assert(virtual_input);
    assert(!SDL_JoystickSetVirtualAxis(virtual_input,0,-32768));SDL_JoystickUpdate();
  }
  return result;
}
#ifdef FIXTURE_IDENTITY_MAIN
int main(void) {
  assert(!SDL_Init(0));assert(!SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS));char guid[33];SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(virtual_input),guid,sizeof guid);puts(guid);SDL_Quit();return 0;
}
#else
void __real_lhx_gamepad_poll(uint64_t);
void __real_lhx_gamepad_apply(CPU *);
void __wrap_lhx_gamepad_poll(uint64_t now) {
  last_now=now;
  int desired=now>=56000000?9:now>=54000000?8:now>=52000000?7:now>=51000000?6:now>=50000000?5:now>=48000000?4:now>=46000000?3:now>=44000000?2:now>=42000000?1:0;
  if (desired!=stage) {
    stage=desired;int axis=stage==1 || stage==4 || stage>=9?32767:stage==2 || stage==8?0:stage==3?-32768:32767;
    /* Preset / keyboard phases leave the full lever stationary. */
    if (stage<=4 || stage>=8) SDL_JoystickSetVirtualAxis(virtual_input,0,axis);
    SDL_JoystickSetVirtualButton(virtual_input,0,stage==5);
    SDL_JoystickSetVirtualButton(virtual_input,2,stage==6);
    SDL_JoystickUpdate();
  }
  __real_lhx_gamepad_poll(now);
}
void __wrap_lhx_gamepad_apply(CPU *c) {
  CPU before=*c;__real_lhx_gamepad_apply(c);assert(!memcmp(&before,c,sizeof *c));
  if (!stage || seen[stage]) return;
  int off=mem_read16(c,c->ds,0xf626),seg=mem_read16(c,c->ds,0xf628);
  if (!seg || off>0xfc00) return;
  int actual=(int32_t)mem_read32(c,seg,off+0x21c);
  int expected=stage==2 || stage==8?12800:stage==3?0:stage==5?9472:stage==6?6912:25600;
  if (actual==expected) {
    seen[stage]=1;fprintf(stderr,"[flex-native] stage=%d target=%d expected=%d time=%llu registers-preserved\n",stage,actual,expected,(unsigned long long)last_now);
  }
}
__attribute__((destructor)) static void completed(void) {
  int count=0;for (int n=1;n<=9;++n) count+=seen[n];
  fprintf(stderr,"[flex-native] observed=%d/9 (direct full/half/zero, preset37, decrease, keyboard5, movement reclaim)\n",count);
}
#endif
