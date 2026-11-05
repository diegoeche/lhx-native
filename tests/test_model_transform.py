"""Native model graph checks; optional recorded-code fixture is decoded offline."""

from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "model_transform.h"
static unsigned errors;
int g_recomp_tick_budget=1000000;
void recomp_tick(CPU*c){(void)c;g_recomp_tick_budget=1000000;}
void recomp_unhandled(CPU*c,uint16_t seg,uint16_t off,const char*reason){
    (void)c;(void)seg;(void)off;fprintf(stderr,"unexpected: %s\n",reason);errors++;
}
void fn_193C5(CPU*c){
    c->bx=(uint16_t)(mem_read16(c,c->ds,0xf1ff)*4);c->ax=c->bx;
    c->bx=(uint16_t)(c->bx*2);c->bx=flags_add16(c,c->bx,c->ax);c->sp+=4;
}
static void point(CPU*c,uint16_t lod,unsigned index,unsigned previous,const signed char*coeff){
    mem_write8(c,c->ds,0xc494,mem_read16(c,c->ss,c->sp+4)&8);
    for(unsigned i=0;i<48;i++)mem_write8(c,c->ds,0x1800+i,coeff[i]);
    mem_write16(c,c->ss,c->sp+2,0x1800);mem_write16(c,c->ss,c->sp+6,previous);
    mem_write16(c,c->ss,c->sp+8,index);lhx_model_compile_point(c);
}
static unsigned begin(CPU*c,uint16_t lod,unsigned count,unsigned wide){
    mem_write8(c,c->ds,lod+1,count);mem_write16(c,c->ss,c->sp+4,wide?8:0);
    mem_write16(c,c->ss,c->sp+14,lod);return lhx_model_compile_begin(c);
}
static void seal(CPU*c,uint16_t lod,unsigned token,uint16_t seg){
    mem_write8(c,c->ds,lod+13,2);mem_write16(c,c->ds,lod+6,0);
    mem_write16(c,c->ds,lod+8,seg);lhx_model_compile_end(c,token);
}
static void invoke(CPU*c,uint16_t seg){
    uint16_t sp=c->sp,cs=c->cs,ip=c->ip;c->si=0x1234;c->di=0x5678;
    push16(c,cs);push16(c,0xffff);assert(lhx_model_dispatch(c,seg,0)==1);
    assert(c->sp==sp&&c->cs==cs&&c->ip==ip&&c->si==0x1234&&c->di==0x5678);
}
'''


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class ModelTransform(unittest.TestCase):
    def compile_and_run(self, source, inputs=()):
        with tempfile.TemporaryDirectory(prefix="lhx-model-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("void fn_193C5(CPU *);\n")
            (tmp / "check.c").write_text(HARNESS + source)
            binary = tmp / "check"
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(tmp / "check.c"), str(ROOT / "runtime/model_transform.c"),
                "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary), *(str(p) for p in inputs)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_predecessor_graph_and_32bit_carries(self):
        self.compile_and_run(r'''
