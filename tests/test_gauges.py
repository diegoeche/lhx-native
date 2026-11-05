"""Exercise original gauge line backends and bitmap restoration as native C."""
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and (ROOT / "bin/dos/LHX.unpacked.EXE").exists(),
                     "C compiler and supplied game required")
class GaugeRendering(unittest.TestCase):
    def test_line_directions_and_repeated_background_restore(self):
        # Generate through the production adapter, then compile only the four
        # original functions under test. No preexisting build output is used.
        with tempfile.TemporaryDirectory(prefix="lhx-gauges-") as directory:
            tmp = Path(directory)
            generated = subprocess.run([
                sys.executable, str(ROOT / "tools/lift.py"), "--output", str(tmp),
                "--exe", str(ROOT / "bin/dos/LHX.unpacked.EXE"),
                "--pcrecomp", str(ROOT / "third_party/pcrecomp")],
                cwd=ROOT, capture_output=True, text=True)
            self.assertEqual(generated.returncode, 0, generated.stdout + generated.stderr)
            executable = (ROOT / "bin/dos/LHX.unpacked.EXE").read_bytes()
            header = struct.unpack_from("<14H", executable)
            image_size = (header[2] - 1) * 512 + (header[1] or 512)
            image = tmp / "main-image.bin"
            image.write_bytes(executable[header[4] * 16:image_size])
            selected = {"02021", "126BB", "12732", "1C4D3"}
            functions = []
            for path in sorted(tmp.glob("recomp_*.c")):
                text = path.read_text()
                for block in re.split(r"(?=^void fn_[0-9A-F]+_body\(CPU \*cpu\))", text,
                                      flags=re.MULTILINE):
                    match = re.match(r"void fn_([0-9A-F]+)_body", block)
                    if match and match[1] in selected:
                        functions.append(block)
            self.assertEqual(len(functions), len(selected))
            code = tmp / "gauge_functions.c"
            code.write_text('#include "lifted.h"\n' + "\n".join(functions))
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lifted.h"
int g_recomp_tick_budget=1000000;
void recomp_tick(CPU*c){(void)c;g_recomp_tick_budget=1000000;}
void recomp_dispatch(CPU*c,uint16_t s,uint16_t o){
    (void)c;(void)s;(void)o;assert(!"unexpected renderer dispatch");
}
void recomp_unhandled(CPU*c,uint16_t s,uint16_t o,const char*why){
    (void)c;(void)s;(void)o;(void)why;assert(!"unknown authored line variant");
}
void port_out8(CPU*c,uint16_t p,uint8_t v){(void)c;(void)p;(void)v;assert(0);}
void port_out16(CPU*c,uint16_t p,uint16_t v){(void)c;(void)p;(void)v;assert(0);}

enum { RECORD=0xf288, LEFT=208, TOP=103, WIDTH=48, HEIGHT=43,
       SCREEN=0x5000, SAVED=0x6000, COLOR=14 };
