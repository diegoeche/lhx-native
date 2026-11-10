"""Logo typography retains its palette and stroke weight; world details stay thin."""
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
#ifdef TEST_GPU
#include "scene_gpu.h"
#include <SDL.h>
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t image[2560*1600],reference[2560*1600];
#ifdef TEST_GPU
static uint32_t overlay[64000];
#endif
static uint8_t palette[768];
static LhxSceneVertex screen(float x,float y){return (LhxSceneVertex){(x-160)*100/128,(100-y)*100/120,100};}
static unsigned column(unsigned w,unsigned h,unsigned x){
 unsigned sum=0;for(unsigned y=0;y<h;y++)sum+=image[y*w+x]&255;return sum;
}
static void materials(unsigned s
#ifdef TEST_GPU
 ,LhxGpu *gpu
#endif
){
 unsigned w=320*s,h=200*s;
 palette[6]=palette[7]=palette[8]=63;
 LhxSceneVertex v[]={screen(70,30),screen(130,30),screen(130,90),screen(70,90),
  screen(70,105.125),screen(130,105.125),
  screen(190,30),screen(250,30),screen(250,90),screen(190,90),
  screen(190,105.125),screen(250,105.125)};
 uint16_t quad[]={0,1,2,3},stroke[]={4,5};
 LhxScenePrimitive p[]={{.kind=0,.color=2,.count=4,.indices=quad},
  {.kind=1,.color=2,.count=2,.indices=stroke}};
 LhxSceneMesh m[]={
  {.vertices=v,.vertex_count=6,.primitives=p,.primitive_count=2,.layer=2,.model_name="logo",.has_placement=1},
  {.vertices=v+6,.vertex_count=6,.primitives=p,.primitive_count=2,.layer=2,.model_name="hangar",.has_placement=1}};
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.has_camera=1,.meshes=m,.mesh_count=2};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 LhxSceneFrame frame={.views=&view,.view_count=1};
 for(unsigned aa=0;aa<2;aa++){
  LhxSceneRenderOptions options={.antialiasing=aa};
  for(unsigned light=0;light<2;light++){
   options.lighting=light;
#ifdef TEST_GPU
   assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);
   assert(lhx_gpu_read_pixels(gpu,image));
#else
   assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&options)==1);
#endif
   /* Both primitives author the same white. Logo fill/stroke stay white;
    * a scene polygon still receives the fixed-world sun's ambient .55. */
   assert(image[60*s*w+100*s]==0xffffffff);
   assert(image[60*s*w+220*s]==(light?0xff8c8c8c:0xffffffff));
   if(!light)memcpy(reference,image,w*h*sizeof(*image));
   else{
    for(unsigned y=0;y<h;y++)for(unsigned x=0;x<160*s;x++)
     assert(image[y*w+x]==reference[y*w+x]);
    for(unsigned y=100*s;y<112*s;y++)for(unsigned x=180*s;x<260*s;x++)
     assert(image[y*w+x]==reference[y*w+x]);
   }
  }
 }
}
int main(void){
 palette[3]=palette[4]=palette[5]=5; /* 20 grey: avoid silhouette postfilter */
 LhxSceneVertex v[]={screen(90,70.125),screen(130,70.125),screen(190,120.125),screen(230,120.125)};
 uint16_t indices[]={0,1};
 LhxScenePrimitive line={.kind=1,.color=1,.count=2,.indices=indices};
 LhxSceneMesh meshes[]={
  {.vertices=v,.vertex_count=2,.primitives=&line,.primitive_count=1,.layer=2,.model_name="logo",.has_placement=1},
  {.vertices=v+2,.vertex_count=2,.primitives=&line,.primitive_count=1,.layer=2,.model_name="hangar",.has_placement=1}};
 LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.meshes=meshes,.mesh_count=2};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 LhxSceneFrame frame={.views=&view,.view_count=1};
 LhxSceneRenderOptions options={.antialiasing=1};
#ifdef TEST_GPU
 if(SDL_Init(SDL_INIT_VIDEO))return 77;
 lhx_gpu_attributes();
 SDL_Window *window=SDL_CreateWindow("Title lines",0,0,320,200,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!window){SDL_Quit();return 77;}
 for(unsigned i=0;i<64000;i++)overlay[i]=0; /* transparent scene sentinel */