static uint32_t value(CPU*c,unsigned axis,unsigned bit,unsigned component){
    return mem_read32(c,c->ds,0x874+(axis*16+bit+1)*12+component*4);
}
int main(void){
    CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);c.cs=0x4321;c.ip=0xabcd;
    c.ss=0x3000;c.sp=0x8000;c.ds=0x2000;
    for(unsigned wide=0;wide<2;wide++){
        lhx_model_reset();memset(c.mem,0,MEM_SIZE);c.dx=0x9876;
        mem_write16(&c,c.ds,0x70e,0x7000);mem_write16(&c,c.ds,0xf1ff,1);
        for(unsigned axis=0;axis<3;axis++)for(unsigned bit=0;bit<15;bit++)
            for(unsigned k=0;k<3;k++)mem_write32(&c,c.ds,0x874+(axis*16+bit+1)*12+k*4,
                0x7ff0fff1u+axis*0x123456u+bit*0x530001u+k*0x170177u);
        for(unsigned k=0;k<3;k++)mem_write32(&c,c.ds,0xda4+k*4,0x1234fff0u+k*0x4171u);
        unsigned token=begin(&c,0x100,3,wide);assert(token);
        signed char a[48]={0},b[48]={0},d[48]={0};
        a[1]=1;b[0]=1;b[16+4]=1;b[16+1]=-1;d[1]=-2;d[32+2]=2;
        point(&c,0x100,0,0xff,a);point(&c,0x100,1,0,b);point(&c,0x100,2,0,d);
        seal(&c,0x100,token,0x6000);
        int32_t xyz[9];
        assert(lhx_model_source_vertices(0x6000,0,xyz,3));
        const int32_t expected[9]={2,0,0,3,14,0,-2,0,8};
        assert(!memcmp(xyz,expected,sizeof(xyz)));
        assert(!lhx_model_source_vertices(0x6000,0,xyz,2));
        assert(!lhx_model_source_vertices(0x6001,0,xyz,3));
        /* Compiled output addresses are fixed at compilation time. */
        mem_write16(&c,c.ds,0x70e,0x7100);invoke(&c,0x6000);
        for(unsigned k=0;k<3;k++){
            uint32_t t=mem_read32(&c,c.ds,0xda4+k*4);
            uint32_t x1=value(&c,0,1,k),x0=value(&c,0,0,k);
            uint32_t y4=value(&c,1,4,k),y1=value(&c,1,1,k),z2=value(&c,2,2,k);
            if(wide){
                uint32_t p0=t+x1,p1=p0+x0+y4-y1,p2=p0-x1-x1+z2+z2;
                assert(mem_read32(&c,c.ds,0x7000+k*4)==p0);
                assert(mem_read32(&c,c.ds,0x7010+k*4)==p1);
                assert(mem_read32(&c,c.ds,0x7020+k*4)==p2);
            }else{
                uint16_t p0=(t>>16)+(x1>>16),p1=p0+(x0>>16)+(y4>>16)-(y1>>16);
                uint16_t p2=p0-(x1>>16)-(x1>>16)+(z2>>16)+(z2>>16);
                assert(mem_read16(&c,c.ds,0x7002+k*4)==p0);
                assert(mem_read16(&c,c.ds,0x7012+k*4)==p1);
                assert(mem_read16(&c,c.ds,0x7022+k*4)==p2);
                assert(mem_read16(&c,c.ds,0x7000+k*4)==0);
            }
        }
        assert(!errors);assert(lhx_model_dispatch(&c,0x6001,0)==0);
        /* A shared near LOD address can describe a different far model later.
         * Its older code allocation must retain the older coefficient graph.
         */
        uint16_t previous_output=mem_read16(&c,c.ds,0x7002);
        unsigned later=begin(&c,0x100,1,wide);signed char identity[48]={0};
        point(&c,0x100,0,0xff,identity);seal(&c,0x100,later,0x6100);
        memset(c.mem+((unsigned)c.ds<<4)+0x7000,0,48);
        invoke(&c,0x6000);assert(mem_read16(&c,c.ds,0x7002)==previous_output);
        invoke(&c,0x6100);
        assert(lhx_model_source_vertices(0x6000,0,xyz,3));
        assert(!memcmp(xyz,expected,sizeof(xyz)));
        if(wide)assert(mem_read32(&c,c.ds,0x7100)==mem_read32(&c,c.ds,0xda4));
        else assert(mem_read16(&c,c.ds,0x7102)==mem_read16(&c,c.ds,0xda6));
        assert(!errors);
    }
    lhx_model_reset();free(c.mem);puts("native model graph passed");
}
''')

    @unittest.skipUnless((ROOT / "build/runtime-fourteenth.ram").exists(),
                         "recorded LHX model-generation fixture absent")
    def test_recorded_generated_column_matches_native_graph(self):
        # Only the TEST BUILD reads recorded instruction bytes. It translates
        # the straight-line column into C as an independent native oracle.
        # The shipping runtime observes original compiler coefficient data.
        fixture = ROOT / "build/runtime-fourteenth.ram"
        ram = fixture.read_bytes()
        ds = 0x38AB
        word = lambda off: struct.unpack_from("<H", ram, ds * 16 + off)[0]
        points = word(0x70E)
        code = ram[0x656A0:0x756A0]
        self.assertEqual(code[:2], b"\x56\x57")
        cursor, predecessor, last, terms = 0x24, 0xFF, None, [0] * 48
        plans, oracle = [], []
        while code[cursor] != 0xC3:
            operation = code[cursor:cursor + 2]
            disp = struct.unpack_from("<H", code, cursor + 2)[0]
            if operation == b"\x8b\x84":
                predecessor = 0xFF if disp == 0xDA6 else (disp - points - 2) // 16
                oracle.append(f"c->ax=mem_read16(c,c->ds,(uint16_t)(c->si+0x{disp:04x}));")
            elif operation in (b"\x03\x85", b"\x2b\x85"):
                index = (disp - 0x876) // 12
                self.assertEqual((disp - 0x876) % 12, 0)
                axis, bit = divmod(index, 16)
                self.assertTrue(0 <= axis < 3 and bit < 15)
                terms[axis * 16 + bit] += 1 if operation == b"\x03\x85" else -1
                function = "flags_add16" if operation == b"\x03\x85" else "flags_sub16"
                oracle.append(f"c->ax={function}(c,c->ax,mem_read16(c,c->ds,(uint16_t)(c->di+0x{disp:04x})));")
            elif operation == b"\x89\x84":
                vertex = (disp - points - 2) // 16
                self.assertEqual((disp - points - 2) % 16, 0)
                plans.append((vertex, predecessor, terms))
                oracle.append(f"mem_write16(c,c->ds,(uint16_t)(c->si+0x{disp:04x}),c->ax);")
                terms, predecessor, last = [0] * 48, vertex, vertex
            else:
                self.fail(f"unexpected recorded fixture operation at {cursor:#x}")
            cursor += 4
        self.assertEqual(len(plans), 119)
        setup = []
        for vertex, predecessor, coefficients in plans:
            initializer = ",".join(map(str, coefficients))
            setup.append(f"{{signed char coeff[48]={{{initializer}}};point(&c,0x100,{vertex},{predecessor},coeff);}}")
        source = ("\nstatic void oracle(CPU*c){\n" + "\n".join(oracle) + "\n}\n" + r'''
