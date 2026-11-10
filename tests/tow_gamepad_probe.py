"""Reproduce controller/TOW integration with an isolated SDL virtual pad.

Default mode prints the observed result. --require-fixed is a regression gate:
held RT must not send a second fire action during manual guidance, and analog
input must reach the missile after the original main loop restores its axes.
No original executable/assets or shared build directory are required.
"""

import argparse
import json
import os
from pathlib import Path
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]

SOURCE = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "gamepad.h"
#include "lifted.h"

static unsigned fire_makes;
static void key(unsigned scan,int release) {
  if(scan==0x39 && !release) fire_makes++;
}
static void poll(uint64_t now) {
  SDL_JoystickUpdate();
  SDL_Event e;
  while(SDL_PollEvent(&e)) lhx_gamepad_event(&e);
  lhx_gamepad_poll(now);
}
static void manual(CPU *cpu,int active) {
  mem_write8(cpu,cpu->ds,0xd855,active);
  mem_write16(cpu,cpu->ds,0x128,active?0xc579:0);
  mem_write16(cpu,cpu->ds,0x120,0x400);
  mem_write16(cpu,cpu->ds,0x122,0x3000);
  mem_write16(cpu,cpu->ds,0xea,0x3000);
  mem_write16(cpu,cpu->ds,0xf35a,0x141c);
  mem_write16(cpu,cpu->ds,0xf368,1);
  mem_write16(cpu,cpu->ds,0xc579,active?0x141c:0);
  mem_write16(cpu,cpu->ds,0xc57b,0x100);
  mem_write16(cpu,cpu->ds,0xc57f,0x400);
  mem_write8(cpu,cpu->ds,0x141c+0x2a,0x13);
}
int main(void) {
  assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
                                "0xfeed/0xbef0",SDL_HINT_OVERRIDE));
  SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
  assert(SDL_Init(SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)==0);
  SDL_VirtualJoystickDesc desc={0};
  desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
  desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;
  desc.naxes=2; desc.nbuttons=1;
  desc.vendor_id=0xfeed; desc.product_id=0xbef0;
  desc.name="LHX TOW regression";
  int index=SDL_JoystickAttachVirtualEx(&desc); assert(index>=0);
  char guid[33],mapping[512];
  SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof(guid));
  snprintf(mapping,sizeof(mapping),"%s,LHX TOW regression,leftx:a0,lefty:a1,righttrigger:b0,",guid);
  assert(SDL_GameControllerAddMapping(mapping)>=0);
  SDL_Joystick *joystick=SDL_JoystickOpen(index); assert(joystick);
  CPU cpu={0}; cpu.mem=calloc(1,MEM_SIZE); assert(cpu.mem);
  cpu.ds=0x2000; cpu.ss=0x3100; cpu.bp=0x8000;
  lhx_gamepad_init(key);
  poll(1000000); lhx_gamepad_apply(&cpu);
  assert(SDL_JoystickSetVirtualButton(joystick,0,1)==0);
  poll(1001000); lhx_gamepad_apply(&cpu);
  assert(fire_makes==1);
  poll(1351000); lhx_gamepad_apply(&cpu);
  unsigned ordinary_repeat=fire_makes-1;
  assert(ordinary_repeat==1); /* retain held fire outside manual guidance */
  assert(SDL_JoystickSetVirtualButton(joystick,0,0)==0); poll(1400000);
  fire_makes=0;
  assert(SDL_JoystickSetVirtualButton(joystick,0,1)==0); poll(2000000);
  assert(fire_makes==1);
  manual(&cpu,1); lhx_gamepad_apply(&cpu);
  poll(2350000); lhx_gamepad_apply(&cpu);
  poll(2450000); lhx_gamepad_apply(&cpu);
  unsigned tow_repeats=fire_makes-1;
  manual(&cpu,0); lhx_gamepad_apply(&cpu); poll(2550000);
  unsigned after_guidance_repeats=fire_makes-1-tow_repeats;
  manual(&cpu,1); lhx_gamepad_apply(&cpu);
  assert(SDL_JoystickSetVirtualButton(joystick,0,0)==0); poll(2560000);
  unsigned previous=fire_makes;
  assert(SDL_JoystickSetVirtualButton(joystick,0,1)==0); poll(2570000);
  unsigned explicit_repress=fire_makes-previous;
  assert(SDL_JoystickSetVirtualButton(joystick,0,0)==0); poll(2580000);
  assert(SDL_JoystickSetVirtualAxis(joystick,0,32767)==0); poll(2600000);
  lhx_gamepad_apply(&cpu); /* relocated observer, before the original save */
  /* Exact original main sequence around fn_20D56: preserve missile axes,
     clear them for helicopter integration, call the existing native observer,
     then restore for the original manual missile routine fn_0338D. */
  mem_write16(&cpu,cpu.ss,cpu.bp-0x74,mem_read16(&cpu,cpu.ds,0xead6));
  mem_write16(&cpu,cpu.ss,cpu.bp-0x0a,mem_read16(&cpu,cpu.ds,0xead8));
  mem_write16(&cpu,cpu.ds,0xead6,0); mem_write16(&cpu,cpu.ds,0xead8,0);
  int helicopter_axis=(int16_t)mem_read16(&cpu,cpu.ds,0xead6);
  mem_write16(&cpu,cpu.ds,0xead6,mem_read16(&cpu,cpu.ss,cpu.bp-0x74));
  mem_write16(&cpu,cpu.ds,0xead8,mem_read16(&cpu,cpu.ss,cpu.bp-0x0a));
  int missile_axis=(int16_t)mem_read16(&cpu,cpu.ds,0xead6);
  manual(&cpu,0); lhx_gamepad_apply(&cpu);
  int ordinary_axis=(int16_t)mem_read16(&cpu,cpu.ds,0xead6);
  printf("{\"ordinary_repeat\":%u,\"tow_repeats\":%u,\"after_guidance_repeats\":%u,\"explicit_repress\":%u,"
         "\"helicopter_axis_during_tow\":%d,\"missile_axis\":%d,\"ordinary_axis\":%d}\n",
         ordinary_repeat,tow_repeats,after_guidance_repeats,explicit_repress,helicopter_axis,missile_axis,ordinary_axis);
  lhx_gamepad_close(); SDL_JoystickClose(joystick);
  assert(SDL_JoystickDetachVirtual(index)==0);
  SDL_Quit(); free(cpu.mem); return 0;
}
'''


def run_probe():
    flags = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2"],
                           check=True, capture_output=True, text=True)
    with tempfile.TemporaryDirectory(prefix="lhx-tow-pad-") as directory:
        tmp = Path(directory)
        (tmp / "recomp_all.h").write_text("")
        source, binary = tmp / "probe.c", tmp / "probe"
        source.write_text(SOURCE)
        subprocess.run(["cc", "-std=c11", "-O2", "-I", str(tmp),
                        "-I", str(ROOT / "runtime"), str(source),
                        str(ROOT / "runtime/gamepad.c"), str(ROOT / "runtime/controls.c"), str(ROOT / "runtime/controller_input.c"), str(ROOT / "runtime/controller_profile.c"), str(ROOT / "runtime/controller_model.c"),
                        *shlex.split(flags.stdout), "-o", str(binary)], check=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True,
                                env=os.environ | {"SDL_VIDEODRIVER": "dummy",
                                                  "SDL_AUDIODRIVER": "dummy"}, timeout=10)
        observed = json.loads(result.stdout)
        return observed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--require-fixed", action="store_true")
    args = parser.parse_args()
    observed = run_probe()
    print(json.dumps(observed, indent=2))
    assert observed["ordinary_repeat"] == 1 and observed["explicit_repress"] == 1
    assert observed["ordinary_axis"] == 100, observed
    if args.require_fixed:
        assert observed["tow_repeats"] == 0, "Held fire prematurely detonates TOW"
        assert observed["after_guidance_repeats"] == 0, "Held fire resumes after guidance ends"
        assert observed["helicopter_axis_during_tow"] == 0, "Stick steers helicopter during TOW"
        assert observed["missile_axis"] == 100, "Stick does not reach original TOW guidance"


if __name__ == "__main__":
    main()
