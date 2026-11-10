"""Gun tracer stroke visibility without changing scenery, palette or occlusion."""
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
#include "scene_composite.h"
#ifdef TEST_GPU
#include "scene_gpu.h"
#endif
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t image[2560*1600],reference[2560*1600];
static uint8_t palette[768],vga[64000];
static unsigned scale,width,height;
static LhxSceneVertex screen(float x,float y,float z){
 return (LhxSceneVertex){(x-160)*z/128,(100-y)*z/120,z};
}
static LhxSceneVertex vertices[4];
static uint16_t ends[]={0,1};
static LhxScenePrimitive primitive={.kind=1,.color=50,.count=2,.indices=ends};
static LhxSceneMesh meshes[]={
 {.vertices=vertices,.vertex_count=2,.primitives=&primitive,.primitive_count=1,.layer=2,.model_name="bullet"},
 {.vertices=vertices+2,.vertex_count=2,.primitives=&primitive,.primitive_count=1,.layer=2,.model_name="hangar"}};
static LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.meshes=meshes,.mesh_count=2};
static LhxSceneFrame frame={.views=&view,.view_count=1};
static LhxSceneRenderOptions options;
#ifdef TEST_GPU
static LhxGpu *gpu;
static uint32_t overlay[64000];
#endif
static void draw(void){
#ifdef TEST_GPU
 lhx_scene_overlay(&frame,vga,palette,overlay);
 assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==1);
 assert(lhx_gpu_read_pixels(gpu,image));
#else
 assert(lhx_scene_render_with_options(&frame,palette,image,width,height,&options)==1);
 lhx_scene_composite(&frame,vga,palette,image,width,height);
#endif
}
static unsigned column(unsigned x,unsigned y0,unsigned y1){
 unsigned energy=0;for(unsigned y=y0*scale;y<y1*scale;y++)energy+=image[y*width+x*scale]&255;
 return energy;
}
int main(void){
 /* Dim grey avoids the silhouette filter's high-contrast threshold. */
 palette[150]=palette[151]=palette[152]=5;
 palette[6]=0;palette[7]=63;palette[8]=0;
 memset(vga,255,sizeof vga);
 vertices[0]=screen(90,70.125,100);vertices[1]=screen(130,70.125,100);
 vertices[2]=screen(190,120.125,100);vertices[3]=screen(230,120.125,100);
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
#ifdef TEST_GPU
 if(SDL_Init(SDL_INIT_VIDEO))return 77;
 lhx_gpu_attributes();
 SDL_Window*window=SDL_CreateWindow("Gun tracer test",0,0,320,200,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!window){SDL_Quit();return 77;}
#endif
 unsigned scales[]={1,3,8};
 for(unsigned k=0;k<3;k++){
  scale=scales[k];width=320*scale;height=200*scale;
#ifdef TEST_GPU
  gpu=lhx_gpu_create(window,width,height,1,0);assert(gpu);
#endif
  for(unsigned aa=0;aa<2;aa++){
   options=(LhxSceneRenderOptions){.antialiasing=aa};draw();
   unsigned tracer=column(110,60,80),world=column(210,110,130);
   fprintf(stderr,"scale=%u aa=%u tracer=%u scenery=%u\n",scale,aa,tracer,world);
   if(aa){assert(abs((int)tracer-20*(int)scale)<=3);assert(abs((int)world-20)<=3);}
   memcpy(reference,image,width*height*sizeof *image);
   strcpy(meshes[0].model_name,"hangar");draw();
   if(!aa)assert(!memcmp(reference,image,width*height*sizeof *image));
   else if(scale>1)assert(tracer>column(110,60,80)*2);
   strcpy(meshes[0].model_name,"bullet");
   /* Palette changes remain dynamic; line illumination is independent of sun. */
   palette[150]=63;palette[151]=39;palette[152]=39;draw();
   memcpy(reference,image,width*height*sizeof *image);
   view.has_camera=1;options.lighting=1;draw();
   assert(!memcmp(reference,image,width*height*sizeof *image));
   palette[150]=palette[151]=palette[152]=5;view.has_camera=0;
  }
  /* A solid in front hides the widened tracer; reverse depth reveals it. */
  LhxSceneVertex wall[]={screen(80,60,50),screen(140,60,50),screen(140,80,50),screen(80,80,50)};
  uint16_t ring[]={0,1,2,3};LhxScenePrimitive face={.kind=0,.color=2,.count=4,.indices=ring};
  LhxSceneMesh solids[]={meshes[0],{.vertices=wall,.vertex_count=4,.primitives=&face,.primitive_count=1,.layer=2}};
  view.meshes=solids;options=(LhxSceneRenderOptions){.antialiasing=1};draw();
  memcpy(reference,image,width*height*sizeof *image);
  solids[0].primitive_count=0;draw();assert(!memcmp(reference,image,width*height*sizeof *image));
  solids[0].primitive_count=1;
  for(unsigned i=0;i<4;i++){wall[i].x*=4;wall[i].y*=4;wall[i].z*=4;}
  draw();assert(memcmp(reference,image,width*height*sizeof *image));
  /* The original pixel-art overlay wins after tracer width/depth rendering. */
  vga[70*320+110]=2;draw();
  for(unsigned y=70*scale;y<71*scale;y++)for(unsigned x=110*scale;x<111*scale;x++)
   assert(image[y*width+x]==0xff00ff00);
  vga[70*320+110]=255;
  /* A clipped scene window must not leak the stroke into the cockpit area. */
  view.y1=70;draw();
  for(unsigned y=70*scale;y<80*scale;y++)for(unsigned x=90*scale;x<130*scale;x++)
   assert(image[y*width+x]==0xff000000); /* palette255 outside scene */
  view.y1=200;view.meshes=meshes;
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
class ProjectileRendering(unittest.TestCase):
    def check_backend(self, gpu):
        flags = []
        if gpu:
            if not shutil.which("pkg-config"):
                self.skipTest("pkg-config required for OpenGL")
            packages = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2", "gl"],
                                      capture_output=True, text=True)
            if packages.returncode:
                self.skipTest("Optional OpenGL libraries unavailable")
            flags = shlex.split(packages.stdout)
        with tempfile.TemporaryDirectory(prefix="lhx-projectile-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            source = work / "check.c"
            source.write_text(HARNESS)
            sources = ["scene_renderer.c", "scene_motion.c", "scene_relief.c",
                       "scene_props.c", "scene_texture.c", "scene_composite.c"]
            if gpu:
                sources.append("scene_gpu.c")
            command = ["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                       "-I", str(work), "-I", str(ROOT / "runtime"),
                       "-I", str(ROOT / "third_party/pcrecomp/runtime"), str(source),
                       *[str(ROOT / "runtime" / path) for path in sources]]
            if gpu:
                command += ["-DTEST_GPU", "-DLHX_HAVE_OPENGL=1"]
            command += [*flags, "-lm", "-o", str(work / "check")]
            built = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(work / "check")], capture_output=True, text=True,
                                    timeout=45, env=os.environ | {
                                        "SDL_VIDEODRIVER": "offscreen",
                                        "SDL_AUDIODRIVER": "dummy"})
            if gpu and result.returncode == 77:
                self.skipTest("Offscreen OpenGL context unavailable")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_cpu_tracer_visibility_palette_depth_and_overlay(self):
        self.check_backend(False)

    def test_gpu_tracer_visibility_palette_depth_and_overlay(self):
        self.check_backend(True)


if __name__ == "__main__":
    unittest.main()