#endif
 unsigned scales[]={1,3,8};
 for(unsigned k=0;k<3;k++){
  unsigned s=scales[k],w=320*s,h=200*s;
#ifdef TEST_GPU
  LhxGpu *gpu=lhx_gpu_create(window,w,h,1,0);assert(gpu);
#endif
  /* Captured title has a camera; auxiliary world views can be promoted by
   * enhanced_view. Neither context may turn a world line into a logo stroke. */
  for(unsigned context=0;context<3;context++){
   view.has_camera=context==1;
   options.shadows=context==2; /* promotes the synthetic auxiliary world view */
#ifdef TEST_GPU
   assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);
   assert(lhx_gpu_read_pixels(gpu,image));
#else
   assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&options)==1);
#endif
   unsigned logo=column(w,h,110*s),world=column(w,h,210*s);
   fprintf(stderr,"scale=%u context=%u logo=%u world=%u\n",s,context,logo,world);
   assert(abs((int)logo-20*(int)s)<=3);
   assert(abs((int)world-20)<=3);
   /* Actual captured gameplay uses: seams, cables, site/ground markings,
    * aircraft outlines/hardware. None supplies an authored stroke width. */
   memcpy(reference,image,w*h*sizeof(*image));
   const char *world_models[]={"hangar","crane","oilbase","city","apache","lhx"};
   for(unsigned name=0;name<sizeof world_models/sizeof *world_models;name++){
   strcpy(meshes[0].model_name,world_models[name]);
#ifdef TEST_GPU
   assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);
   assert(lhx_gpu_read_pixels(gpu,image));
#else
   assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&options)==1);
#endif
   assert(abs((int)column(w,h,110*s)-20)<=3);
   for(unsigned y=100*s;y<140*s;y++)for(unsigned x=180*s;x<240*s;x++)
    assert(image[y*w+x]==reference[y*w+x]);
   }
   strcpy(meshes[0].model_name,"logo");
  }
  /* AA-off paths preserve the same width independent of model identity. */
  options.antialiasing=0;options.shadows=0;
#ifdef TEST_GPU
  assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);assert(lhx_gpu_read_pixels(gpu,image));
#else
  assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&options)==1);
#endif
  memcpy(reference,image,w*h*sizeof(*image));strcpy(meshes[0].model_name,"lhx");
#ifdef TEST_GPU
  assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);assert(lhx_gpu_read_pixels(gpu,image));
#else
  assert(lhx_scene_render_with_options(&frame,palette,image,w,h,&options)==1);
#endif
  assert(!memcmp(reference,image,w*h*sizeof(*image)));
  strcpy(meshes[0].model_name,"logo");options.antialiasing=1;
  materials(s
#ifdef TEST_GPU
   ,gpu
#endif
  );
#ifdef TEST_GPU
  lhx_gpu_destroy(gpu);
#endif
 }
#ifdef TEST_GPU
 SDL_DestroyWindow(window);SDL_Quit();
#endif
 lhx_scene_renderer_reset();
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class TitleLines(unittest.TestCase):
    def check_backend(self, gpu):
        with tempfile.TemporaryDirectory(prefix="lhx-title-lines-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            source = work / "check.c"
            source.write_text(HARNESS)
            flags = []
            files = ["scene_renderer.c", "scene_motion.c", "scene_relief.c", "scene_props.c", "scene_texture.c"]
            if gpu:
                packages = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2", "gl"], capture_output=True, text=True)
                if packages.returncode:
                    self.skipTest("Optional OpenGL development libraries unavailable")
                flags = ["-DTEST_GPU", "-DLHX_HAVE_OPENGL=1", *shlex.split(packages.stdout)]
                files += ["scene_gpu.c"]
            binary = work / "check"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "runtime"),
                "-I", str(ROOT / "third_party/pcrecomp/runtime"), str(source),
                *[str(ROOT / "runtime" / f) for f in files], *flags, "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(binary)], capture_output=True, text=True,
                env=os.environ | {"SDL_VIDEODRIVER": "offscreen"}, timeout=30)
            if run.returncode == 77:
                self.skipTest("Offscreen OpenGL context unavailable")
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)

    def test_cpu_title_and_world_widths(self):
        self.check_backend(False)

    @unittest.skipUnless(shutil.which("pkg-config"), "pkg-config required")
    def test_gpu_title_and_world_widths(self):
        self.check_backend(True)


if __name__ == "__main__":
    unittest.main()
