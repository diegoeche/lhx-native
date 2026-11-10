"""Regress spherical sky wrapping and mip selection with hidden renderers."""
from pathlib import Path
import os
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_renderer.h"
#include "scene_sky.h"
#ifdef GPU
#include "scene_gpu.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static uint32_t pixels[960*600];
static LhxSceneView view={.x1=320,.y1=200,.cx=160.5,.cy=100.5,.focus=128,
 .has_camera=1,.camera={0,100,0},.sky_color=7};
static LhxSceneFrame frame={.views=&view,.view_count=1};
static LhxSceneRenderOptions options;
static uint8_t palette[768];
static unsigned width=320,height=200;
#ifdef GPU
static LhxGpu *gpu;
static uint32_t overlay[64000];
#endif
static void heading(double degrees){
 double a=degrees*3.141592653589793/180;
 view.world_to_view[0][0]=view.world_to_view[2][2]=cos(a);
 view.world_to_view[0][2]=-sin(a);view.world_to_view[2][0]=sin(a);
 view.world_to_view[1][1]=1;
}
static void attitude(double yaw,double pitch,double roll){
 double a=yaw*3.141592653589793/180,b=pitch*3.141592653589793/180,c=roll*3.141592653589793/180;
 double right[3]={cos(a),0,-sin(a)},up[3]={-sin(a)*sin(b),cos(b),-cos(a)*sin(b)};
 for(unsigned i=0;i<3;i++){
  view.world_to_view[0][i]=right[i]*cos(c)+up[i]*sin(c);
  view.world_to_view[1][i]=-right[i]*sin(c)+up[i]*cos(c);
 }
 view.world_to_view[2][0]=sin(a)*cos(b);view.world_to_view[2][1]=sin(b);
 view.world_to_view[2][2]=cos(a)*cos(b);
}
static void draw(void){
#ifdef GPU
 assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);
 assert(lhx_gpu_read_pixels(gpu,pixels));
#else
 assert(lhx_scene_render_with_options(&frame,palette,pixels,width,height,&options)==1);
