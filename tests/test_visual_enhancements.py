"""Analytical rendering checks and optional resource/theatre lifecycle tests."""
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include "scene_renderer.h"
#include "scene_assets.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static uint32_t pixels[320*200],saved[320*200];
static uint8_t palette[768];
static LhxSceneView view;
static LhxSceneFrame frame;
static LhxSceneRenderOptions options;
static void setup(void){
 memset(palette,0,sizeof palette);palette[3]=63;
 palette[12]=palette[13]=palette[14]=63;
 view=(LhxSceneView){.x1=320,.y1=200,.cx=160,.cy=100,.focus=128,.sky_color=4};
 for(unsigned i=0;i<3;i++)view.world_to_view[i][i]=1;
 frame=(LhxSceneFrame){.views=&view,.view_count=1};options=(LhxSceneRenderOptions){0};
}
static void render(void){assert(lhx_scene_render_with_options(&frame,palette,pixels,320,200,&options)==1);}
static LhxSceneVertex at(double x,double y,double z){
 return (LhxSceneVertex){(x-160)*z/128,(100-y)*z/120,z};}
static unsigned channel(uint32_t c,unsigned s){return(c>>s)&255;}
static void close_color(uint32_t a,uint32_t b,unsigned tolerance){
 for(unsigned s=0;s<=16;s+=8)assert(abs((int)channel(a,s)-(int)channel(b,s))<=(int)tolerance);}
