"""Check scene capture, pixel filtering and viewport lifetime without game data."""

from pathlib import Path
import json
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneCapture(unittest.TestCase):
    def test_capture_filter_and_view_lifetime(self):
        with tempfile.TemporaryDirectory(prefix="lhx-scene-capture-") as directory:
            tmp = Path(directory)
            # The native capture API needs no generated function definitions.
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''

#include "scene_filter.h"
#include "scene_world.h"
#include "scene_metrics.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int g_lhx_scene_enabled=1;
int g_lhx_scene_world_enabled;
void lhx_scene_world_environment(CPU*c,LhxSceneView*v) {(void)c;(void)v;}
void lhx_scene_world_surface(CPU*c,unsigned r,uint16_t s,unsigned o,LhxSceneMesh*m)
 {(void)c;(void)r;(void)s;(void)o;(void)m;}
int lhx_scene_world_capture(CPU*c,const LhxSceneView*v,LhxSceneWorldAlloc a,
 LhxSceneWorldFree f,LhxSceneWorldEmit e,void*x,LhxSceneWorldStats*s) {
 (void)c;(void)v;(void)a;(void)f;(void)e;(void)x;(void)s;
 abort(); /* Original capture mode must never invoke supplemental discovery. */
}
void recomp_unhandled(CPU *c,uint16_t s,uint16_t o,const char *why) {
 (void)c;(void)s;(void)o;fprintf(stderr,"capture error: %s\n",why);abort();
}
static void viewport(CPU*c,int x,int y,int w,int h) {
 mem_write16(c,c->ds,0xeb5a,x);mem_write16(c,c->ds,0xeb58,w/2);
 mem_write16(c,c->ds,0xeed1,y);mem_write16(c,c->ds,0xeed3,y+h-1);
 mem_write16(c,c->ds,0xeb52,x+w/2);mem_write16(c,c->ds,0xeb54,y+h/2);
 mem_write16(c,c->ds,0xf02f,7);
}
int main(void) {
 CPU cpu;cpu_init(&cpu);cpu.mem=calloc(1,MEM_SIZE);assert(cpu.mem);cpu.ds=0x1000;
 cpu.ax=0x1234;cpu.bx=0x5678;cpu.flags=0x246;CPU regs=cpu;
 for(int y=0;y<200;y++)mem_write16(&cpu,cpu.ds,0xeb6e + y*2,y*320);
 mem_write16(&cpu,cpu.ds,0xecfe,0x5000);
 memset(cpu.mem+0x50000,7,64000);viewport(&cpu,10,10,100,20);
 lhx_scene_event(&cpu,0x1468a);assert(memcmp(&cpu,&regs,sizeof(cpu))==0);
 assert(cpu.mem[0x50000+10*320+10]==255 && cpu.mem[0x50000+10*320+9]==7);
 assert(lhx_scene_drop_byte(0x50000+10*320));
 /* Word/dword stores crossing a filter edge preserve every unfiltered byte. */
 mem_write32(&cpu,0x5000,30*320-2,0x12345678);
 assert(cpu.mem[0x50000+30*320-2]==7 && cpu.mem[0x50000+30*320-1]==7);
 assert(cpu.mem[0x50000+30*320]==0x34 && cpu.mem[0x50000+30*320+1]==0x12);
 mem_write8(&cpu,0x5000,10*320,99);assert(cpu.mem[0x50000+10*320]==7);
 mem_write16(&cpu,0x5000,10*320-1,0x3456);
 assert(cpu.mem[0x50000+10*320-1]==0x56 && cpu.mem[0x50000+10*320]==7);
 lhx_scene_event(&cpu,0x146be);
 lhx_scene_event(&cpu,0x1aa0d);lhx_scene_event(&cpu,0x1aa10);
 mem_write8(&cpu,0x5000,10*320+11,88);
 assert(cpu.mem[0x50000+10*320+11]==88);lhx_scene_event(&cpu,0x146c3);
 mem_write8(&cpu,0x5000,10*320+11,99);assert(cpu.mem[0x50000+10*320+11]==88);
 mem_write16(&cpu,cpu.ds,0xf209,0x200);mem_write16(&cpu,cpu.ds,0xf207,0x300);
 mem_write16(&cpu,cpu.ds,0x70e,0x400);mem_write8(&cpu,cpu.ds,0xf1ff,1);
 mem_write8(&cpu,cpu.ds,0x30c,2);mem_write32(&cpu,cpu.ds,0x334,0);
 mem_write32(&cpu,cpu.ds,0x338,256);mem_write16(&cpu,cpu.ds,0xf201,0x100);
 mem_write16(&cpu,cpu.ds,0xf203,0x3000);mem_write32(&cpu,0x3000,0x10a,0);
 mem_write8(&cpu,cpu.ds,0x200,5);mem_write8(&cpu,cpu.ds,0x201,3);
 for(int i=0;i<3;i++) {mem_write16(&cpu,cpu.ds,0x402+16*i,10+i);mem_write16(&cpu,cpu.ds,0x406+16*i,20+i);mem_write16(&cpu,cpu.ds,0x40a+16*i,100+i);}
 unsigned char prim[]={0x10,0,0,14,255,0,3,0,1,2, 1,0,0,3,0,0,1, 2,0,0,4,0,2, 3,0,0,5,0,3,0,1, 4,0,0,0,0,0,0,0};
 memcpy(cpu.mem+0x10000+0x20e,prim,sizeof prim);
 lhx_scene_event(&cpu,0x1cd76);lhx_scene_event(&cpu,0x14792);
 /* No capture after scene end even if context and old pending view match. */
 lhx_scene_event(&cpu,0x1cd76);lhx_scene_event(&cpu,0x11096);
 const LhxSceneFrame*f=lhx_scene_frame();assert(f->view_count==1 && f->serial==1);
 const LhxSceneMesh*m=f->views[0].meshes;assert(f->views[0].mesh_count==1);
 assert(m->vertex_count==3 && m->primitive_count==4 && m->layer==2);
 assert(m->vertices[0].x==20 && m->vertices[0].y==40 && m->vertices[0].z==200);
 assert(m->primitives[0].count==3 && m->primitives[0].one_sided && !m->primitives[0].stipple);
 assert(m->primitives[3].radius==12 && m->primitives[3].indices[0]==1);
 mem_write16(&cpu,cpu.ds,0x402,777);mem_write8(&cpu,cpu.ds,0x215,2);
 assert(m->vertices[0].x==20 && m->primitives[0].indices[0]==0);
 /* New full-width primary retires old partial views; auxiliary captured after
    that primary starts survives publication even when presented first. */
 viewport(&cpu,0,0,320,200);lhx_scene_event(&cpu,0x1468a);lhx_scene_event(&cpu,0x14792);
 viewport(&cpu,200,120,100,60);lhx_scene_event(&cpu,0x1468a);lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 viewport(&cpu,0,0,320,200);lhx_scene_event(&cpu,0x11096);assert(f->view_count==2);
 viewport(&cpu,0,0,320,94);lhx_scene_event(&cpu,0x1468a);lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);assert(f->view_count==1 && f->views[0].y1==94);
 /* Cockpit instruments update less often than the main3D view. Repeated
    presentation of the same primary must preserve the cached attitude view. */
 viewport(&cpu,14,148,52,42);lhx_scene_event(&cpu,0x1468a);
 lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 assert(f->view_count==2);
 for(int i=0;i<20;++i) {
   viewport(&cpu,0,0,320,94);lhx_scene_event(&cpu,0x1468a);
   lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
   assert(f->view_count==2);
 }
 /* A chase transition retires the old family; returning to cockpit and
    publishing its attitude once again keeps that view between updates. */
 viewport(&cpu,0,0,320,200);lhx_scene_event(&cpu,0x1468a);
 lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 assert(f->view_count==1 && f->views[0].y1==200);
 viewport(&cpu,0,0,320,94);lhx_scene_event(&cpu,0x1468a);
 lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 viewport(&cpu,14,148,52,42);lhx_scene_event(&cpu,0x1468a);
 lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 for(int i=0;i<20;++i) {
   viewport(&cpu,0,0,320,94);lhx_scene_event(&cpu,0x1468a);
   lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
   assert(f->view_count==2);
 }
 /* The original NW Libya briefing map keeps rectangle32,28..224,177 while
    scrolling its projection centre. More than16 positions must reuse one
    displayed viewport instead of exhausting the persistent-window bound. */
 lhx_scene_reset();
 for (int i=0;i<40;++i) {
   viewport(&cpu,32,28,192,149);
   mem_write16(&cpu,cpu.ds,0xeb52,143+i);
   mem_write16(&cpu,cpu.ds,0xeb54,102+i/3);
   lhx_scene_event(&cpu,0x1468a);lhx_scene_event(&cpu,0x14792);
   lhx_scene_event(&cpu,0x11096);
   assert(f->view_count==1 && f->views[0].cx==143+i && f->views[0].cy==102+i/3);
 }
 /* Sample arena pressure before cache trimming. Its neighbor belongs to a
    view bound and must not turn the byte-sized flag into a false positive. */
 assert(lhx_scene_metrics_init(8));
 for(unsigned arena=0;arena<=1;arena++) {
   viewport(&cpu,0,0,320,94);
   mem_write8(&cpu,cpu.ds,0xf0a1,arena);
   mem_write8(&cpu,cpu.ds,0xf0a2,255);
   lhx_scene_event(&cpu,0x1468a);
   mem_write8(&cpu,cpu.ds,0xf0a1,2); /* Later persistent-cache trim. */
   lhx_scene_event(&cpu,0x14792);lhx_scene_event(&cpu,0x11096);
 }
 puts("METRICS_JSON:");assert(lhx_scene_metrics_report_json(stdout)==1);
 lhx_scene_metrics_reset();
 g_lhx_scene_enabled=0;mem_write8(&cpu,0x5000,100,66);lhx_scene_event(&cpu,0x1468a);assert(cpu.mem[0x50000+100]==66 && f->view_count==1);
 assert(memcmp(&cpu,&regs,sizeof(cpu))==0);
 lhx_scene_reset();assert(!f->view_count && !f->serial);free(cpu.mem);puts("scene capture/filter tests passed");
}
''')
            binary = tmp / "check"
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(tmp), "-I", str(ROOT / "runtime"), str(source),
                str(ROOT / "runtime/scene_capture.c"),
                str(ROOT / "runtime/scene_composite.c"),
                str(ROOT / "runtime/scene_metrics.c"),
                "-lm", "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("scene capture/filter tests passed", result.stdout)
            metrics, _ = json.JSONDecoder().raw_decode(
                result.stdout.split("METRICS_JSON:", 1)[1].lstrip())
            self.assertEqual(metrics["main_view_visibility_samples"], 2)
            self.assertEqual(metrics["full_arena_count"], 1)


if __name__ == "__main__":
    unittest.main()
