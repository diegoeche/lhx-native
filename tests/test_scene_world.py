"""Independent world inventory, float transforms, and geometry ownership checks."""
from pathlib import Path
import hashlib
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "scene_world.h"
#include "model_transform.h"
const uint16_t g_load_seg=0x110;
int g_recomp_tick_budget=1000000;
void recomp_tick(CPU*c){(void)c;g_recomp_tick_budget=1000000;}
void recomp_unhandled(CPU*c,uint16_t s,uint16_t o,const char*r){
 (void)c;(void)s;(void)o;fprintf(stderr,"unexpected %s\n",r);abort();}
void fn_193C5(CPU*c){c->sp+=4;}
static unsigned live,emissions,fail_after=~0u,allocations;
static int decline;
static LhxSceneMesh last;
static LhxSceneVertex points[255];
static void *take(void*ctx,size_t n,size_t z){
 (void)ctx;if(allocations++>=fail_after)return NULL;
 void*p=calloc(n,z);if(p)live++;return p;}
static void drop(void*ctx,void*p){(void)ctx;if(p){assert(live);live--;free(p);}}
static int emit(void*ctx,uint32_t id,LhxSceneMesh*m){
 assert(id==m->object_id);if(decline)return 0;
 last=*m;memcpy(points,m->vertices,m->vertex_count*sizeof(*points));emissions++;
 for(unsigned i=0;i<m->primitive_count;i++){
  LhxScenePrimitive*p=m->primitives+i;assert(p->kind<=3&&p->color==7);
  for(unsigned j=0;j<p->count;j++)assert(p->indices[j]<m->vertex_count);
  drop(ctx,p->indices);
 }
 drop(ctx,m->primitives);drop(ctx,m->vertices);return 1;}
static void dw(CPU*c,unsigned o,unsigned v){mem_write16(c,c->ds,o,v);}
static void object(CPU*c,unsigned o,unsigned rec,unsigned flags,unsigned next,
                   double x,double y,double z){
 mem_write16(c,0x3000,o,rec);mem_write16(c,0x3000,o+2,flags);
 mem_write16(c,0x3000,o+4,next);
 mem_write32(c,0x3000,o+6,(uint32_t)(int32_t)(x*256));
 mem_write32(c,0x3000,o+10,(uint32_t)(int32_t)(y*256));
 mem_write32(c,0x3000,o+14,(uint32_t)(int32_t)(z*256));}
static void lod(CPU*c,unsigned o,unsigned n,unsigned mode,unsigned seg,unsigned ptr){
 mem_write8(c,c->ds,o,1);mem_write8(c,c->ds,o+1,n);
 dw(c,o+6,ptr);dw(c,o+8,seg);mem_write8(c,c->ds,o+13,mode);
 mem_write8(c,c->ds,o+14,0x10);mem_write8(c,c->ds,o+17,7);
 mem_write8(c,c->ds,o+18,255);mem_write8(c,c->ds,o+20,n);
 for(unsigned j=0;j<n;j++)mem_write8(c,c->ds,o+21+j,j);}
static void record(CPU*c,unsigned r,unsigned low,unsigned high){
 dw(c,r+2,20);dw(c,r+14,1);dw(c,r+16,0);
 dw(c,r+18,low);dw(c,r+20,high);}