static unsigned char clean[65536], expected[65536];
static void word(CPU*c,unsigned off,unsigned value){mem_write16(c,c->ds,off,value);}
static int floor14(int value){return value>=0?value/16384:-((-value+16383)/16384);}
static void restore(CPU*c){
    c->sp=0x7000;push16(c,RECORD);push16(c,0xffff);fn_02021(c);
    assert(c->sp==0x7000);
    assert(!memcmp(c->mem+SCREEN*16,clean,sizeof(clean)));
}
static void line(CPU*c,int x0,int y0,int x1,int y1,unsigned initial_phase){
    int dx=x1-x0,dy=y1-y0,xmajor=abs(dx)>abs(dy);
    if((xmajor && dx<0)||(!xmajor && dy<0)){
        int t=x0;x0=x1;x1=t;t=y0;y0=y1;y1=t;dx=-dx;dy=-dy;
    }
    int major=xmajor?dx:dy,minor=xmajor?dy:dx;
    int slope=minor*16384/major;
    unsigned phase=initial_phase;
    if(xmajor && slope<0)phase|=0x3fff; /* Authored descending-row rounding. */
    memcpy(expected,clean,sizeof(expected));
    for(int n=0;n<=major;n++){
        /* Original X-major code tests BX!=0 before changing the row:
         * a final bucket transition retains the penultimate pixel's row. */
        int minor_step=xmajor && n==major?n-1:n;
        int offset=floor14((int)phase+minor_step*slope)-floor14(phase);
        int x=x0+(xmajor?n:offset),y=y0+(xmajor?offset:n);
        assert(x>=LEFT && x<LEFT+WIDTH && y>=TOP && y<TOP+HEIGHT);
        expected[y*320+x]=COLOR;
    }
    word(c,0x306,initial_phase);word(c,0x308,slope);word(c,0x30a,major);
    c->si=x0;c->di=y0;c->es=SCREEN;c->bp=0x7800;
    mem_write16(c,c->ss,c->bp+6,0xff00|COLOR);
    c->sp=0x7000;push16(c,0xffff);
    if(xmajor)fn_126BB(c);else fn_12732(c);
    assert(c->sp==0x7000);
    if(memcmp(c->mem+SCREEN*16,expected,sizeof(expected))){
        fprintf(stderr,"line (%d,%d)-(%d,%d), phase %x, slope %d, major %d\n",
                x0,y0,x1,y1,phase,slope,major);
        for(unsigned p=0;p<65536;p++)if(c->mem[SCREEN*16+p]!=expected[p]){
            fprintf(stderr,"first difference (%u,%u): actual %u expected %u\n",
                    p%320,p/320,c->mem[SCREEN*16+p],expected[p]);break;
        }
        assert(!"line pixels differ");
    }
    int left=x0<x1?x0:x1,top=y0<y1?y0:y1;
    word(c,RECORD+0x20,left);word(c,RECORD+0x22,top);
    word(c,RECORD+0x24,abs(dx)+1);word(c,RECORD+0x26,abs(dy)+1);
    restore(c); /* The complete screen must be clean, including outside dirty bounds. */
}
int main(int argc,char **argv){
    assert(argc==2);
    CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);
    FILE*f=fopen(argv[1],"rb");assert(f);
    assert(fread(c.mem+0x1100,1,292816,f)==292816);fclose(f);
    c.ds=c.ss=0x38ab;c.cs=0x10e1;c.flags=0x202;
    memset(clean,0x19,sizeof(clean));
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++){
        unsigned char pixel=1+(x*3+y*7)%13;
        clean[(TOP+y)*320+LEFT+x]=pixel;
        mem_write8(&c,SAVED,y*WIDTH+x,pixel);
    }
    memcpy(c.mem+SCREEN*16,clean,sizeof(clean));
    word(&c,0x188,0);word(&c,0xeb68,1);word(&c,0xecfe,SCREEN);
    word(&c,0xed08,1);word(&c,0xed0a,320);word(&c,0xed0c,200);
    word(&c,0xed0e,320);word(&c,0xed10,SCREEN);word(&c,0x4da,0xffff);
    for(unsigned y=0;y<200;y++)word(&c,0xeb6e + y*2,y*320);
    word(&c,RECORD,LEFT);word(&c,RECORD+2,TOP);
    word(&c,RECORD+4,WIDTH);word(&c,RECORD+6,HEIGHT);
    word(&c,RECORD+0x30,0);word(&c,RECORD+0x32,WIDTH);
    word(&c,RECORD+0x34,HEIGHT);word(&c,RECORD+0x36,WIDTH);
    word(&c,RECORD+0x38,SAVED);
    /* Exact quarter slopes cover all eight octants, both endpoint orders,
     * flat lines, and repeated switches of the authored opcode variants. */
    for(unsigned phase=0;phase<=0x3000;phase+=0x1000)
        for(int dx=-16;dx<=16;dx+=4)for(int dy=-16;dy<=16;dy+=4){
            if(!dx&&!dy)continue;
            if(abs(dx)!=16 && abs(dy)!=16)continue;
            line(&c,232,126,232+dx,126+dy,phase);
            line(&c,232+dx,126+dy,232,126,phase);
        }
    for(unsigned y=0;y<HEIGHT;y++)for(unsigned x=0;x<WIDTH;x++)
        assert(mem_read8(&c,SAVED,y*WIDTH+x)==1+(x*3+y*7)%13);
    free(c.mem);puts("gauge directions, exact pixels, bounds and repeated restore passed");
}
''')
            binary = tmp / "check"
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(code), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary), str(image)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("gauge directions, exact pixels, bounds and repeated restore passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
