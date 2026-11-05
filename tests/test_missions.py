"""Check all embedded scenario callbacks as compiled native functions."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lift_missions import KNOWN, generate, mission_payloads


@unittest.skipUnless(shutil.which("cc") and (ROOT / "bin/dos/LHX3.LIB").exists(),
                     "C compiler and supplied scenario files required")
class MissionCallbacks(unittest.TestCase):
    def test_original_loader_lengths(self):
        payloads, count = mission_payloads(ROOT / "bin/dos")
        self.assertEqual(count, 22)
        self.assertEqual(len(payloads["FREE.S"]), 639)
        self.assertEqual(len(payloads["DEMO.S"]), 187)
        for name, payload in payloads.items():
            self.assertEqual(payload[-1], 0xCB)  # RETF; the following FF stays in the scenario stream.
            self.assertEqual(len(payload), KNOWN[name]["size"])

    def test_all_88_native_callbacks(self):
        with tempfile.TemporaryDirectory(prefix="lhx-missions-") as directory:
            tmp = Path(directory)
            report = generate(ROOT / "bin/dos", tmp, ROOT / "third_party/pcrecomp")
            self.assertEqual(report["scenario_files_examined"], 22)
            self.assertEqual(len(report["native_modules"]), 22)
            self.assertEqual(sum(len(m["exports"]) for m in report["native_modules"]), 88)
            self.assertEqual(sum(len(m["switch_tables"]) for m in report["native_modules"]), 8)
            self.assertEqual(report["unhandled_instructions"], [])
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mission_module.h"
static unsigned misses;
void recomp_unhandled(CPU*c,uint16_t s,uint16_t o,const char*why){
    (void)c;(void)s;(void)o;(void)why;misses++;
}
void recomp_dispatch(CPU*c,uint16_t s,uint16_t o){
    (void)c;(void)s;(void)o;assert(!"unexpected mission far-return trampoline");
}
static void invoke(CPU*c,const LhxMissionModule*m,uint16_t seg,unsigned callback,unsigned selector){
    uint16_t sp=c->sp;
    push16(c,0);push16(c,0);push16(c,selector);push16(c,0);
    push16(c,0x9000);push16(c,0x0200);push16(c,c->cs);push16(c,0xffff);
    assert(lhx_mission_dispatch(c,seg,m->functions[callback].address)==1);
    assert(c->sp==(uint16_t)(sp-12));c->sp+=12;
    assert(c->ds==0x2000 && c->cs==0x4321 && c->ip==0xabcd);
}
int main(void){
    CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);
    c.ss=0x3000;c.sp=0x8000;c.ds=0x2000;c.cs=0x4321;c.ip=0xabcd;
    for(unsigned i=0;i<g_mission_module_count;i++){
        const LhxMissionModule*m=&g_mission_modules[i];
        uint16_t seg=0x5000+i*0x200;
        memcpy(c.mem+(unsigned)seg*16,m->identity,m->size);
        for(unsigned callback=0;callback<4;callback++){
            memset(c.mem+0x90200,0xcc,8192);
            c.ax=0;c.bx=0x1111;c.dx=0x2222;c.cx=0x3333;
            c.si=0x4444;c.di=0x5555;c.bp=0x6666;c.es=0x7777;c.flags=0x0246;
            invoke(&c,m,seg,callback,0);
            assert(c.ds==0x2000 && c.cs==0x4321 && c.ip==0xabcd);
            assert(c.si==0x4444 && c.di==0x5555 && c.bp==0x6666);
            unsigned first=m->functions[0].address;
            unsigned saved_ds=m->identity[first+8]|(unsigned)m->identity[first+9]<<8;
            assert(mem_read16(&c,seg,saved_ds)==0x2000);
            int simple=!strcmp(m->name,"FREE.S")||!strcmp(m->name,"DEMO.S");
            if(simple)assert(c.bx==0x1111 && c.dx==0x2222 && c.flags==0x0246);
            if(simple && (callback==0 || callback==3)){
                unsigned start=callback==0?10:m->functions[2].address+23;
                unsigned size=m->functions[callback].address-start;
                assert(c.cx==0 && c.es==0x9000);
                assert(!memcmp(c.mem+0x90200,m->identity+start,size));
                assert(c.mem[0x90200+size]==0xcc);
                assert(c.ax==(callback==3?0:seg));
            }else if(simple){
                assert(c.cx==0x3333 && c.es==0x7777 && c.ax==seg);
                assert(c.mem[0x90200]==0xcc);
            }
        }
        if(!strcmp(m->name,"STRIKEA.S")){
            /* All five Alpha Strike event/debrief switch arms, including the
             * authored success counters and 1000-point rewards. */
            static const unsigned required[]={2,1,5,4,1};
            for(unsigned difficulty=0;difficulty<5;difficulty++){
                memcpy(c.mem+(unsigned)seg*16,m->identity,m->size);
                invoke(&c,m,seg,0,difficulty);
                assert(mem_read16(&c,seg,0x0a)==difficulty);
                invoke(&c,m,seg,3,difficulty);assert(c.ax==0);
                for(unsigned n=0;n<required[difficulty];n++){
                    invoke(&c,m,seg,1,difficulty);
                    assert(mem_read8(&c,seg,0x0c)==n+1);
                    invoke(&c,m,seg,3,difficulty);
                    assert(c.ax==(n+1==required[difficulty]?1000:0));
                }
            }
        }
        assert(lhx_mission_dispatch(&c,seg,9)==1 && misses==i+1);
        /* Mutable saved DS does not invalidate identity; changed code does. */
        c.mem[(unsigned)seg*16+8]=0xab;c.mem[(unsigned)seg*16+9]=0xcd;
        c.mem[(unsigned)seg*16+m->functions[0].address]^=1;
        assert(lhx_mission_dispatch(&c,seg,m->functions[0].address)==0);
    }
    assert(lhx_mission_dispatch(&c,0x5000,0)==0);
    free(c.mem);puts("all native mission callbacks passed");
}
''')
            binary = tmp / "check"
            generated = sorted(tmp.glob("mission_*.c"))
            compile_result = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/mission_module.c"),
                *(str(path) for path in generated), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("all native mission callbacks passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