static void sampling(void){
 uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
 LhxTexture t={.count=1,.levels={{2,2,NULL}}};
 t.levels[0].pixels=malloc(sizeof colors);assert(t.levels[0].pixels);
 memcpy(t.levels[0].pixels,colors,sizeof colors);
 assert(lhx_texture_sample(&t,.25,.25,1)==colors[0]);
 assert(lhx_texture_sample(&t,.75,.25,1)==colors[1]);
 assert(lhx_texture_sample(&t,.25,.75,1)==colors[2]);
 assert(lhx_texture_sample(&t,-10,-10,1)==colors[0]);
 close_color(lhx_texture_sample(&t,.5,.5,1),0xff808080,0);
 assert(lhx_texture_mipmaps(&t)&&t.count==2);
 assert(lhx_texture_sample(&t,.2,.8,100)==0xff808080);
 assert(lhx_texture_sample(&t,NAN,0,1)==0);
 lhx_texture_free(&t);assert(t.count==0);
}
static void perspective(void){
 setup();
 uint32_t colors[]={0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
 LhxTexture t={.count=1,.levels={{2,2,colors}}};
 LhxSceneVertex v[]={at(40,30,4),at(280,30,40),at(160,180,20)};
 LhxSceneUV uv[]={{0,0},{1,0},{0,1}};uint16_t indices[]={0,1,2};
 LhxScenePrimitive p={.kind=0,.color=1,.count=3,.indices=indices};
 LhxSceneMesh m={.vertices=v,.vertex_count=3,.primitives=&p,.primitive_count=1,
                 .layer=2,.texture=&t,.uvs=uv};view.meshes=&m;view.mesh_count=1;
 render();
 /* Independent screen barycentrics and inverse-depth UV at pixel centres. */
 for(unsigned y=65;y<=105;y+=20)for(unsigned x=120;x<=180;x+=30){
  double c=(y+.5-30)/150,b=(x+.5-40-120*c)/240,a=1-b-c;
  double q=a/4+b/40+c/20,u=(b/40)/q,w=(c/20)/q;
  double fx=fmax(0,fmin(1,u*2-.5)),fy=fmax(0,fmin(1,w*2-.5));
  unsigned r=(unsigned)(255*((1-fx)*(1-fy)+fx*fy)+.5);
  unsigned g=(unsigned)(255*fx+.5),blue=(unsigned)(255*fy+.5);
  close_color(pixels[y*320+x],0xff000000|r<<16|g<<8|blue,2);
 }
 /* Texture-disabled material preserves the original palette path. */
 m.texture=NULL;render();assert(pixels[80*320+160]==0xffff0000);
 m.texture=&t;uv[0].u=NAN;
 assert(lhx_scene_render_with_options(&frame,palette,pixels,320,200,&options)==-1);
 uv[0].u=0;
 /* Clipping must interpolate UV in camera space before dividing by depth. */
 v[0]=at(40,30,1);render();memcpy(saved,pixels,sizeof pixels);
 double ab=(2-v[0].z)/(v[1].z-v[0].z),ac=(2-v[0].z)/(v[2].z-v[0].z);
 LhxSceneVertex clipped[]={
  {v[0].x+(v[1].x-v[0].x)*ab,v[0].y+(v[1].y-v[0].y)*ab,2},v[1],v[2],
  {v[0].x+(v[2].x-v[0].x)*ac,v[0].y+(v[2].y-v[0].y)*ac,2}};
 LhxSceneUV cuv[]={{ab,0},{1,0},{0,1},{0,ac}};uint16_t quad[]={0,1,2,3};
 m.vertices=clipped;m.vertex_count=4;m.uvs=cuv;p.indices=quad;p.count=4;render();
 unsigned different=0;
 for(unsigned i=0;i<320*200;i++){
  if((pixels[i]==0)!=(saved[i]==0))different++;
  else close_color(pixels[i],saved[i],2);
 }
 assert(different<10);
}
static void lighting(void){
 setup();options.lighting=1;view.has_camera=1;
 uint16_t idx[]={0,1,2,3};
 LhxScenePrimitive p={.kind=0,.color=4,.count=4,.indices=idx};
 LhxSceneVertex original[]={{-4,4,20},{4,4,20},{4,-4,20},{-4,-4,20}},v[4];
 LhxSceneMesh m={.vertices=v,.vertex_count=4,.primitives=&p,.primitive_count=1,.layer=2};
 view.meshes=&m;view.mesh_count=1;
 for(unsigned turn=0;turn<2;turn++){
  double a=turn*.3,c=cos(a),s=sin(a);
  view.world_to_view[0][0]=c;view.world_to_view[0][2]=s;
  view.world_to_view[2][0]=-s;view.world_to_view[2][2]=c;
  for(unsigned i=0;i<4;i++)v[i]=(LhxSceneVertex){c*original[i].x+s*original[i].z,
     original[i].y,-s*original[i].x+c*original[i].z};
  render();unsigned x=(unsigned)(160+128*tan(a));
  /* World normal (0,0,-1) faces away from this fixed sun: ambient .55. */
  assert(pixels[100*320+x]==0xff8c8c8c);
  view.has_camera=0;render();assert(pixels[100*320+x]==0xffffffff);view.has_camera=1;
 }
}
static void runway_and_ground(void){
 setup();view.has_camera=1;view.camera[1]=10;
 LhxSceneMesh m={.layer=1,.runway=1,.runway_corners={
  {-10,-10,10},{10,-10,10},{10,-10,50},{-10,-10,50}}};
 view.meshes=&m;view.mesh_count=1;options.runway_textures=1;render();
 assert(pixels[140*320+160]!=0 && pixels[140*320+160]!=0xff000000);
 options.runway_textures=0;render();assert(pixels[140*320+160]==0xff000000);
 uint32_t red[]={0xffff0000,0xffff0000,0xffff0000,0xffff0000};
 uint32_t blue[]={0xff0000ff,0xff0000ff,0xff0000ff,0xff0000ff};
 LhxTerrain t={.near={.texture={.count=1,.levels={{2,2,red}}},.bounds={-10,10,10,30}},
               .far={.texture={.count=1,.levels={{2,2,blue}}},.bounds={-100,100,0,100}}};
 options.terrain=&t;view.mesh_count=0;render();
 assert(pixels[160*320+160]==0xffff0000); /* ray hits (0,0,20) in near grid */
 assert(pixels[120*320+160]==0xff0000ff); /* ray hits (0,0,60) in far grid */
 assert(pixels[40*320+160]==0xffffffff);  /* ray looks above true horizon */
 view.has_camera=0;render();assert(pixels[160*320+160]==0xff000000);
}
int main(int argc,char **argv){
 assert(argc>=4);sampling();perspective();lighting();runway_and_ground();
 LhxTerrainGrid grid={0};assert(lhx_texture_load(&grid,argv[2]));
 uint32_t first=grid.texture.levels[0].pixels[0];
 for(int i=3;i<argc;i++){
  assert(!lhx_texture_load(&grid,argv[i]));assert(grid.texture.levels[0].pixels[0]==first);
 }
 lhx_texture_free(&grid.texture);
 FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,3,SEEK_SET));
 lhx_terrain_configure(0,"missing");lhx_terrain_file_open(5,f);
 lhx_terrain_file_read(5,79,1);assert(!lhx_terrain_current());
 lhx_terrain_configure(1,".");lhx_terrain_file_open(5,f);assert(ftell(f)==3);
 lhx_terrain_file_read(5,78,1);assert(!lhx_terrain_current());
 const uint32_t expected[]={0xffff0000,0xff00ff00,0xff0000ff};
 for(unsigned i=0;i<3;i++){
  lhx_terrain_file_read(5,79+i*16,1);const LhxTerrain*t=lhx_terrain_current();
  assert(t&&t->near.texture.levels[0].pixels[0]==expected[i]);
 }
 assert(!remove("asia_far.lht"));lhx_terrain_file_read(5,95,1);
 assert(!lhx_terrain_current()); /* missing new theatre never reuses old imagery */
 lhx_terrain_file_close(5);lhx_terrain_file_read(5,79,1);assert(!lhx_terrain_current());
 fclose(f);lhx_terrain_reset();lhx_scene_renderer_reset();
 puts("Texture projection, clipping, world lighting, ground and theatre lifecycle passed");
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class VisualEnhancements(unittest.TestCase):
    def test_rendering_and_asset_lifecycle(self):
        with tempfile.TemporaryDirectory(prefix="lhx-visual-") as directory:
            work = Path(directory)
            (work / "check.c").write_text(HARNESS)
            library = b"EALIB" + struct.pack("<H", 3)
            for i, name in enumerate((b"GULF.W", b"ASIA.W", b"EUROPE.W", b"")):
                library += struct.pack("<13sBI", name, 0, 79 + i * 16)
            (work / "library.lib").write_bytes(library + b"x" * 48)
            for name, color in zip(("gulf", "asia", "europe"),
                                   (b"\xff\0\0", b"\0\xff\0", b"\0\0\xff")):
                for label in ("near", "far"):
                    (work / f"{name}_{label}.lht").write_bytes(
                        b"LHX texture 1\n2 2\n-100 100 -100 100\n" + color * 4)
            bad = []
            for i, payload in enumerate((b"bad", b"LHX texture 1\n999999 2\n",
                    b"LHX texture 1\n2 2\nnan 1 0 1\n",
                    b"LHX texture 1\n2 2\n1 0 0 1\n",
                    b"LHX texture 1\n2 2\n0 1 0 1\n" + b"x" * 11,
                    b"LHX texture 1\n2 2\n0 1 0 1\n" + b"x" * 13)):
                path = work / f"bad-{i}.lht"
                path.write_bytes(payload)
                bad.append(str(path))
            binary = work / "check"
            built = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(ROOT / "runtime"), str(work / "check.c"),
                *[str(ROOT / "runtime" / f"{name}.c") for name in
                  ("scene_renderer", "scene_texture", "scene_assets", "scene_props", "scene_relief")],
                "-lm", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            result = subprocess.run([str(binary), str(work / "library.lib"),
                str(work / "gulf_near.lht"), *bad], cwd=work,
                capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless(shutil.which("magick"), "Optional ImageMagick importer required")
    def test_import_images_without_elevation_and_preserve_source(self):
        spec = importlib.util.spec_from_file_location("import_terrain", ROOT / "tools/import_terrain.py")
        importer = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(importer)
        with tempfile.TemporaryDirectory(prefix="lhx-terrain-import-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "output"
            source.mkdir()
            for name in ("gulf", "asia", "europe"):
                meta = {"attribution": ["fixture"], "near": {"pixels": 8, "bounds": [-1, 1, -2, 2]},
                        "far": {"pixels": 8, "bounds": [-10, 10, -20, 20]}}
                (source / f"{name}.json").write_text(json.dumps(meta))
                for label in ("near", "far"):
                    subprocess.run(["magick", "-size", "8x8", "xc:red",
                                    str(source / f"{name}_{label}.jpg")], check=True,
                                   capture_output=True)
            before = {p.name: p.read_bytes() for p in source.iterdir()}
            result = importer.convert(source, output, near_size=4, far_size=2)
            self.assertFalse(result["relief"])
            self.assertEqual(len(list(output.glob("*.lht"))), 6)
            self.assertTrue((output / "gulf_near.lht").read_bytes().startswith(
                b"LHX texture 1\n4 4\n-1 1 -2 2\n"))
            self.assertEqual(before, {p.name: p.read_bytes() for p in source.iterdir()})
            with self.assertRaises(ValueError):
                importer.convert(source, source)
            meta["near"]["bounds"] = [1, 0, 0, 1]
            (source / "gulf.json").write_text(json.dumps(meta))
            with self.assertRaises(ValueError):
                importer.convert(source, output / "invalid")
            self.assertFalse((output / "invalid").exists())


if __name__ == "__main__":
    unittest.main()