int main(int argc,char**argv){
    assert(argc==2);CPU c={0},r={0};c.mem=calloc(1,MEM_SIZE);r.mem=calloc(1,MEM_SIZE);
    FILE*f=fopen(argv[1],"rb");assert(f);assert(fread(c.mem,1,MEM_SIZE,f)==MEM_SIZE);fclose(f);
    c.ds=0x38ab;c.ss=0x2000;c.sp=0x8000;c.cs=0x10e1;c.ip=0xd05e;
    unsigned token=begin(&c,0x100,119,0);assert(token);
''' + "\n".join(setup) + r'''
    seal(&c,0x100,token,0x656a);
    memcpy(r.mem,c.mem,MEM_SIZE);r.ds=c.ds;r.ss=c.ss;r.sp=c.sp;r.flags=c.flags;
    /* Keep the live tables, including their independently rounded powers. */
    push16(&r,r.cs);push16(&r,0xffff);fn_193C5(&r);
    for(unsigned k=0;k<3;k++){r.si=k*4;r.di=r.bx+k*4;oracle(&r);}
    invoke(&c,0x656a);
    assert(memcmp(c.mem+((unsigned)c.ds<<4)+0xd866,
                  r.mem+((unsigned)r.ds<<4)+0xd866,119*16)==0);
    assert(c.ax==r.ax && c.dx==r.dx && c.flags==r.flags && !errors);
    lhx_model_reset();free(c.mem);free(r.mem);puts("recorded generated column matches native graph");
}
''')
        self.compile_and_run(source, [fixture])


if __name__ == "__main__":
    unittest.main()
