"""Exercise the recompiled driver's API and interrupt chain without DOS."""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lift_audio import driver_payload, generate


@unittest.skipUnless(shutil.which("cc") and (ROOT / "bin/dos/LHX2.LIB").exists(),
                     "C compiler and supplied game files required")
class NativeAudio(unittest.TestCase):
    def test_driver_calls_and_irq_chain(self):
        with tempfile.TemporaryDirectory(prefix="lhx-audio-") as directory:
            tmp = Path(directory)
            report = generate(ROOT / "bin/dos", tmp, ROOT / "third_party/pcrecomp")
            self.assertEqual(report["instruction_count"], 434)
            self.assertEqual(report["decoded_code_bytes"], 1110)
            self.assertEqual(len(report["function_offsets"]), 18)
            self.assertEqual(report["unhandled_instructions"], [])
            (tmp / "recomp_all.h").write_text("")
            (tmp / "payload.bin").write_bytes(driver_payload(ROOT / "bin/dos"))
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "audio_driver.h"
#include "audio_ibm_gen.h"

int g_recomp_tick_budget=500;
static unsigned missing, bios_ticks, eois, tone_writes;
static uint16_t timer_divisor;
static uint8_t speaker_gate, timer_low;
static int timer_high;
static unsigned observed;
static LhxAudioSnapshot latest;
static void observe(const LhxAudioSnapshot *snapshot,void *unused) {
    (void)unused;observed++;latest=*snapshot;
}
void recomp_tick(CPU *c) { (void)c; g_recomp_tick_budget=500; }
void recomp_unhandled(CPU *c,uint16_t seg,uint16_t off,const char *why) {
    (void)c; (void)seg; (void)off; (void)why; missing++;
}
void recomp_dispatch(CPU *c,uint16_t seg,uint16_t off) {
    if(seg==0xf000) {
        assert(off==8); bios_ticks++; c->sp+=4; c->flags=pop16(c); return;
    }
    assert(lhx_audio_dispatch(c,seg,off)==1);
}
void dispatch_near(CPU *c,uint16_t seg,uint16_t off) { recomp_dispatch(c,seg,off); }
void dispatch_far(CPU *c,uint16_t seg,uint16_t off) { recomp_dispatch(c,seg,off); }
uint8_t port_in8(CPU*c,uint16_t p) { (void)c; return p==0x61?speaker_gate:0; }
uint16_t port_in16(CPU*c,uint16_t p) { return port_in8(c,p)|(uint16_t)port_in8(c,p+1)<<8; }
void port_out8(CPU*c,uint16_t p,uint8_t v) {
    (void)c;
    if(p==0x40) {
        if(!timer_high)timer_low=v; else timer_divisor=timer_low|((uint16_t)v<<8);
        timer_high=!timer_high;
    } else if(p==0x42)tone_writes++;
    else if(p==0x61)speaker_gate=v;
    else if(p==0x20 && v==0x20)eois++;
}
void port_out16(CPU*c,uint16_t p,uint16_t v) { port_out8(c,p,v);port_out8(c,p+1,v>>8); }

