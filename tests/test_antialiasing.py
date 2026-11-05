"""Check spatial AA, subpixel coverage, occlusion and viewport ownership."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_renderer.h"
#include <assert.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
static uint32_t image[960*600],baseline[960*600];
static uint8_t palette[768];
static LhxSceneVertex screen(double x,double y,double z){
 return (LhxSceneVertex){(x/3-160)*z/128,(100-y/3)*z/120,z};}
static unsigned energy(void){
 unsigned sum=0;for(unsigned i=0;i<960*600;i++)sum+=image[i]&255;return sum;}
int main(void){
 palette[3]=palette[4]=palette[5]=63;
 palette[6]=palette[7]=palette[8]=5; /* twenty grey levels */
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128};
 LhxSceneFrame frame={.views=&view,.view_count=1};
 LhxSceneRenderOptions aa={.antialiasing=1};
 uint16_t idx[]={0,1,2};
 LhxScenePrimitive p={.kind=2,.color=2,.count=1,.indices=idx};
 LhxSceneVertex v[3];
 LhxSceneMesh m={.vertices=v,.vertex_count=1,.primitives=&p,.primitive_count=1,.layer=2};
 view.meshes=&m;view.mesh_count=1;
 /* Moving a marker through four subpixel phases keeps one pixel's energy.
  * The low-contrast marker also checks that the spatial filter leaves it alone. */
 for(unsigned phase=0;phase<4;phase++){
  v[0]=screen(400.5+phase*.25,300.5+phase*.25,100);
  assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
  assert(abs((int)energy()-20)<=2);
 }
 assert(lhx_scene_render(&frame,palette,image,960,600)==1);
 assert(energy()>80); /* legacy marker expands with render scale */
 /* Interior columns of a horizontal line have stable fractional coverage. */
 p.kind=1;p.count=2;m.vertex_count=2;
 for(unsigned phase=0;phase<4;phase++){
  v[0]=screen(300.5,300.5+phase*.25,100);v[1]=screen(360.5,300.5+phase*.25,100);
  assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
  unsigned sum=0;for(unsigned y=295;y<306;y++)sum+=image[y*960+330]&255;
  assert(abs((int)sum-20)<=2);
 }
 /* A finite sphere smaller than a pixel fades with projected area. */
 p.kind=3;p.count=1;p.radius=.0651041667f;m.vertex_count=1;
 v[0]=screen(400.5,300.5,100);
 assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
 unsigned near=energy();assert(near>0 && near<8);
 v[0]=screen(400.5,300.5,200);
 assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
 assert(energy()<near);
 /* High-contrast diagonal silhouettes gain intermediate colour samples. */
 p.kind=0;p.count=3;p.color=1;m.vertex_count=3;
 v[0]=screen(200.2,180.3,100);v[1]=screen(760.7,230.4,100);v[2]=screen(400.1,500.8,100);
 assert(lhx_scene_render(&frame,palette,baseline,960,600)==1);
 assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
 unsigned partial=0;for(unsigned i=0;i<960*600;i++){
  unsigned c=image[i]&255;partial+=c>0 && c<255;
 }
 assert(partial>100);
 assert(image[300*960+450]==0xffffffff && image[40*960+40]==0xff000000);
 /* Completely hidden thin geometry must not leak through an opaque face. */
 LhxScenePrimitive dot={.kind=2,.color=2,.count=1,.indices=idx};
 LhxSceneVertex point=screen(450.5,300.5,200);
 LhxSceneMesh meshes[]={m,{.vertices=&point,.vertex_count=1,.primitives=&dot,
                            .primitive_count=1,.layer=2}};
 memcpy(baseline,image,sizeof image);view.meshes=meshes;view.mesh_count=2;
 assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
 assert(!memcmp(baseline,image,sizeof image));
 /* Filtering a small view cannot read/mix pixels from outside its rectangle. */
 view.mesh_count=0;view.x0=30;view.x1=90;view.y0=20;view.y1=70;
 for(unsigned i=0;i<960*600;i++)image[i]=0x12345678;
 assert(lhx_scene_render_with_options(&frame,palette,image,960,600,&aa)==1);
 for(unsigned y=0;y<600;y++)for(unsigned x=0;x<960;x++)
  assert(image[y*960+x]==(x>=90 && x<270 && y>=60 && y<210?0xff000000:0x12345678));
 /* Small render targets and an empty frame still respect ownership. */
 for(unsigned w=1;w<=3;w++)for(unsigned h=1;h<=3;h++)
  assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&aa)>=0);
 frame.view_count=0;assert(!lhx_scene_render_with_options(&frame,palette,image,960,600,&aa));
 lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class Antialiasing(unittest.TestCase):
    def test_coverage_filter_depth_and_bounds(self):
        with tempfile.TemporaryDirectory(prefix="lhx-aa-") as directory:
            work = Path(directory)
            source = work / "check.c"
            source.write_text(HARNESS)
            binary = work / "check"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(source),
                str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"), str(ROOT / "runtime/scene_texture.c"),
                "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True, timeout=15)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)


if __name__ == "__main__":
    unittest.main()
