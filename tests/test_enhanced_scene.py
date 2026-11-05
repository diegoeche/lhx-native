"""Independent checks for relief placement, shadow blending and model-space detail."""
import importlib.util
import json
import os
from pathlib import Path
import shlex
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location('import_relief', ROOT/'tools/import_relief.py')
RELIEF = importlib.util.module_from_spec(SPEC); SPEC.loader.exec_module(RELIEF)

HARNESS = r'''
#include "scene_renderer.h"
#include "scene_geometry.h"
#include "scene_relief.h"
#include "scene_surface.h"
#include "scene_sky.h"
#ifdef GPU
#include "scene_gpu.h"
#endif
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t pixels[64000],saved[64000],baseline[64000];
static uint8_t palette[768];
static LhxSceneView view;
static LhxSceneFrame frame;
static LhxSceneRenderOptions options;
#ifdef GPU
static LhxGpu *gpu;
#endif
static void setup(void){
 memset(palette,0,sizeof palette);palette[3]=palette[4]=palette[5]=63;
 palette[7]=63; /* color2 green */
 view=(LhxSceneView){.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.has_camera=1,.sky_color=1,.ground_color=1};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 frame=(LhxSceneFrame){.views=&view,.view_count=1};options=(LhxSceneRenderOptions){0};
}
static void draw(void){
#ifdef GPU
 static const uint32_t transparent[64000]={0};
 assert(lhx_gpu_draw(gpu,&frame,palette,transparent,&options)==1);assert(lhx_gpu_read_pixels(gpu,pixels));
#else
 assert(lhx_scene_render_with_options(&frame,palette,pixels,320,200,&options)==1);
#endif
}
static unsigned difference(const uint32_t *a,const uint32_t *b){unsigned n=0;for(unsigned i=0;i<64000;i++)n+=a[i]!=b[i];return n;}
static void placement(LhxSceneMesh *m,const char *name,double x,double y,double z){
 m->has_placement=1;snprintf(m->model_name,sizeof m->model_name,"%s",name);
 m->world_origin[0]=x;m->world_origin[1]=y;m->world_origin[2]=z;
 for(unsigned i=0;i<3;i++){m->model_to_view[i][i]=1;m->model_bounds[i*2]=-80;m->model_bounds[i*2+1]=80;}
 m->model_to_view[0][3]=x-view.camera[0];m->model_to_view[1][3]=y-view.camera[1];m->model_to_view[2][3]=z-view.camera[2];
}
static void paint(void){
 setup();uint16_t ids[]={0,1,2,3};LhxSceneVertex vertices[]={{-80,80,200},{80,80,200},{80,-80,200},{-80,-80,200}};
 LhxScenePrimitive p={.kind=0,.count=4,.indices=ids,.color=2};
 LhxSceneMesh m={.vertices=vertices,.vertex_count=4,.primitives=&p,.primitive_count=1,.layer=2};
 placement(&m,"apache",0,0,200);view.meshes=&m;view.mesh_count=1;
 draw();assert(pixels[100*320+160]==0xff00ff00);memcpy(baseline,pixels,sizeof pixels);
 options.military_paint=1;draw();assert(pixels[100*320+160]==lhx_military_paint(0xff00ff00));
 memcpy(saved,pixels,sizeof pixels);options.surface_detail=1;draw();assert(difference(saved,pixels)>500);
 /* At the same local point on the translated model, the pattern stays fixed. */
 uint32_t paint=pixels[100*320+160];
 for(unsigned i=0;i<4;i++)vertices[i].x+=25;
 m.model_to_view[0][3]+=25;m.world_origin[0]+=25;
 draw();assert(pixels[100*320+176]==paint);
 memcpy(saved,pixels,sizeof pixels);strcpy(m.model_name,"m1");draw();assert(difference(saved,pixels)>100);
 options=(LhxSceneRenderOptions){0};for(unsigned i=0;i<4;i++)vertices[i].x-=25;draw();
 assert(!memcmp(baseline,pixels,sizeof pixels));
}
static void shadows(void){
 setup();view.camera[1]=100;view.horizon.colors[0]=1;
 uint16_t ids[]={0,1,2,3};LhxSceneVertex wall[]={{0,-100,200},{0,-20,200},{40,-20,200},{40,-100,200}};
 LhxScenePrimitive p={.kind=0,.count=4,.indices=ids,.color=2};
 LhxSceneMesh meshes[2]={{.vertices=wall,.vertex_count=4,.primitives=&p,.primitive_count=1,.layer=2}};
 placement(meshes,"hangar",20,0,200);meshes[0].model_bounds[2]=0;meshes[0].model_bounds[3]=80;
 view.meshes=meshes;view.mesh_count=1;draw();memcpy(baseline,pixels,sizeof pixels);
 options.shadows=1;draw();memcpy(saved,pixels,sizeof pixels);
 unsigned dark=0;for(unsigned i=0;i<64000;i++)if(pixels[i]!=baseline[i] && baseline[i]==0xffffffff){
  /* Drivers may allocate more samples than the requested minimum: edge
     samples can be partly covered, but interiors must darken exactly once. */
  assert((pixels[i]&255)>=126);if((pixels[i]&255)<=129)dark++;
 }
 assert(dark>100);
 /* Identical overlapping casters darken once; object surfaces stay opaque. */
 meshes[1]=meshes[0];view.mesh_count=2;draw();assert(!memcmp(saved,pixels,sizeof pixels));
 assert(pixels[65*320+165]==0xffffffff); /* Sky above the ground. */
 strcpy(meshes[0].model_name,"apache");view.mesh_count=1;draw();assert(!memcmp(baseline,pixels,sizeof pixels));
 strcpy(meshes[0].model_name,"hangar");meshes[0].world_origin[1]=100;draw();assert(!memcmp(baseline,pixels,sizeof pixels));
 options.shadows=0;draw();assert(!memcmp(baseline,pixels,sizeof pixels));
}
static void rotors(void){
 setup();LhxSceneVertex v[]={{0,0,200},{70,0,200},{0,70,200},{-70,0,200},{0,-70,200}};
 uint16_t idx[4][3]={{0,1,2},{0,2,3},{0,3,4},{0,4,1}};LhxScenePrimitive p[4];
 for(unsigned i=0;i<4;i++)p[i]=(LhxScenePrimitive){.kind=0,.count=3,.indices=idx[i],.color=2,.stipple=1};
 LhxSceneMesh m={.vertices=v,.vertex_count=5,.primitives=p,.primitive_count=4,.layer=2};
 placement(&m,"apache",0,0,200);view.meshes=&m;view.mesh_count=1;
 draw();memcpy(baseline,pixels,sizeof pixels);options.rotor_blades=1;draw();memcpy(saved,pixels,sizeof pixels);
 assert(difference(baseline,pixels)>100);options.time_seconds=.037;draw();assert(difference(saved,pixels)>100);
 options.rotor_blades=0;draw();assert(!memcmp(baseline,pixels,sizeof pixels));
 strcpy(m.model_name,"b2");options.rotor_blades=1;draw();assert(!memcmp(baseline,pixels,sizeof pixels));
}
static void runway(void){
 LhxTexture a={0},b={0};assert(lhx_texture_runway_detail(&a,6400,512,0));
 assert(lhx_texture_runway_detail(&b,6400,512,90));unsigned different=0;
 for(unsigned y=0;y<2048;y++)for(unsigned x=0;x<256;x++)if(a.levels[0].pixels[y*256+x]!=b.levels[0].pixels[y*256+x]){
  different++;assert(y<200 || y>1800);}
 assert(different>100);assert(!lhx_texture_runway_detail(&a,NAN,512,0));
 lhx_texture_free(&a);lhx_texture_free(&b);
}
static void target_sky(void){
 setup();view.has_camera=0;view.horizon.colors[0]=1;
 uint32_t red[8];for(unsigned i=0;i<8;i++)red[i]=0xffff0000;
 LhxSky sky={.day={.count=1,.levels={{4,2,red}}},.night={.count=1,.levels={{4,2,red}}}};
 options.sky=&sky;draw();assert(pixels[25*320+160]==0xffffffff); /* attitude: no world objects */
 uint16_t ids[]={0,1,2};LhxSceneVertex v[]={{-10,-10,200},{10,-10,200},{0,10,200}};
 LhxScenePrimitive p={.kind=0,.count=3,.indices=ids,.color=2};
 LhxSceneMesh m={.vertices=v,.vertex_count=3,.primitives=&p,.primitive_count=1,.layer=2};
 placement(&m,"hangar",0,0,200);view.meshes=&m;view.mesh_count=1;
 draw();assert(pixels[25*320+160]==0xffc70000);
 options.sky=NULL;draw();assert(pixels[25*320+160]==0xffffffff);
}
static void terrain_depth(void){
 setup();view.camera[1]=100;
 uint32_t red[]={0xffff0000};float heights[]={0,0,0,0};
 LhxTerrainVertex ground[]={
  {-1000,0,50,0,0,1},{1000,0,50,1,0,1},{-1000,300,300,0,1,1},{1000,300,300,1,1,1}};
 uint32_t indices[]={0,2,1,1,2,3};
 LhxTerrain t={.near={.texture={.count=1,.levels={{1,1,red}}},.bounds={-1000,1000,0,1000},
  .source_n=4,.step=2,.n=2,.heights=heights,.vertices=ground,.indices=indices,.vertex_count=4,.index_count=6,.relief_revision=99}};
 t.far=t.near;
 uint16_t ring[]={0,1,2,3};LhxSceneVertex wall[]={{-50,-50,500},{50,-50,500},{50,50,500},{-50,50,500}};
 LhxScenePrimitive p={.kind=0,.count=4,.indices=ring,.color=2};
 LhxSceneMesh m={.vertices=wall,.vertex_count=4,.primitives=&p,.primitive_count=1,.layer=2};
 placement(&m,"hangar",0,100,500);view.meshes=&m;view.mesh_count=1;
 options.terrain=&t;draw();assert(pixels[100*320+160]==0xff00ff00);
 options.terrain_relief=1;draw();assert(pixels[100*320+160]==0xffff0000);
 options.terrain_relief=0;draw();assert(pixels[100*320+160]==0xff00ff00);
}
static void terrain_drape(const char *path){
 setup();view.camera[1]=50;view.camera[2]=-100;
 uint32_t blue=0xff0000ff;
 LhxTerrain t={.near={.bounds={-20,20,0,40}},.far={.bounds={-20,20,0,40}}};
 assert(lhx_relief_load(&t.near,path));assert(lhx_relief_load(&t.far,path));
 t.near.texture=(LhxTexture){.count=1,.levels={{1,1,&blue}}};t.far.texture=t.near.texture;
 uint16_t ids[]={0,1,2,3};LhxSceneVertex vertices[]={{-15,-50,115},{5,-50,115},{5,-50,135},{-15,-50,135}};
 LhxScenePrimitive p={.kind=0,.count=4,.indices=ids,.color=2};
 LhxSceneMesh m={.vertices=vertices,.vertex_count=4,.primitives=&p,.primitive_count=1,.layer=1};
 placement(&m,"road",0,0,0);view.meshes=&m;view.mesh_count=1;
 options.terrain=&t;options.terrain_relief=1;
 assert(lhx_scene_render_with_options(&frame,palette,saved,320,200,&options)==1);
 draw();unsigned compared=0;
 for(unsigned y=1;y<199;y++)for(unsigned x=1;x<319;x++){
  unsigned k=y*320+x;
  if(saved[k]==0xff00ff00 && saved[k-1]==saved[k] && saved[k+1]==saved[k] && saved[k-320]==saved[k] && saved[k+320]==saved[k]){
   assert(pixels[k]==0xff00ff00);compared++;
  }
 }
 assert(compared>30);memcpy(saved,pixels,sizeof pixels);options.terrain_relief=0;draw();assert(difference(saved,pixels)>50);
 lhx_relief_free(&t.near);lhx_relief_free(&t.far);
}
static unsigned triangles;static double first_y;
static int emit_view(void *c,const LhxSceneView *v,const uint32_t col[256],float day){(void)c;(void)v;(void)col;(void)day;return 1;}
static int emit_layer(void *c,int layer){(void)c;(void)layer;return 1;}
static int emit_triangle(void *c,const LhxSceneVertex p[3],const LhxSceneUV *uv,const LhxTexture *t,uint32_t color,float tint,int one,int stipple,float bias){
 (void)c;(void)uv;(void)t;(void)color;(void)tint;(void)one;(void)stipple;(void)bias;if(!triangles)first_y=p[0].y;triangles++;return 1;}
static int emit_line(void *c,LhxSceneVertex a,LhxSceneVertex b,uint32_t col,int stipple){(void)c;(void)a;(void)b;(void)col;(void)stipple;return 1;}
static int emit_particle(void *c,LhxSceneVertex a,float r,uint32_t col,int stipple){(void)c;(void)a;(void)r;(void)col;(void)stipple;return 1;}
static int emit_end(void *c){(void)c;return 1;}
static void relief(const char *path,const char *bad){
 setup();LhxTerrain t={.near={.bounds={-20,20,0,40}},.far={.bounds={-20,20,0,40}}};
 assert(lhx_relief_load(&t.near,path));assert(lhx_relief_load(&t.far,path));assert(lhx_relief_ready(&t));
 assert(fabs(lhx_relief_height(&t,-5,25)-15)<1e-6);assert(fabs(lhx_relief_height(&t,0,20)-27.5)<1e-6);
 assert(lhx_relief_height(&t,999,0)==0);assert(lhx_relief_height(&t,NAN,0)==0);
 assert(!lhx_relief_load(&t.near,bad));assert(fabs(lhx_relief_height(&t,-5,25)-15)<1e-6);
 view.camera[0]=-5;view.camera[1]=100;view.camera[2]=25;
 uint16_t idx[]={0,1,2};LhxScenePrimitive p={.kind=0,.count=3,.indices=idx,.color=1};
 LhxSceneVertex vertices[]={{0,0,100},{5,0,100},{0,5,100}};
 LhxSceneMesh m={.vertices=vertices,.vertex_count=3,.primitives=&p,.primitive_count=1,.layer=2};
 placement(&m,"hangar",0,100,20);view.meshes=&m;view.mesh_count=1;
 options.terrain=&t;options.terrain_relief=1;
 const LhxSceneGeometrySink sink={emit_view,emit_layer,emit_triangle,emit_line,emit_particle,emit_end,NULL,NULL,NULL};
 triangles=0;assert(lhx_scene_emit_geometry(&frame,palette,&options,&sink,NULL)==1);
 assert(triangles==1 && fabs(first_y-12.5)<1e-6);
 strcpy(m.model_name,"hill1");triangles=0;assert(lhx_scene_emit_geometry(&frame,palette,&options,&sink,NULL)==1);assert(!triangles);
 options.terrain_relief=0;triangles=0;assert(lhx_scene_emit_geometry(&frame,palette,&options,&sink,NULL)==1);assert(triangles==1 && first_y==0);
 lhx_relief_free(&t.near);lhx_relief_free(&t.far);
}
int main(int argc,char **argv){
 assert(argc>=2);
#ifdef GPU
 if(SDL_Init(SDL_INIT_VIDEO)<0)return 77;
 lhx_gpu_attributes();
 SDL_Window *window=SDL_CreateWindow("Enhancement test",0,0,320,200,SDL_WINDOW_OPENGL|SDL_WINDOW_HIDDEN);
 if(!window){SDL_Quit();return 77;}gpu=lhx_gpu_create(window,320,200,0,0);assert(gpu);
#endif
 if(!strcmp(argv[1],"paint"))paint();else if(!strcmp(argv[1],"shadow"))shadows();
 else if(!strcmp(argv[1],"rotor"))rotors();else if(!strcmp(argv[1],"runway"))runway();
 else if(!strcmp(argv[1],"terrain-depth"))terrain_depth();
 else if(!strcmp(argv[1],"target-sky"))target_sky();
 else if(!strcmp(argv[1],"terrain-drape")){assert(argc==4);terrain_drape(argv[2]);}
 else if(!strcmp(argv[1],"relief")){assert(argc==4);relief(argv[2],argv[3]);}else assert(0);
#ifdef GPU
 lhx_gpu_destroy(gpu);SDL_DestroyWindow(window);SDL_Quit();
#endif
 lhx_scene_renderer_reset();return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'C compiler required')
class EnhancedScene(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix='lhx-enhanced-')
        cls.addClassCleanup(cls.temp.cleanup)
        cls.work = Path(cls.temp.name); source=cls.work/'check.c';source.write_text(HARNESS)
        (cls.work/'lifted.h').write_text('#include "cpu.h"\n')
        packages=subprocess.run(['pkg-config','--cflags','--libs','sdl2','gl'],capture_output=True,text=True)
        cls.gpu_available=packages.returncode==0
        cls.binaries={}
        for backend in ('cpu','gpu'):
            if backend=='gpu' and not cls.gpu_available:continue
            binary=cls.work/backend
            command=['cc','-std=c11','-O2','-Wall','-Wextra','-Werror','-I',str(ROOT/'runtime'),'-I',str(cls.work),'-I',str(ROOT/'third_party/pcrecomp/runtime'),str(source)]
            command += [str(ROOT/'runtime'/f'{name}.c') for name in ('scene_renderer','scene_props','scene_texture','scene_relief')]
            if backend=='gpu':command += ['-DGPU=1','-DLHX_HAVE_OPENGL=1',str(ROOT/'runtime/scene_gpu.c'),*shlex.split(packages.stdout)]
            command += ['-lm','-o',str(binary)]
            result=subprocess.run(command,capture_output=True,text=True)
            if result.returncode:raise AssertionError(result.stderr)
            cls.binaries[backend]=binary
        info={'heights':4,'bounds':[-20,20,0,40]};h=[0]*16;h[2]=10;h[8]=20;h[10]=40
        g=RELIEF.grid(info,struct.pack('<16h',*h));payload=RELIEF.geometry(g,[g],[],None)
        cls.good=cls.work/'good.lhr';cls.good.write_bytes(payload)
        cls.bad=cls.work/'bad.lhr';cls.bad.write_bytes(payload[:-3])

    def check(self,scenario,backend):
        if backend not in self.binaries:self.skipTest('OpenGL development files unavailable')
        result=subprocess.run([str(self.binaries[backend]),scenario,str(self.good),str(self.bad)],capture_output=True,text=True,timeout=30,
            env=os.environ|{'SDL_VIDEODRIVER':os.environ.get('LHX_TEST_VIDEO_DRIVER','offscreen')})
        if result.returncode==77:self.skipTest('Offscreen OpenGL unavailable')
        self.assertEqual(result.returncode,0,result.stderr)

    def test_cpu_paint_and_model_space_detail(self): self.check('paint','cpu')
    def test_gpu_paint_and_model_space_detail(self): self.check('paint','gpu')
    def test_cpu_shadow_overlap_and_aircraft_exclusion(self): self.check('shadow','cpu')
    def test_gpu_shadow_overlap_and_aircraft_exclusion(self): self.check('shadow','gpu')
    def test_cpu_rotors_and_disabled_baseline(self): self.check('rotor','cpu')
    def test_gpu_rotors_and_disabled_baseline(self): self.check('rotor','gpu')
    def test_cpu_ground_markings_follow_relief(self): self.check('terrain-drape','cpu')
    def test_gpu_ground_markings_follow_relief(self): self.check('terrain-drape','gpu')
    def test_cpu_target_sky_and_attitude_preservation(self): self.check('target-sky','cpu')
    def test_gpu_target_sky_and_attitude_preservation(self): self.check('target-sky','gpu')
    def test_cpu_terrain_occludes_distant_scenery(self): self.check('terrain-depth','cpu')
    def test_gpu_terrain_occludes_distant_scenery(self): self.check('terrain-depth','gpu')
    def test_runway_heading_and_bounds(self): self.check('runway','cpu')
    def test_relief_triangles_placement_and_original_hill_fallback(self): self.check('relief','cpu')

    def test_import_rejects_negative_heights_and_bad_lengths(self):
        info={'heights':4,'bounds':[-20,20,0,40]}
        for payload in (b'',struct.pack('<16h',*([-1]*16))):
            with self.assertRaises(ValueError):RELIEF.grid(info,payload)