static void api(CPU*c,uint16_t seg,unsigned command,unsigned a,unsigned b,unsigned d) {
    uint16_t sp=c->sp,ds=c->ds,cs=c->cs,ip=c->ip;
    c->bx=0x1111;c->cx=0x2222;c->dx=0x3333;c->si=0x4444;c->di=0x5555;c->bp=0x6666;
    push16(c,d);push16(c,b);push16(c,a);push16(c,command);
    push16(c,cs);push16(c,0xffff);
    assert(lhx_audio_dispatch(c,seg,0)==1);
    assert(c->sp==(uint16_t)(sp-8));c->sp+=8;
    assert(c->ds==ds && c->cs==cs && c->ip==ip);
    assert(c->bx==0x1111 && c->cx==0x2222 && c->dx==0x3333);
    assert(c->si==0x4444 && c->di==0x5555 && c->bp==0x6666);
}
int main(int argc,char**argv) {
    assert(argc==2);
    FILE*f=fopen(argv[1],"rb");assert(f);
    uint8_t payload[4096];size_t n=fread(payload,1,sizeof payload,f);fclose(f);
    assert(n==4043);
    CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);
    c.cs=0x4321;c.ip=0xabcd;c.ss=0x3000;c.ds=0x1234;c.sp=0x1000;c.flags=FLAG_IF;
    for(unsigned seg=0x6000;seg<=0x7000;seg+=0x1000) {
        memcpy(c.mem+seg*16,payload,n);
        mem_write16(&c,0,0x20,8);mem_write16(&c,0,0x22,0xf000);
        api(&c,seg,0x12,0,0,0);assert(c.ax==0);
        api(&c,seg,0x0e,0,0,0);assert(c.ax==1);
        assert(mem_read16(&c,0,0x20)==0x363 && mem_read16(&c,0,0x22)==seg);
        assert(timer_divisor==0x13b1);
        CPU reference=c;reference.mem=malloc(MEM_SIZE);assert(reference.mem);
        memcpy(reference.mem,c.mem,MEM_SIZE);
        lhx_audio_observe(NULL,NULL);
        api(&reference,seg,0,1,100,0);
        lhx_audio_observe(observe,NULL);
        api(&c,seg,0,1,100,0);assert(c.ax==1);
        assert(!memcmp(c.mem,reference.mem,MEM_SIZE));free(reference.mem);
        for(unsigned tick=0;tick<13;tick++) {
            uint16_t sp=c.sp,cs=c.cs,ip=c.ip;
            push16(&c,c.flags);push16(&c,c.cs);push16(&c,0xffff);
            assert(lhx_audio_dispatch(&c,seg,0x363)==1);
            assert(c.sp==sp && c.cs==cs && c.ip==ip);
        }
        assert(mem_read8(&c,seg,0x4ec)==13);
        assert(observed>=14 && latest.voices[1].active);
        assert(latest.voices[1].effect==1 && latest.voices[1].divisor>0);
        api(&c,seg,0,0,65,0);api(&c,seg,6,0,0,0);
        assert(latest.voices[3].active && latest.voices[3].effect==0);
        assert(latest.voices[3].divisor==65);
        api(&c,seg,4,0,5,0);assert(latest.voices[3].divisor==5);
        api(&c,seg,0,15,3,0);api(&c,seg,6,0,0,0);
        assert(latest.voices[2].active && latest.voices[2].effect==15);
        assert(latest.voices[3].active);
        api(&c,seg,0x18,0x90,60,1);api(&c,seg,6,0,0,0);
        assert(latest.voices[2].active && latest.voices[2].effect==20);
        api(&c,seg,0x08,0,0,0);assert((speaker_gate&3)==0);
        for(unsigned i=0;i<LHX_AUDIO_VOICES;i++)assert(!latest.voices[i].active);
        api(&c,seg,0x0a,0,0,0);assert(mem_read8(&c,seg,0x502)==1);
        api(&c,seg,0x0c,0,0,0);assert(mem_read8(&c,seg,0x502)==0);
        api(&c,seg,0x10,0,0,0);assert(c.ax==1);
        for(unsigned i=0;i<LHX_AUDIO_VOICES;i++)assert(!latest.voices[i].active);
        assert(mem_read16(&c,0,0x20)==8 && mem_read16(&c,0,0x22)==0xf000);
        assert((speaker_gate&3)==0);
    }
    assert(missing==0 && bios_ticks==2 && eois==24 && tone_writes>0);
    assert(lhx_audio_dispatch(&c,0x2000,0)==0);
    assert(lhx_audio_dispatch(&c,0x6000,0x500)==1 && missing==1);
    c.mem[0x60000]^=1;assert(lhx_audio_dispatch(&c,0x6000,0)==0);
    lhx_audio_observe(NULL,NULL);
    free(c.mem);puts("native audio API and IRQ chain passed");
}
''')
            binary = tmp / "check"
            compile_result = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/audio_driver.c"),
                str(tmp / "audio_ibm_code.c"), str(tmp / "audio_ibm_identity.c"),
                "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compile_result.returncode, 0,
                             compile_result.stdout + compile_result.stderr)
            result = subprocess.run([str(binary), str(tmp / "payload.bin")],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("native audio API and IRQ chain passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