static LhxSceneView view={.x0=0,.y0=0,.x1=320,.y1=200,.cx=160,.cy=100,.focus=128};
static void setup(CPU*c){
 memset(c->mem,0,MEM_SIZE);c->ds=0x2000;c->ss=0x2000;c->sp=0xf000;
 dw(c,0xf019,0x100);dw(c,0x104,2);dw(c,0x106,1);
 dw(c,0x108,0);dw(c,0x10a,0x3000);dw(c,0x10c,0x1180);dw(c,0x10e,0x4000);
 mem_write16(c,0x4000,0,0x1000);mem_write16(c,0x4000,4,0x1000);
 dw(c,0xf03b,16384);dw(c,0xf043,14224);dw(c,0xf04b,16384);
 record(c,0x300,0x400,0x500);lod(c,0x400,3,0,0x4100,0);lod(c,0x500,4,0,0x4100,0);
 const int16_t p[12]={10,0,0, 0,20,0, -10,0,0, 0,-20,0};
 for(unsigned i=0;i<12;i++)mem_write16(c,0x4100,2*i,(uint16_t)p[i]);
 object(c,0x1000,0x300,3,0x1040,0,0,500000);
 object(c,0x1040,0x300,0,0x1080,0,0,500000);
 object(c,0x1080,0,0x21,0x10c0,0,0,0);mem_write16(c,0x3000,0x1098,0x1100);
 object(c,0x10c0,0,0x20,0,0,0,0);mem_write16(c,0x3000,0x10d8,0x1140);
 object(c,0x1100,0x300,3,0,5,0,500000);
 object(c,0x1140,0x300,3,0,0,0,500000); /* Inactive parent hides this child. */
 object(c,0x1180,0x300,3,0x11c0,0,0,-1000);
 record(c,0x600,0x680,0);lod(c,0x680,3,0,0x4100,0);dw(c,0x684,0x1111);
 object(c,0x11c0,0x600,3,0x1200,0,0,500000);
 /* A retained far block with relocated metadata, signed-byte source points. */
 record(c,0x700,0x9000,0);dw(c,0x720,0x100);dw(c,0x722,0x4200);
 dw(c,0x724,0x9000);dw(c,0x726,64);
 lod(c,0x9000,3,4,0x4300,0);
 memcpy(c->mem+(0x4200<<4)+0x100,c->mem+(c->ds<<4)+0x9000,64);
 const int8_t q[9]={-3,1,2, 3,1,2, 0,-1,-2};
 memcpy(c->mem+(0x4300<<4),q,sizeof(q));
 object(c,0x1200,0x700,3,0,0,0,500000);
 allocations=emissions=0;fail_after=~0u;decline=0;assert(!live);
}
static void capture(CPU*c,LhxSceneWorldStats*s){
 CPU before=*c;unsigned char*copy=malloc(MEM_SIZE);assert(copy);memcpy(copy,c->mem,MEM_SIZE);
 assert(lhx_scene_world_capture(c,&view,take,drop,emit,NULL,s));
 assert(!memcmp(&before,c,sizeof(*c))&&!memcmp(copy,c->mem,MEM_SIZE));free(copy);assert(!live);}
