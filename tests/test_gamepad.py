"""Exercise F310 bindings with an isolated SDL virtual controller."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"),
                     "C compiler and SDL2 pkg-config required")
class Gamepad(unittest.TestCase):
    def test_f310_mapping_axes_repeats_and_lifecycle(self):
        version = subprocess.run(["pkg-config", "--atleast-version=2.24.0", "sdl2"],
                                 capture_output=True)
        if version.returncode:
            self.skipTest("SDL2 2.24 virtual joystick support required")
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2"],
                               capture_output=True, text=True)
        if flags.returncode:
            self.skipTest("SDL2 development files absent")
        with tempfile.TemporaryDirectory(prefix="lhx-gamepad-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gamepad.h"
#include "lifted.h"
static unsigned makes[65536],breaks[65536],sequence[8192],count;
static unsigned char down[65536];
static uint64_t now=1000000;
static CPU cpu;
static SDL_Joystick*joystick;
static int load_config(const char *path){
    LhxControls config;
    if(!lhx_controls_load(&config,path,1))return 0;
    lhx_gamepad_configure(&config);return 1;
}
static void key(unsigned scan,int release){
    assert(scan<65536 && count<8192);
    sequence[count++]=scan|(release?0x10000:0);
    if(release)breaks[scan]++;else makes[scan]++;
    down[scan]=!release;
}
static void step(uint64_t elapsed){
    now+=elapsed;SDL_JoystickUpdate();
    SDL_Event event;while(SDL_PollEvent(&event))lhx_gamepad_event(&event);
    lhx_gamepad_poll(now);lhx_gamepad_apply(&cpu);
}
static void button(unsigned physical,int pressed){
    assert(SDL_JoystickSetVirtualButton(joystick,physical,pressed)==0);step(1000);
}
static void axis(unsigned physical,int value){
    assert(SDL_JoystickSetVirtualAxis(joystick,physical,value)==0);step(1000);
}
static void hat(unsigned value){
    assert(SDL_JoystickSetVirtualHat(joystick,0,value)==0);step(1000);
}
static int lateral(void){return (int16_t)mem_read16(&cpu,cpu.ds,0xead6);}
static int pitch(void){return (int16_t)mem_read16(&cpu,cpu.ds,0xead8);}
static void neutral(void){
    for(unsigned i=0;i<4;i++)assert(SDL_JoystickSetVirtualAxis(joystick,i,0)==0);
    for(unsigned i=0;i<12;i++)assert(SDL_JoystickSetVirtualButton(joystick,i,0)==0);
    assert(SDL_JoystickSetVirtualHat(joystick,0,SDL_HAT_CENTERED)==0);step(1000);
}
static int attach(void){
    SDL_VirtualJoystickDesc desc={0};
    desc.version=SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
    desc.type=SDL_JOYSTICK_TYPE_GAMECONTROLLER;
    desc.naxes=4;desc.nbuttons=12;desc.nhats=1;
    /* Unique test IDs let SDL ignore real controllers attached to the host.
     * The raw button/axis layout and mapping remain those of the F310. */
    desc.vendor_id=0xfeed;desc.product_id=0xbeef;desc.name="Logitech F310 regression";
    int index=SDL_JoystickAttachVirtualEx(&desc);assert(index>=0);
    char guid[33],mapping[1024];
    SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(index),guid,sizeof(guid));
    snprintf(mapping,sizeof(mapping),
      "%s,Logitech F310 regression,a:b1,b:b2,x:b0,y:b3,back:b8,start:b9,"
      "leftshoulder:b4,rightshoulder:b5,leftstick:b10,rightstick:b11,"
      "leftx:a0,lefty:a1,rightx:a2,righty:a3,lefttrigger:b6,righttrigger:b7,"
      "dpup:h0.1,dpright:h0.2,dpdown:h0.4,dpleft:h0.8,",guid);
    assert(SDL_GameControllerAddMapping(mapping)>=0);
    assert(SDL_IsGameController(index));
    joystick=SDL_JoystickOpen(index);assert(joystick);
    assert(SDL_JoystickGetVendor(joystick)==0xfeed);
    assert(SDL_JoystickGetProduct(joystick)==0xbeef);
    return index;
}
static void tap(unsigned physical,unsigned scan){
    unsigned m=makes[scan],b=breaks[scan];
    button(physical,1);assert(down[scan] && makes[scan]==m+1);
    step(1000000);assert(makes[scan]==m+1); /* ordinary buttons do not repeat */
    button(physical,0);assert(!down[scan] && breaks[scan]==b+1);
}
static void repeat(unsigned scan,unsigned delay,unsigned period){
    unsigned m=makes[scan],b=breaks[scan];assert(down[scan]);
    step(delay-1);assert(makes[scan]==m);
    unsigned edge=count;
    step(1);assert(makes[scan]==m+1 && breaks[scan]==b+1 && down[scan]);
    assert(sequence[edge]==(scan|0x10000) && sequence[edge+1]==scan);
    step(period-1);assert(makes[scan]==m+1);
    step(1);assert(makes[scan]==m+2 && breaks[scan]==b+2 && down[scan]);
}
int main(int argc,char **argv){
    assert(argc==4);
    assert(SDL_SetHintWithPriority(SDL_HINT_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT,
                                 "0xfeed/0xbeef",SDL_HINT_OVERRIDE));
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS,"1");
    assert(SDL_Init(SDL_INIT_GAMECONTROLLER|SDL_INIT_EVENTS)==0);
    cpu.mem=calloc(1,MEM_SIZE);assert(cpu.mem);cpu.ds=0x2000;
    int index=attach();lhx_gamepad_init(key);step(0);
    /* Raw DirectInput face-button ordering differs from SDL's A/B/X/Y enum. */
    tap(1,0x1c);tap(2,0x01);tap(0,0x2e);tap(3,0x21);
    tap(6,0x04);tap(10,0x22);tap(11,0x1b);
    /* Fire and +/- collective repeat while held; release stops repeats. */
    button(7,1);repeat(0x39,350000,100000);button(7,0);
    unsigned stopped=makes[0x39];step(1000000);assert(makes[0x39]==stopped);
    button(4,1);repeat(0x07,350000,200000);button(4,0);
    button(5,1);repeat(0x08,350000,200000);button(5,0);
    /* Opposite throttle directions cancel instead of oscillating power. */
    button(4,1);button(5,1);assert(!down[0x07] && !down[0x08]);
    unsigned throttle_events=count;step(1000000);assert(count==throttle_events);
    button(4,0);assert(down[0x08]);button(5,0);assert(!down[0x08]);
    unsigned start=count;
    button(9,1);assert(down[0x1d] && down[0x19]);
    assert(sequence[start]==0x1d && sequence[start+1]==0x19);
    button(9,0);assert(!down[0x1d] && !down[0x19]);
    assert(sequence[start+2]==(0x10000|0x19) && sequence[start+3]==(0x10000|0x1d));
    tap(8,0x3f);tap(8,0x3b);
    assert(load_config(argv[2]));
    /* Multiple countermeasure bindings still deploy just once while held. */
    unsigned cartridges=makes[0x2e],released=breaks[0x2e];
    button(3,1);button(6,1);step(2000000);
    assert(makes[0x2e]==cartridges+1 && breaks[0x2e]==released);
    button(3,0);assert(down[0x2e]);button(6,0);
    assert(!down[0x2e] && breaks[0x2e]==released+1);
    button(6,1);assert(makes[0x2e]==cartridges+2);button(6,0);
    /* RT and X share fire: either source keeps Space held until both release. */
    unsigned m=makes[0x39],b=breaks[0x39];
    button(0,1);button(7,1);button(0,0);
    assert(down[0x39] && makes[0x39]==m+1 && breaks[0x39]==b);
    button(7,0);assert(!down[0x39] && breaks[0x39]==b+1);
    button(7,1);button(0,1);button(7,0);
    assert(down[0x39] && makes[0x39]==m+2 && breaks[0x39]==b+1);
    button(0,0);assert(!down[0x39] && breaks[0x39]==b+2);
    assert(load_config(argv[3]));
    /* Hardware deadzone plus the original flight code's +/-20 deadband. */
    static const int inputs[]={0,6000,6001,16384,32767,-6000,-6001,-16384,-32768};
    static const int values[]={0,0,20,51,100,0,-20,-51,-100};
    for(unsigned n=0;n<sizeof(inputs)/sizeof(inputs[0]);n++){
        axis(0,inputs[n]);assert(lateral()==values[n] && pitch()==0);
    }
    axis(0,0);assert(lateral()==0);
    axis(1,-32768);assert(lateral()==0 && pitch()==-100);
    axis(1,32767);assert(pitch()==100);axis(1,0);assert(pitch()==0);
    /* Neutral input centers once, then lets the original keyboard input run. */
    mem_write16(&cpu,cpu.ds,0xead6,7);mem_write16(&cpu,cpu.ds,0xead8,9);
    step(1000);assert(lateral()==7 && pitch()==9);
    axis(0,16384);assert(lateral()==51 && pitch()==0);
    axis(2,-32768);assert(lateral()==-100 && down[0x38]);
    axis(2,0);assert(lateral()==51 && !down[0x38]);
    axis(0,0);axis(2,32767);assert(lateral()==100 && down[0x38]);
    axis(2,0);assert(lateral()==0 && pitch()==0 && !down[0x38]);
    /* Hat and collective repeats use the supplied virtual clock exactly. */
    const unsigned hats[]={SDL_HAT_UP,SDL_HAT_DOWN,SDL_HAT_LEFT,SDL_HAT_RIGHT};
    const unsigned arrows[]={0xe048,0xe050,0xe04b,0xe04d};
    for(unsigned n=0;n<4;n++){
        hat(hats[n]);repeat(arrows[n],350000,120000);hat(SDL_HAT_CENTERED);assert(!down[arrows[n]]);
    }
    hat(SDL_HAT_RIGHTUP);assert(down[0xe048] && down[0xe04d]);
    hat(SDL_HAT_RIGHT);assert(!down[0xe048] && down[0xe04d]);hat(SDL_HAT_CENTERED);
    axis(3,-12000);assert(!down[0x08]);axis(3,-12001);repeat(0x08,350000,200000);
    axis(3,0);assert(!down[0x08]);
    axis(3,12000);assert(!down[0x07]);axis(3,12001);repeat(0x07,350000,200000);
    axis(3,0);assert(!down[0x07]);
    /* Losing focus releases every key and neutralizes analog input once. */
    button(7,1);axis(2,32767);hat(SDL_HAT_UP);
    lhx_gamepad_focus(0);step(1000);
    assert(!down[0x39] && !down[0x38] && !down[0xe048]);
    assert(lateral()==0 && pitch()==0);
    unsigned events=count;step(2000000);assert(count==events);
    neutral();lhx_gamepad_focus(1);step(1000);assert(count==events);
    /* Physical disconnection cannot leave fire, rudder, or a menu arrow down. */
    button(7,1);axis(2,-32768);hat(SDL_HAT_LEFT);
    assert(SDL_JoystickDetachVirtual(index)==0);step(1000);
    assert(!down[0x39] && !down[0x38] && !down[0xe04b]);
    assert(lateral()==0 && pitch()==0);
    SDL_JoystickClose(joystick);joystick=NULL;
    events=count;step(2000000);assert(count==events);
    /* A later hot-add opens without restarting the game. */
    index=attach();step(1000);tap(1,0x1c);
    /* A file can swap sticks/buttons, invert pitch, disable an input, and
       change repeat timing without recompiling the input implementation. */
    assert(load_config(argv[1]));
    button(1,1);repeat(0x39,40000,60000);button(1,0);
    tap(0,0x1c);tap(4,0x1a);tap(5,0x1b);
    unsigned shots=makes[0x39];button(7,1);step(1000000);
    assert(makes[0x39]==shots);button(7,0);
    axis(0,7000);assert(lateral()==0 && !down[0x38]);
    axis(0,32767);assert(lateral()==100 && down[0x38]);
    axis(2,-32768);assert(lateral()==100);
    axis(0,0);assert(lateral()==-100 && !down[0x38]);axis(2,0);
    axis(3,-32768);assert(pitch()==100);axis(3,0);assert(pitch()==0);
    axis(1,-32768);repeat(0x08,40000,90000);axis(1,0);assert(!down[0x08]);
    button(0,1);axis(0,32767);lhx_gamepad_close();lhx_gamepad_apply(&cpu);
    assert(!down[0x1c] && lateral()==0 && pitch()==0);
    for(unsigned scan=0;scan<65536;scan++)assert(!down[scan]);
    SDL_JoystickClose(joystick);assert(SDL_JoystickDetachVirtual(index)==0);
    SDL_Quit();free(cpu.mem);
    puts("F310 buttons, analog axes, overlap, repeat, focus and hotplug passed");
}
''')
            binary = tmp / "check"
            config = tmp / "custom.ini"
            config.write_text("""[buttons]
a = fire
x = confirm
leftshoulder = previous_weapon
rightshoulder = next_weapon
[axes]
leftx = rudder
lefty = collective
rightx = roll
righty = -pitch
righttrigger = none
[settings]
deadzone = 8000
repeat_delay_ms = 40
fire_repeat_ms = 60
throttle_repeat_ms = 90
""")
            overlap = tmp / "overlap.ini"
            overlap.write_text("[buttons]\nx = fire\ny = chaff\n[axes]\nlefttrigger = chaff\n")
            defaults = tmp / "defaults.ini"
            defaults.write_text("# Default layout for subsequent checks.\n")
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/gamepad.c"), str(ROOT / "runtime/controls.c"),
                *shlex.split(flags.stdout), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            env = os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}
            result = subprocess.run([str(binary), str(config), str(overlap), str(defaults)],
                                    env=env, capture_output=True, text=True,
                                    timeout=10)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("F310 buttons, analog axes, overlap, repeat, focus and hotplug passed",
                          result.stdout)


if __name__ == "__main__":
    unittest.main()
