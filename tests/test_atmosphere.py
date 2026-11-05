"""Check a continuous terrain horizon, depth haze, and the disabled CPU path."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_renderer.h"
#include "scene_atmosphere.h"
#include <assert.h>
#include <math.h>
#include <string.h>
static uint32_t image[320*200],baseline[320*200];
static unsigned difference(uint32_t a,uint32_t b){
 unsigned d=0;for(unsigned s=0;s<=16;s+=8){int n=(int)((a>>s)&255)-(int)((b>>s)&255);d+=n<0?-n:n;}return d;}
int main(void){
 uint8_t palette[768]={0};palette[3]=63;palette[8]=63;
 palette[21]=palette[22]=palette[23]=63;
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,
   .has_camera=1,.camera={0,1000,0},.sky_color=7,.ground_color=1,
   .horizon={.colors={1,7},.two_colors=1,.x=160,.y=100,.nx=0,.ny=1}};
 view.world_to_view[0][0]=view.world_to_view[1][1]=view.world_to_view[2][2]=1;
 LhxSceneFrame frame={.views=&view,.view_count=1};
 uint32_t blue=0xff0000ff;
 LhxTerrain terrain={.near={.texture={.count=1,.levels={{1,1,&blue}}},.bounds={-1000,1000,0,1000}},
                     .far={.texture={.count=1,.levels={{1,1,&blue}}},.bounds={-10000,10000,0,10000}}};
 LhxSceneRenderOptions options={.terrain=&terrain};
 assert(lhx_scene_render_with_options(&frame,palette,baseline,320,200,&options)==1);
 assert(baseline[100*320+160]==0xffff0000); /* Original red strip outside imagery. */
 options.atmosphere=1;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
 uint32_t haze=lhx_haze_color(0xffffffff);assert(haze==0xffa3b0bf);
 assert(image[100*320+160]==haze); /* No palette ground beyond the texture edge. */
 assert(difference(image[99*320+160],haze)<50); /* Sky approaches the same horizon. */
 assert(difference(image[150*320+160],blue)<3); /* Near satellite detail remains. */
 assert(fabs(lhx_haze_visibility(250000)-exp(-1))<0.000001);
 assert(lhx_haze_color(0xff000000)==0xff0d0f17);
 /* Distant original geometry must blend into the same haze as terrain. */
 LhxSceneVertex points[]={{-100000,-100000,250000},{100000,-100000,250000},
                          {100000,100000,250000},{-100000,100000,250000}};
 uint16_t indices[]={0,1,2,3};
 LhxScenePrimitive face={.kind=0,.count=4,.indices=indices,.color=7};
 LhxSceneMesh mesh={.vertices=points,.vertex_count=4,.primitives=&face,.primitive_count=1,.layer=2};
 view.meshes=&mesh;view.mesh_count=1;options.terrain=NULL;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
 assert(difference(image[100*320+160],lhx_atmosphere_mix(0xffffffff,haze,exp(-1)))<=1);
 /* Instruments and omitted/disabled settings preserve their original colors. */
 view.has_camera=0;
 assert(lhx_scene_render_with_options(&frame,palette,image,320,200,&options)==1);
 options.atmosphere=0;
 assert(lhx_scene_render_with_options(&frame,palette,baseline,320,200,&options)==1);
 assert(!memcmp(image,baseline,sizeof image));
 assert(image[100*320+160]==0xffffffff);
 lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class Atmosphere(unittest.TestCase):
    def test_horizon_haze_and_legacy_colors(self):
        with tempfile.TemporaryDirectory(prefix="lhx-atmosphere-") as directory:
            work = Path(directory)
            source = work / "check.c"
            source.write_text(HARNESS)
            binary = work / "check"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(source), str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"),
                str(ROOT / "runtime/scene_texture.c"), "-lm", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