#endif
}
int main(void){
 palette[21]=palette[22]=palette[23]=63;
#ifdef GPU
 width=960;height=600;
 if(SDL_Init(SDL_INIT_VIDEO)<0)return 77;
 lhx_gpu_attributes();
 SDL_Window *window=SDL_CreateWindow("Sky projection test",0,0,320,200,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!window){SDL_Quit();return 77;}
 gpu=lhx_gpu_create(window,width,height,0,0);assert(gpu);
#endif
 /* The last/first longitude texels straddle the same bilinear sample. */
 uint32_t wrap[]={0xffff0000,0xffff0000,0xff0000ff,0xff0000ff,
                  0xffff0000,0xffff0000,0xff0000ff,0xff0000ff};
 LhxSky sky={.day={.count=1,.levels={{4,2,wrap}},.revision=1}};
 options.sky=&sky;heading(180);draw();
 unsigned at=(height/10)*width+width/2;
 uint32_t seam=pixels[at];
 assert((seam&255)>90 && (seam&255)<110);
 assert(((seam>>16)&255)>90 && ((seam>>16)&255)<110);
 /* This lower-hemisphere blue must not appear at a longitude wrap far above
  * the horizon. Implicit derivatives used to select the global purple mip
  * along a two-pixel vertical stripe. Offset yaw makes a quad cross the wrap. */
 LhxTexture *t=&sky.day;*t=(LhxTexture){.count=1,.levels={{1024,512,NULL}}};
 t->levels[0].pixels=malloc(1024*512*sizeof(uint32_t));assert(t->levels[0].pixels);
 for(unsigned y=0;y<512;y++)for(unsigned x=0;x<1024;x++)
  t->levels[0].pixels[y*1024+x]=y<256?0xffff0000:0xff0000ff;
 assert(lhx_texture_mipmaps(t));
 heading(180.17);draw();
 for(unsigned y=0;y<height/3;y++)for(unsigned x=0;x<width;x++){
  uint32_t c=pixels[y*width+x];
  if((c&255)>1 || ((c>>16)&255)<198){
   fprintf(stderr,"sky wrap chose wrong mip at %u,%u: %08x\n",x,y,c);abort();
  }
 }
 lhx_texture_free(t);
 /* A constant opaque panorama isolates coverage from art and mip variation.
  * Include exact poles, pitched/rolled horizons and both haze settings through
  * a complete yaw turn. Black/transparent output would expose a missing fill
  * or nonfinite projection; every sky/ground ray must retain opaque color. */
 uint32_t white[]={0xffffffff,0xffffffff};
 sky.day=(LhxTexture){.count=1,.levels={{2,1,white}},.revision=123456};
 palette[7]=63;view.ground_color=2;
 const double pitches[]={-90,-65,0,65,90};
 for(unsigned haze=0;haze<2;haze++)for(unsigned yaw=0;yaw<=360;yaw+=45)
  for(unsigned p=0;p<5;p++){
   options.atmosphere=haze;attitude(yaw,pitches[p],yaw%90?43:0);draw();
   for(unsigned i=0;i<width*height;i++){
    assert((pixels[i]>>24)==255);assert((pixels[i]&0x00ffffff)!=0);
    if(!haze)assert(pixels[i]==0xffc7c7c7 || pixels[i]==0xff00ff00);
   }
  }
 /* Absent photo skies retain the captured horizon, independent of heading. */
 options.sky=NULL;options.atmosphere=0;
 view.horizon=(LhxSceneHorizon){.colors={7,2},.two_colors=1,.y=100.5,.ny=-1};
 draw();assert(pixels[20*width+width/2]==0xffffffff);
 assert(pixels[(height-20)*width+width/2]==0xff00ff00);
 /* Native star geometry is retained by the day/disabled paths and suppressed
  * when the imported night panorama supplies its own stars. */
 uint32_t blue[]={0xff0000ff,0xff0000ff};
 sky.night=(LhxTexture){.count=1,.levels={{2,1,blue}},.revision=123457};
 LhxSceneVertex point={0,40,100};uint16_t index=0;
 LhxScenePrimitive star={.kind=3,.count=1,.indices=&index,.color=7};
 LhxSceneMesh stars={.vertices=&point,.vertex_count=1,.primitives=&star,
                    .primitive_count=1,.layer=0,.model_name="stars"};
 view.meshes=&stars;view.mesh_count=1;attitude(0,0,0);
 options.sky=&sky;view.sky_color=7;draw();
 unsigned star_at=(unsigned)((view.cy-.9375*view.focus*point.y/point.z)*height/200)*width+
                  (unsigned)(view.cx*width/320);
 assert(pixels[star_at]==0xffffffff);
 view.sky_color=0;draw();assert(pixels[star_at]==0xff0000c7);
 options.sky=NULL;draw();assert(pixels[star_at]==0xffffffff);
 view.mesh_count=0;view.sky_color=7;
#ifndef GPU
 /* Malformed caller-supplied levels retain the sampler's bounded fallback. */
 options.sky=&sky;
 sky.day=(LhxTexture){.count=1,.levels={{0,1,white}}};draw();
 assert(pixels[20*width+width/2]==0xff000000);
 sky.day.levels[0]=(LhxTextureLevel){2,0,white};draw();
 assert(pixels[20*width+width/2]==0xff000000);
 sky.day=(LhxTexture){.count=2,.levels={{4,2,wrap},{2,1,NULL}}};
 view.focus=.25;draw();assert(pixels[20*width+width/2]==0xff000000);
#endif
#ifdef GPU
 lhx_gpu_destroy(gpu);SDL_DestroyWindow(window);SDL_Quit();
#endif
 lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SkyProjection(unittest.TestCase):
    def check_renderer(self, gpu):
        packages = []
        if gpu:
            if not shutil.which("pkg-config"):
                self.skipTest("pkg-config required")
            result = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2", "gl"],
                                    capture_output=True, text=True)
            if result.returncode:
                self.skipTest("Optional OpenGL libraries unavailable")
            packages = shlex.split(result.stdout)
        with tempfile.TemporaryDirectory(prefix="lhx-sky-projection-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            source = work / "check.c"
            source.write_text(HARNESS)
            binary = work / "check"
            units = ["scene_renderer", "scene_motion", "scene_relief", "scene_props", "scene_texture"]
            if gpu:
                units.append("scene_gpu")
            result = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "runtime"),
                "-I", str(ROOT / "third_party/pcrecomp/runtime"),
                *(["-DGPU", "-DLHX_HAVE_OPENGL"] if gpu else []), str(source),
                *(str(ROOT / "runtime" / (name + ".c")) for name in units),
                *packages, "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30,
                env=os.environ | {"SDL_VIDEODRIVER": os.environ.get("LHX_TEST_VIDEO_DRIVER", "offscreen")})
            if gpu and result.returncode == 77:
                self.skipTest("Offscreen OpenGL unavailable")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cpu_longitude_wrap(self):
        self.check_renderer(False)

    def test_gpu_longitude_wrap_and_mips(self):
        self.check_renderer(True)


if __name__ == "__main__":
    unittest.main()