int main(void){
 CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);LhxSceneWorldStats s;
 /* Optional display metadata uses existing camera/model layouts read-only. */
 setup(&c);
 mem_write8(&c,c.ds,0x100,6);mem_write8(&c,c.ds,0x101,9);
 mem_write32(&c,c.ds,0xf01f,10*256);
 LhxSceneView environment=view;
 lhx_scene_world_environment(&c,&environment);
 assert(environment.has_camera && environment.camera[1]==10);
 assert(environment.ground_color==6 && environment.sky_color==9);
 assert(environment.world_to_view[1][1]==14224.0/16384);
 LhxSceneMesh surface={.layer=1};
 dw(&c,0x31e,0x800);memcpy(c.mem+(c.ds<<4)+0x800,"runway",7);
 mem_write32(&c,c.ds,0x32c,(uint32_t)-2560);mem_write32(&c,c.ds,0x330,2560);
 mem_write32(&c,c.ds,0x33c,0);mem_write32(&c,c.ds,0x340,100*256);
 unsigned char *snapshot=malloc(MEM_SIZE);assert(snapshot);memcpy(snapshot,c.mem,MEM_SIZE);
 lhx_scene_world_surface(&c,0x300,0x3000,0x1000,&surface);
 assert(surface.runway && surface.runway_corners[0].x==-10 &&
        surface.runway_corners[2].x==10 && surface.runway_corners[2].z==500100);
 assert(!memcmp(snapshot,c.mem,MEM_SIZE));free(snapshot);
 /* Relocated names and a rotated runway retain the same world alignment. */
 dw(&c,0x31e,0x9040);dw(&c,0x320,0x100);dw(&c,0x322,0x4200);
 dw(&c,0x324,0x9000);dw(&c,0x326,128);
 memcpy(c.mem+(0x4200<<4)+0x140,"runway",7);
 mem_write16(&c,0x3000,0x1002,1);mem_write16(&c,0x3000,0x1012,720);
 lhx_scene_world_surface(&c,0x300,0x3000,0x1000,&surface);
 assert(surface.runway && fabs(surface.runway_corners[2].x+100)<.001 &&
        fabs(surface.runway_corners[2].z-500010)<.001);
 environment=(LhxSceneView){.x1=100};lhx_scene_world_environment(&c,&environment);
 assert(!environment.has_camera);
 setup(&c);capture(&c,&s);
 assert(s.considered==8&&s.inactive==2&&s.groups==1&&s.unsupported==1);
 assert(s.frustum_rejected==1&&s.emitted==3&&emissions==3);
 assert(s.vertices==11&&s.primitives==3&&last.object_id==seg_off(0x3000,0x1200));
 assert(last.object_distance==500000&&fabs(points[0].x+3)<.001);
 assert(fabs(points[0].z-500002)<.001&&fabs(points[0].y-14224.0/16384)<.001);
 /* No finite range/16-bit translation limit; rich LOD selected despite original far=1. */
 setup(&c);dw(&c,0x10c,0);mem_write16(&c,0x3000,0x1004,0);
 mem_write16(&c,0x4000,4,0);capture(&c,&s);
 assert(s.emitted==1&&last.vertex_count==4&&last.object_distance==500000);
 assert(fabs(points[0].x-10)<.001&&points[0].z==500000);
 /* Distinct cardinal rotations, authored Ry(-h)*Rx(-p)*Rz(-r). */
 for(unsigned axis=0;axis<3;axis++){
  mem_write16(&c,0x3000,0x1002,1);
  for(unsigned j=0;j<3;j++)mem_write16(&c,0x3000,0x1012+j*2,j==axis?720:0);
  capture(&c,&s);
  if(axis==0){assert(fabs(points[0].x)<.001&&fabs(points[0].z-500010)<.001);}
  if(axis==1){assert(fabs(points[1].y)<.001&&fabs(points[1].z-499980)<.001);}
  if(axis==2){assert(fabs(points[0].x)<.001&&fabs(points[0].y+10*14224.0/16384)<.001);}
 }
 /* Unsupported rich model falls back to the retained low LOD. */
 mem_write8(&c,c.ds,0x50d,2);dw(&c,0x508,0x7777);capture(&c,&s);
 assert(last.vertex_count==3&&s.emitted==1);
 /* BRDM's authored BSP leaf is enabled per object, without touching shared data. */
 setup(&c);dw(&c,0x10c,0);mem_write16(&c,0x3000,0x1004,0);
 mem_write16(&c,0x4000,4,0);record(&c,0x300,0xba0e,0);
 lod(&c,0xba0e,3,0,0x4100,0);dw(&c,0xba10,0x466);dw(&c,0xba12,0x2eab);
 dw(&c,0xba18,0xbac2);mem_write8(&c,c.ds,0xbac2,3);
 mem_write8(&c,c.ds,0xbac3,1);dw(&c,0xbac4,0xba1c);
 mem_write16(&c,0x3000,0x1018,0x1bbe);capture(&c,&s);
 assert(s.emitted==1&&last.primitive_count==1);
 mem_write16(&c,0x3000,0x1018,0);capture(&c,&s);
 assert(s.emitted==1&&last.primitive_count==0&&mem_read8(&c,c.ds,0xbac2)==3);
 /* An enabled leaf referencing unknown primitive data fails cleanly. */
 dw(&c,0xbac4,0x1234);mem_write16(&c,0x3000,0x1018,0x1bbe);
 assert(!lhx_scene_world_capture(&c,&view,take,drop,emit,NULL,&s));assert(!live);
 setup(&c);dw(&c,0x10c,0);mem_write16(&c,0x3000,0x1004,0);
 mem_write16(&c,0x4000,4,0);
 /* The authored current-view flag rejects whole groups, including their children. */
 setup(&c);dw(&c,0x06ef,1);mem_write16(&c,0x3000,0x1082,0x25);
 capture(&c,&s);assert(s.emitted==2&&s.considered==7&&s.groups==0);
 setup(&c);dw(&c,0x10c,0);mem_write16(&c,0x3000,0x1004,0);
 mem_write16(&c,0x4000,4,0);
 dw(&c,0x06ef,1);mem_write16(&c,0x3000,0x1002,7);capture(&c,&s);
 assert(s.emitted==0&&s.unsupported==1);
 dw(&c,0x06ef,0);mem_write16(&c,0x3000,0x1002,3);
 /* Allocation and rejected-emit ownership failures leak no buffers. */
 for(unsigned failure=0;failure<3;failure++){
  allocations=0;fail_after=failure;
  assert(!lhx_scene_world_capture(&c,&view,take,drop,emit,NULL,&s));assert(!live);
 }
 fail_after=~0u;decline=1;
 assert(!lhx_scene_world_capture(&c,&view,take,drop,emit,NULL,&s));assert(!live);
 /* Malformed grid and source bounds fail without reading outside segments. */
 decline=0;dw(&c,0x104,0xffff);
 assert(!lhx_scene_world_capture(&c,&view,take,drop,emit,NULL,&s));assert(!live);
 setup(&c);dw(&c,0x506,0xffff);
 assert(!lhx_scene_world_capture(&c,&view,take,drop,emit,NULL,&s));assert(!live);
 lhx_model_reset();free(c.mem);puts("native world inventory/float geometry passed");
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneWorld(unittest.TestCase):
    def test_world_inventory_and_float_geometry(self):
        with tempfile.TemporaryDirectory(prefix="lhx-scene-world-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("void fn_193C5(CPU*);\n")
            (tmp / "check.c").write_text(HARNESS)
            result = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(tmp), "-I", str(ROOT / "runtime"), str(tmp / "check.c"),
                str(ROOT / "runtime/scene_world.c"),
                str(ROOT / "runtime/model_transform.c"), "-lm", "-o", str(tmp / "check")
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(tmp / "check")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    @unittest.skipUnless((ROOT / "bin/dos/LHX.unpacked.EXE").exists() and
                         (ROOT / "build/cmake/generated/recomp_006.c").exists(),
                         "original image/generated native functions absent")
    def test_rotation_against_original_native_matrix(self):
        # Compare the native float placement with the original matrix builder,
        # compiled to C by the production lifter. No CPU instructions run or
        # get interpreted in this test; the original sine table is plain data.
        names = ["14872", "17C9E", "17CB4", "17BF6", "17C43", "1B1DF",
                 "2DE16", "2DE46", "2DE98", "2E27A"]
        generated = "\n".join(p.read_text() for p in
                              (ROOT / "build/cmake/generated").glob("recomp_*.c"))
        functions = []
        for name in names:
            start = generated.index("void fn_" + name + "_body(CPU *cpu)")
            end = generated.index("\nvoid fn_", start + 20)
            end = generated.index("\n", end + 1)
            functions.append(generated[start:end])
        image = (ROOT / "bin/dos/LHX.unpacked.EXE").read_bytes()
        header_bytes = struct.unpack_from("<H", image, 8)[0] * 16
        module = image[header_bytes:]
        self.assertEqual(hashlib.sha256(module).hexdigest(),
                         "4d30628da15b38eb93cad06420d255574db7283cfec4292f9bfbc8b9d4369f9e")
        source = r'''
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "scene_world.c"
#define SEG_0FD1 0x10e1
#define SEG_30DE 0x31ee
#define SEG_2D9B 0x2eab
const uint16_t g_load_seg=0x110;
int g_recomp_tick_budget=1000000;
void recomp_tick(CPU*c){(void)c;}
void recomp_unhandled(CPU*c,uint16_t s,uint16_t o,const char*r){
 (void)c;(void)s;(void)o;fprintf(stderr,"%s\n",r);abort();}
void recomp_dispatch(CPU*c,uint16_t s,uint16_t o){(void)c;(void)s;(void)o;abort();}
void fn_193C5(CPU*c){c->sp+=4;}
''' + "\n".join(functions) + r'''
int main(int argc,char**argv){
 assert(argc==2);CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);
 c.ds=c.ss=0x38ab;c.sp=0xe000;
 FILE*f=fopen(argv[1],"rb");assert(f);
 assert(fread(c.mem+0x1100,1,292816,f)==292816);fclose(f);
 World w={.cpu=&c};Address object={0x8000,0};double maximum=0;
 for(unsigned h=0;h<2880;h+=560)for(unsigned p=0;p<2880;p+=400)
  for(unsigned r=0;r<2880;r+=350){
   mem_write16(&c,object.segment,0x12,h);mem_write16(&c,object.segment,0x14,p);
   mem_write16(&c,object.segment,0x16,r);
   double native[3][3];placement(&w,object,1,native);uint16_t sp=c.sp;
   push16(&c,0x4000);push16(&c,h);push16(&c,p);push16(&c,r);
   push16(&c,0x10e1);push16(&c,0xffff);fn_14872(&c);assert(sp==c.sp);
   for(unsigned i=0;i<9;i++){
    double original=(int16_t)mem_read16(&c,c.ds,0x4000+2*i)/16384.0;
    double error=fabs(original-native[i/3][i%3]);if(error>maximum)maximum=error;
   }
  }
 /* Original Q14 multiply/trig rounding is intentionally removed, preserving
  * the authored rotation convention without its coordinate range. */
 assert(maximum<.0004);
 /* Compare read-only vehicle/damage selection with every original callback.
  * The original writes its shared BSP data; the new route only returns modes. */
 mem_write16(&c,c.ds,0xf201,0);mem_write16(&c,c.ds,0xf203,object.segment);
 const unsigned states[]={0,0x1bbe,0x1c5a,0x1a74,0x1abc};
 const unsigned nodes[]={0xbac2,0xbc4c,0xc078,0xc082,0xc211,0xc21b,
                         0x6c47,0x6bd1,0x6bbf,0x6c1d,0x6bfd};
 for(unsigned state=0;state<5;state++)for(unsigned altitude=0;altitude<2;altitude++)
  for(unsigned deployed=0;deployed<2;deployed++){
   mem_write16(&c,object.segment,0x18,states[state]);
   mem_write32(&c,object.segment,0x0a,altitude?30000:10000);
   mem_write8(&c,object.segment,0x24,deployed?7:0);
   mem_write16(&c,c.ds,0xf209,0x6771);
   for(unsigned i=0;i<sizeof(nodes)/sizeof(*nodes);i++)mem_write8(&c,c.ds,nodes[i],0x5b);
   push16(&c,0x2eab);push16(&c,0xffff);fn_2DE16(&c);
   push16(&c,0x2eab);push16(&c,0xffff);fn_2DE46(&c);
   push16(&c,0x2eab);push16(&c,0xffff);fn_2DE98(&c);
   for(unsigned i=0;i<sizeof(nodes)/sizeof(*nodes);i++){
    unsigned cb=i<2?0x466:i<6?0x496:0x4e8;
    assert(leaf_mode(&w,object,cb,0x6771,nodes[i],0x5b)==mem_read8(&c,c.ds,nodes[i]));
   }
   assert(mem_read16(&c,c.ds,0xc1b9)==(states[state]==0x1c5a?0xff0c:0xff09));
  }
 free(c.mem);
}
'''
        with tempfile.TemporaryDirectory(prefix="lhx-scene-rotation-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("\n".join(
                "void fn_" + n + "(CPU*);" for n in names) +
                "\nvoid fn_193C5(CPU*);\n")
            (tmp / "module").write_bytes(module)
            (tmp / "check.c").write_text(source)
            result = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(tmp / "check.c"), str(ROOT / "runtime/model_transform.c"),
                "-lm", "-o", str(tmp / "check")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            result = subprocess.run([str(tmp / "check"), str(tmp / "module")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
