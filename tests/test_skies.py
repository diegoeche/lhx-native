"""Check bounded panorama loading, day/night selection and the original fallback."""
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_sky.h"
#include "scene_renderer.h"
#include <assert.h>
#include <stddef.h>
static uint32_t image[320*200];
int main(int argc,char **argv){
 assert(argc==3);LhxSky *sky=lhx_sky_load(argv[1]);assert(sky);assert(!lhx_sky_load(argv[2]));
 assert(sky->day.levels[0].width==4 && sky->day.count==3);
 uint8_t palette[768]={0};palette[21]=palette[22]=palette[23]=63;
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.sky_color=7,.has_camera=1,.camera={0,100,0}};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 LhxSceneFrame frame={.views=&view,.view_count=1};LhxSceneRenderOptions options={.sky=sky};
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
 assert(image[20*320+160]==0xffc70000);assert(image[180*320+160]==0xff000000);
 for(unsigned i=0;i<8;i++)sky->day.levels[0].pixels[i]=(i%4)<2?0xffff0000:0xff0000ff;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
 assert((image[20*320+160]&255)>90 && ((image[20*320+160]>>16)&255)>90);
 view.world_to_view[0][0]=view.world_to_view[2][2]=0;
 view.world_to_view[0][2]=1;view.world_to_view[2][0]=-1;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[20*320+160]==0xffc70000);
 view.world_to_view[0][0]=view.world_to_view[2][2]=1;
 view.world_to_view[0][2]=view.world_to_view[2][0]=0;
 view.sky_color=0;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[20*320+160]==0xff0000c7);
 view.has_camera=0;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[20*320+160]==0xff000000);
 view.has_camera=1;options.sky=NULL;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);assert(image[20*320+160]==0xff000000);
 lhx_sky_free(sky);lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class Skies(unittest.TestCase):
    def test_loading_day_night_and_fallback(self):
        with tempfile.TemporaryDirectory(prefix="lhx-skies-") as directory:
            root = Path(directory)
            valid, invalid = root / "valid", root / "invalid"
            valid.mkdir()
            invalid.mkdir()
            for name, rgb in [("day", b"\xff\0\0"), ("night", b"\0\0\xff")]:
                (valid / (name+".lhs")).write_bytes(b"LHXSKY1\0"+struct.pack("<II", 4, 2)+rgb*8)
            (invalid / "day.lhs").write_bytes(b"LHXSKY1\0"+struct.pack("<II", 4098, 2049))
            source = root / "check.c"
            source.write_text(HARNESS)
            binary = root / "check"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(source), str(ROOT / "runtime/scene_sky.c"),
                str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"),
                str(ROOT / "runtime/scene_texture.c"), "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary), str(valid), str(invalid)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)


if __name__ == "__main__":
    unittest.main()
