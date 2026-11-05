"""Validate controller and rendering profiles before SDL or gameplay."""
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"),
                     "C compiler and SDL2 development files required")
class Controls(unittest.TestCase):
    def test_defaults_remapping_and_invalid_profiles(self):
        flags = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2"],
                               capture_output=True, text=True)
        if flags.returncode:
            self.skipTest("SDL2 development files required")
        with tempfile.TemporaryDirectory(prefix="lhx-controls-") as directory:
            tmp = Path(directory)
            valid = tmp / "valid.ini"
            valid.write_text("""# comments and case-insensitive names
[BUTTONS]
A = fire ; inline comment
x = none
y = flares
leftshoulder = previous_weapon
[axes]
lefty = -pitch
lefttrigger = chaff
righttrigger = none
[settings]
deadzone = 7000
repeat_delay_ms = 0
fire_repeat_ms = 50
throttle_repeat_ms = 400
[RENDERING]
Renderer = ORIGINAL
render_scale = 1
visibility = original
show_fps = TRUE
lighting = true
antialiasing = TRUE
sphere_depth = TRUE
atmosphere = TRUE
replacement_models = TRUE
models_dir = /tmp/scenery models
photo_skies = TRUE
sky_dir = /tmp/sky images
terrain_relief = TRUE
shadows = TRUE
military_paint = TRUE
surface_detail = TRUE
rotor_blades = TRUE
runway_detail = TRUE

backend = GPU
runway_textures = true
terrain_textures = false
terrain_dir = /tmp/terrain images
""")
            native = tmp / "native.ini"
            native.write_text("[rendering]\nrenderer = native\nrender_scale = 8\n"
                              "visibility = extended\nshow_fps = false\nbackend = cpu\n")
            bad_profiles = [
                "[rendering]\nterrain_relief = yes\n",
                "[rendering]\nshadows = yes\n",
                "[rendering]\nmilitary_paint = yes\n",
                "[rendering]\nsurface_detail = yes\n",
                "[rendering]\nrotor_blades = yes\n",
                "[rendering]\nrunway_detail = yes\n",

                "a = fire\n", "[buttons]\na = shoot\n",
                "[buttons]\nunknown_button = fire\n", "[axis]\nleftx = roll\n",
                "[axes]\nlefttrigger = shoot\n", "[axes]\nunknown_axis = roll\n",
                "[settings]\nfire_repeat_ms = 0\n", "[settings]\ndeadzone = 32767\n",
                "[settings]\ntrigger_threshold = -1\n",
                "[settings]\nrepeat_delay_ms = 50ms\n",
                "[settings]\nunknown = 1\n", "[buttons]\n" + "a" * 600 + " = fire\n",
                "[buttons]\na = fire\n[settings]\nfire_repeat_ms = 1\n",
                "[rendering]\nrenderer = enhanced\n",
                "[rendering]\nrender_scale = 0\n",
                "[rendering]\nrender_scale = 9\n",
                "[rendering]\nrender_scale = -1\n",
                "[rendering]\nrender_scale = 3px\n",
                "[rendering]\nrender_scale = 9999999999999999999999999\n",
                "[rendering]\nrender_scale =\n",
                "[rendering]\nvisibility = far\n",
                "[rendering]\nshow_fps = yes\n",
                "[rendering]\nunknown = true\n",
                "[rendering]\nvisibility = extended\nrender_scale = 9\n",
                "[rendering]\nlighting = on\n",
                "[rendering]\nantialiasing = yes\n",
                "[rendering]\nsphere_depth = on\n",
                "[rendering]\nrunway_textures = 2\n",
                "[rendering]\nterrain_textures = yes\n",
                "[rendering]\nterrain_dir =\n",
                "[rendering]\nterrain_dir = " + "x" * 256 + "\n",
                "[rendering]\nbackend = opengl\n",
                "[rendering]\natmosphere = on\n",
                "[rendering]\nreplacement_models = yes\n",
                "[rendering]\nmodels_dir =\n",
                "[rendering]\nmodels_dir = " + "x" * 256 + "\n",
                "[rendering]\nphoto_skies = yes\n",
                "[rendering]\nsky_dir =\n",
                "[rendering]\nsky_dir = " + "x" * 256 + "\n",
            ]
            paths = []
            for index, body in enumerate(bad_profiles):
                path = tmp / f"bad-{index}.ini"
                path.write_text(body)
                paths.append(path)
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <string.h>
#include "controls.h"
int main(int argc,char **argv) {
    assert(argc>=6);
    LhxControls defaults, c, saved;
    lhx_controls_defaults(&defaults);
    assert(defaults.rendering.renderer==LHX_RENDER_DEFAULT);
    assert(defaults.rendering.render_scale==0);
    assert(defaults.rendering.extended_visibility && !defaults.rendering.show_fps);
    assert(!defaults.rendering.lighting && !defaults.rendering.runway_textures &&
           !defaults.rendering.terrain_textures);
    assert(!defaults.rendering.antialiasing);
    assert(!defaults.rendering.sphere_depth);
    assert(!defaults.rendering.atmosphere);
    assert(!defaults.rendering.terrain_relief);
    assert(!defaults.rendering.shadows);
    assert(!defaults.rendering.military_paint);
    assert(!defaults.rendering.surface_detail);
    assert(!defaults.rendering.rotor_blades);
    assert(!defaults.rendering.runway_detail);

    assert(!defaults.rendering.replacement_models);
    assert(!strcmp(defaults.rendering.models_dir,"bin/enhanced/models"));
    assert(!defaults.rendering.photo_skies);
    assert(!strcmp(defaults.rendering.sky_dir,"bin/enhanced/sky"));
    assert(!defaults.rendering.gpu);
    assert(!strcmp(defaults.rendering.terrain_dir,"bin/enhanced/terrain"));
    assert(lhx_controls_load(&c,argv[1],0)); /* absent optional file */
    assert(!memcmp(&defaults,&c,sizeof(c)));
    assert(!lhx_controls_load(&c,argv[1],1));
    assert(lhx_controls_load(&c,argv[2],1));
    /* The editable project profile may intentionally differ from defaults. */
    assert(lhx_controls_load(&c,argv[3],1));
    assert(c.buttons[SDL_CONTROLLER_BUTTON_A]==LHX_FIRE);
    assert(c.buttons[SDL_CONTROLLER_BUTTON_X]==LHX_NONE);
    assert(c.buttons[SDL_CONTROLLER_BUTTON_Y]==LHX_FLARES);
    assert(c.buttons[SDL_CONTROLLER_BUTTON_B]==LHX_CANCEL);
    assert(c.buttons[SDL_CONTROLLER_BUTTON_LEFTSHOULDER]==LHX_PREVIOUS);
    assert(c.axes[SDL_CONTROLLER_AXIS_LEFTY].action==LHX_PITCH);
    assert(c.axes[SDL_CONTROLLER_AXIS_LEFTY].direction==-1);
    assert(c.axes[SDL_CONTROLLER_AXIS_TRIGGERRIGHT].action==LHX_AXIS_NONE);
    assert(c.axes[SDL_CONTROLLER_AXIS_TRIGGERLEFT].action==LHX_AXIS_CHAFF);
    assert(c.deadzone==7000 && c.repeat_delay_ms==0);
    assert(c.fire_repeat_ms==50 && c.throttle_repeat_ms==400);
    assert(c.rendering.renderer==LHX_RENDER_ORIGINAL && c.rendering.render_scale==1);
    assert(!c.rendering.extended_visibility && c.rendering.show_fps);
    assert(c.rendering.lighting && c.rendering.runway_textures && !c.rendering.terrain_textures);
    assert(c.rendering.antialiasing);
    assert(c.rendering.sphere_depth);
    assert(c.rendering.atmosphere);
    assert(c.rendering.terrain_relief);
    assert(c.rendering.shadows);
    assert(c.rendering.military_paint);
    assert(c.rendering.surface_detail);
    assert(c.rendering.rotor_blades);
    assert(c.rendering.runway_detail);

    assert(c.rendering.replacement_models);
    assert(!strcmp(c.rendering.models_dir,"/tmp/scenery models"));
    assert(c.rendering.photo_skies);
    assert(!strcmp(c.rendering.sky_dir,"/tmp/sky images"));
    assert(c.rendering.gpu);
    assert(!strcmp(c.rendering.terrain_dir,"/tmp/terrain images"));
    saved=c;
    assert(lhx_controls_load(&c,argv[4],1));
    assert(c.rendering.renderer==LHX_RENDER_NATIVE && c.rendering.render_scale==8);
    assert(!c.rendering.gpu);
    assert(c.rendering.extended_visibility && !c.rendering.show_fps);
    c=saved;
    for(int i=5;i<argc;i++) {
        assert(!lhx_controls_load(&c,argv[i],1));
        assert(!memcmp(&saved,&c,sizeof(c))); /* no partially applied profile */
    }
}
''')
            binary = tmp / "check"
            built = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/controls.c"),
                *shlex.split(flags.stdout), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([
                str(binary), str(tmp / "absent.ini"), str(ROOT / "controls.ini"),
                str(valid), str(native), *map(str, paths)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)
            self.assertIn("bad-18.ini:4:", run.stderr)


if __name__ == "__main__":
    unittest.main()
