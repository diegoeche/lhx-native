"""Exercise actual OpenGL shaders/FBOs with an offscreen context when available."""
from pathlib import Path
import os
import json
import shlex
import shutil
import subprocess
import tempfile
import time
import unittest
from test_scenery import prepare_fixture

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_gpu.h"
#include "scene_composite.h"
#include "scene_atmosphere.h"
#include "scene_props.h"
#include "scene_sky.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t image[960*600],reference[960*600],overlay[64000];
static uint8_t palette[768],vga[64000];
static LhxSceneView view={.x1=320,.y1=200,.cx=160,.cy=100,.focus=128};
static LhxSceneFrame frame={.views=&view,.view_count=1};
static LhxSceneRenderOptions options={.sphere_depth=1,.antialiasing=1};
static LhxGpu *gpu;
static void draw(void){
 lhx_scene_overlay(&frame,vga,palette,overlay);
 assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)>=0);
 assert(lhx_gpu_read_pixels(gpu,image));
}
static uint32_t pixel(unsigned x,unsigned y){return image[(y*3+1)*960+x*3+1];}
int main(void){
 if(SDL_Init(SDL_INIT_VIDEO)<0)return 77;
 lhx_gpu_attributes();
 SDL_Window *window=SDL_CreateWindow("LHX GPU test",0,0,320,200,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!window){SDL_Quit();return 77;}
 gpu=lhx_gpu_create(window,960,600,1,1);assert(gpu);
 palette[3]=63;palette[7]=63;palette[11]=63;
 palette[21]=palette[22]=palette[23]=63;
 palette[765]=palette[766]=palette[767]=63;
 memset(vga,255,sizeof vga);
 uint16_t ring[]={0,1,2,3},index=0;
 LhxSceneVertex wall[]={{-80,-80,200},{80,-80,200},{80,80,200},{-80,80,200}};
 LhxSceneVertex center={0,0,100};
 LhxScenePrimitive face={.kind=0,.count=4,.indices=ring,.color=3};
 LhxScenePrimitive sphere={.kind=3,.count=1,.indices=&index,.color=1,.radius=10};
 LhxSceneMesh smoke={.vertices=&center,.vertex_count=1,.primitives=&sphere,.primitive_count=1,.layer=0};
 LhxSceneMesh solid={.vertices=wall,.vertex_count=4,.primitives=&face,.primitive_count=1,.layer=2};
 LhxSceneMesh meshes[]={smoke,solid};view.meshes=meshes;view.mesh_count=2;
 draw();assert(pixel(160,100)==0xffff0000); /* Near smoke covers farther wall. */
 memcpy(reference,image,sizeof image);
 SDL_SetWindowSize(window,500,400);lhx_gpu_present(gpu);
 assert(lhx_gpu_read_pixels(gpu,image));assert(!memcmp(reference,image,sizeof image));
 center.z=300;draw();assert(pixel(160,100)==0xff0000ff);
 center.z=100;options.sphere_depth=0;draw();assert(pixel(160,100)==0xff0000ff);
 options.sphere_depth=1;
 for(unsigned order=0;order<2;order++){
  meshes[order]=smoke;meshes[1-order]=solid;draw();assert(pixel(160,100)==0xffff0000);
 }
 /* Original HUD pixels stay exact and nearest-neighbour over the MSAA scene. */
 vga[100*320+160]=2;draw();
 for(unsigned y=300;y<303;y++)for(unsigned x=480;x<483;x++)assert(image[y*960+x]==0xff00ff00);
 vga[100*320+160]=255;
 /* Authored one-sided faces use the same winding as the CPU renderer. */
 view.meshes=&solid;view.mesh_count=1;face.one_sided=1;
 draw();assert(pixel(160,100)==0xff000000);
 uint16_t reverse[]={3,2,1,0};face.indices=reverse;
 draw();assert(pixel(160,100)==0xff0000ff);
 face.one_sided=0;face.indices=ring;
 /* Constant texture, then same storage with a new revision: no stale uploads. */
 uint32_t texels[]={0xff00ff00,0xff00ff00,0xff00ff00,0xff00ff00};
 LhxTexture texture={.count=1,.levels={{2,2,texels}},.revision=1};
 LhxSceneUV uv[]={{0,0},{1,0},{1,1},{0,1}};solid.texture=&texture;solid.uvs=uv;
 face.color=7;draw();assert(pixel(160,100)==0xff00ff00);
 for(unsigned i=0;i<4;i++)texels[i]=0xffff0000;
 texture.revision++;
 draw();assert(pixel(160,100)==0xffff0000);
 solid.texture=NULL;solid.uvs=NULL;
 /* A tilted horizon matches the CPU away from the subpixel boundary. */
 view.mesh_count=0;view.horizon=(LhxSceneHorizon){.colors={1,2},.two_colors=1,.x=159.31,.y=98.17,.nx=.7,.ny=.3};
 options.antialiasing=0;
 draw();assert(lhx_scene_render_with_options(&frame,palette,reference,960,600,NULL)==1);
 unsigned same=0;for(unsigned i=0;i<960*600;i++)same+=reference[i]==image[i];assert(same>960*600-10);
 options.antialiasing=1;
 draw();unsigned blended=0;
 for(unsigned i=0;i<960*600;i++)blended+=image[i]!=0xffff0000 && image[i]!=0xff00ff00;
 assert(blended>100);
 /* Ground ray intersection, inverse basis, UVs and near/far texture selection. */
 uint32_t green=0xff00ff00,blue=0xff0000ff;
 LhxTerrain terrain={.near={.texture={.count=1,.levels={{1,1,&green}}},.bounds={-1000,1000,0,1000}},
                     .far={.texture={.count=1,.levels={{1,1,&blue}}},.bounds={-10000,10000,0,10000}}};
 view.has_camera=1;view.camera[1]=100;view.world_to_view[0][0]=view.world_to_view[1][1]=view.world_to_view[2][2]=1;view.sky_color=7;
 options.terrain=&terrain;draw();assert(pixel(160,150)==green);assert(pixel(160,110)==blue);
 /* The imagery's far edge and distant objects meet a shared hazy horizon. */
 options.atmosphere=1;draw();assert(pixel(160,100)==lhx_haze_color(0xffffffff));
 view.sky_color=0;draw();assert(pixel(160,100)==lhx_haze_color(0xff000000));view.sky_color=7;
 options.terrain=NULL;view.meshes=&solid;view.mesh_count=1;
 for(unsigned i=0;i<4;i++){wall[i].x*=1250;wall[i].y*=1250;wall[i].z*=1250;}
 draw();uint32_t expected=lhx_atmosphere_mix(0xffffffff,lhx_haze_color(0xffffffff),exp(-1));
 for(unsigned shift=0;shift<=16;shift+=8)
  assert(abs((int)((pixel(160,100)>>shift)&255)-(int)((expected>>shift)&255))<=1);
 for(unsigned i=0;i<4;i++){wall[i].x/=1250;wall[i].y/=1250;wall[i].z/=1250;}
 options.atmosphere=0;view.mesh_count=0;
 /* Imported static art uses the existing object's pose; disabling restores it. */
 LhxProps *scenery=lhx_props_load(getenv("LHX_SCENERY_FIXTURE"));assert(scenery);
 strcpy(solid.model_name,"hangar");solid.has_placement=1;solid.world_origin[1]=80;
 for(unsigned i=0;i<3;i++){
  solid.model_bounds[i*2]=-80;solid.model_bounds[i*2+1]=80;solid.model_to_view[i][i]=1;
 }
 solid.model_to_view[1][3]=-20;solid.model_to_view[2][3]=200;
 options.props=scenery;view.meshes=&solid;view.mesh_count=1;draw();assert(pixel(160,100)==0xffff0000);
 /* Cached meshes retain transforms and flat lighting, without per-frame uploads. */
 options.lighting=1;
 assert(lhx_scene_render_with_options(&frame,palette,reference,960,600,&options)==1);
 draw();unsigned compared=0;
 for(unsigned y=240;y<330;y+=9)for(unsigned x=420;x<540;x+=9){
  unsigned k=y*960+x;if((reference[k]>>16&255)>50 && !(reference[k]&0xffff)){
   assert(abs((int)(image[k]>>16&255)-(int)(reference[k]>>16&255))<=2);compared++;
  }
 }assert(compared>20);
 draw();draw(); /* Statistics below confirm that these reuse the original upload. */
 options.lighting=0;
 options.props=NULL;draw();assert(pixel(160,100)==0xffffffff);view.mesh_count=0;
 /* Panoramas follow original day/night sky brightness and preserve instruments. */
 uint32_t red_sky[]={0xffff0000,0xffff0000},blue_sky[]={0xff0000ff,0xff0000ff};
 LhxSky skies={.day={.count=1,.levels={{2,1,red_sky}}},.night={.count=1,.levels={{2,1,blue_sky}}}};
 options.sky=&skies;draw();assert(pixel(160,20)==0xffc70000);
 uint32_t gradient[]={0xffff0000,0xffff0000,0xff0000ff,0xff0000ff};
 skies.day.levels[0]=(LhxTextureLevel){4,1,gradient};skies.day.revision++;
 draw();assert((pixel(160,20)&255)>90 && ((pixel(160,20)>>16)&255)>90);
 view.world_to_view[0][0]=view.world_to_view[2][2]=0;
 view.world_to_view[0][2]=1;view.world_to_view[2][0]=-1;
 draw();assert(pixel(160,20)==0xffc70000);
 view.world_to_view[0][0]=view.world_to_view[2][2]=1;
 view.world_to_view[0][2]=view.world_to_view[2][0]=0;
 view.sky_color=0;draw();assert(pixel(160,20)==0xff0000c7);view.sky_color=7;
 options.sky=NULL;
 options.terrain=NULL;view.has_camera=0;
 /* Index255 is opaque outside scene windows; instrument views are scissored. */
 view=(LhxSceneView){.x0=30,.y0=20,.x1=90,.y1=70,.cx=60,.cy=45,.focus=128};
 draw();assert(pixel(40,30)==0xff000000);assert(pixel(29,30)==0xffffffff);assert(pixel(40,70)==0xffffffff);
 frame.view_count=0;draw();assert(pixel(40,30)==0xffffffff);
 /* Bounded rejection without touching absent mesh memory. */
 frame.view_count=1;view.mesh_count=65537;
 assert(lhx_gpu_draw(gpu,&frame,palette,overlay,&options)==-1);
 view.mesh_count=0;
 lhx_gpu_report(gpu,stdout);fputc('\n',stdout);
 lhx_gpu_destroy(gpu);
 /* AA-disabled creation uses a single sample and the same complete path. */
 gpu=lhx_gpu_create(window,960,600,0,0);assert(gpu);options.antialiasing=0;
 view=(LhxSceneView){.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.meshes=&solid,.mesh_count=1};
 draw();assert(pixel(160,100)==0xffffffff);
 lhx_gpu_destroy(gpu);lhx_props_free(scenery);lhx_scene_renderer_reset();SDL_DestroyWindow(window);SDL_Quit();
}
'''


@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"), "C toolchain required")
class SceneGpu(unittest.TestCase):
    def test_offscreen_projection_depth_textures_and_overlay(self):
        packages = subprocess.run(["pkg-config", "--cflags", "--libs", "sdl2", "gl"],
                                  capture_output=True, text=True)
        if packages.returncode:
            self.skipTest("Optional OpenGL development libraries unavailable")
        with tempfile.TemporaryDirectory(prefix="lhx-gpu-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            source = work / "check.c"
            source.write_text(HARNESS)
            scenery = prepare_fixture(work / "scenery")
            binary = work / "check"
            compile_result = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra",
                "-Werror", "-DLHX_HAVE_OPENGL=1", "-I", str(work),
                "-I", str(ROOT / "runtime"), "-I", str(ROOT / "third_party/pcrecomp/runtime"),
                str(source), str(ROOT / "runtime/scene_gpu.c"),
                str(ROOT / "runtime/scene_renderer.c"), str(ROOT / "runtime/scene_motion.c"), str(ROOT / "runtime/scene_relief.c"), str(ROOT / "runtime/scene_props.c"), str(ROOT / "runtime/scene_texture.c"),
                str(ROOT / "runtime/scene_composite.c"), *shlex.split(packages.stdout),
                "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0, compile_result.stderr)
            started = time.monotonic()
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                env=os.environ | {"SDL_VIDEODRIVER": os.environ.get("LHX_TEST_VIDEO_DRIVER", "offscreen"),
                                  "LHX_SCENERY_FIXTURE": str(scenery)}, timeout=30)
            if result.returncode == 77:
                self.skipTest("Offscreen OpenGL context unavailable")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            report = json.loads(result.stdout)
            self.assertGreater(report["timed_draws"], 0)
            self.assertGreater(report["cached_prop_instances"], 3)
            self.assertGreater(report["prop_uploaded_vertices"], 0)
            self.assertGreater(report["prop_drawn_vertices"], report["prop_uploaded_vertices"] * 3)
            self.assertLessEqual(report["device_draw_max_us"],
                                 (time.monotonic() - started + 1) * 1_000_000)
            self.assertEqual(report["framebuffer_readbacks"], report["submitted_draws"])


if __name__ == "__main__":
    unittest.main()
