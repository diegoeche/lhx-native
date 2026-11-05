"""Check the generated-code replacement against native x86 arithmetic."""

from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and platform.machine() in ("x86_64", "amd64"),
                     "native x86 C compiler required for arithmetic oracle")
class Projection(unittest.TestCase):
    def test_native_projection(self):
        # No game bytes or generated sources are needed for these assertions.
        # The empty generated header supplies only the unused prototype layer.
        with tempfile.TemporaryDirectory(prefix="lhx-projection-") as directory:
            tmp = Path(directory)
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "projection.c"
const uint16_t g_load_seg = 0x110;
static int slow_calls, errors;
void recomp_dispatch(CPU *c, uint16_t seg, uint16_t off) {
    assert(seg == 0x10e1 && off == 0x8cf7); slow_calls++; c->sp += 2;
}
void recomp_unhandled(CPU *c, uint16_t cs, uint16_t ip, const char *why) {
    (void)c; (void)cs; (void)ip; (void)why; errors++;
}

/* Execute the fixed arithmetic template on this machine's x86 hardware.
 * This independently checks the preserved-AL shortcut and carry propagation.
 */
static uint32_t reference(uint16_t value, unsigned shift) {
    uint32_t a = value, d = value & 0x8000 ? 0xffff : 0;
    if (shift < 6) {
        for (unsigned i = 0; i < shift; i++)
            __asm__ volatile("shlw $1, %%ax; rclw $1, %%dx" : "+a"(a), "+d"(d) :: "cc");
    } else {
        __asm__ volatile("movb %%dl, %%dh; movb %%ah, %%dl; movb %%al, %%ah" : "+a"(a), "+d"(d));
        if (shift < 8) {
            for (unsigned i = shift; i < 8; i++)
                __asm__ volatile("sarw $1, %%dx; rcrw $1, %%ax" : "+a"(a), "+d"(d) :: "cc");
        } else {
            for (unsigned i = 8; i < shift; i++)
                __asm__ volatile("shlw $1, %%ax; rclw $1, %%dx" : "+a"(a), "+d"(d) :: "cc");
        }
    }
    return ((d & 0xffff) << 16) | (a & 0xffff);
}
static void point(CPU *c, int x, int y, int z, unsigned shift) {
    c->ax = 0x100; c->si = 0x1234; c->di = 0x5678; c->sp = 0xff00;
    mem_write16(c,c->ds,0x102,x); mem_write16(c,c->ds,0x106,y);
    mem_write16(c,c->ds,0x10a,z); mem_write16(c,c->ds,0xf02f,shift);
    mem_write16(c,c->ds,0xeb52,160); mem_write16(c,c->ds,0xeb54,100);
    lhx_project(c);
    assert(c->si == 0x1234 && c->di == 0x5678 && c->sp == (shift ? 0xff02 : 0xff00));
}
int main(void) {
    CPU c = {0}; c.mem = calloc(1,MEM_SIZE); assert(c.mem); c.ds=0x200;
    for (unsigned shift=1;shift<=16;shift++)
        for (unsigned value=0;value<=0xffff;value++)
            assert(scaled_numerator(&c,value,shift) == reference(value,shift));
    point(&c,100,50,100,8); assert(c.ax==417 && (int16_t)c.cx==-28 && c.dx==0);
    point(&c,-100,-50,100,8); assert((int16_t)c.ax==-94 && c.cx==225 && (int16_t)c.dx==-44);
    point(&c,10,-10,100,5); assert(c.ax==163 && c.cx==103 && c.dx==20);
    point(&c,32767,-32768,1,8); assert(c.ax==32672 && c.cx==32612);
    point(&c,1,-1,0,8); assert(c.ax==10000 && (int16_t)c.cx==-10000);
    point(&c,0,0,0,8); assert(c.ax==160 && c.cx==100);
    point(&c,100,0,-100,8); assert((int16_t)c.ax==-97 && c.cx==100);
    point(&c,1,1,1,0); assert(errors==1 && c.sp==0xff00);
    mem_write8(&c,c.ds,0xf1f8,1); c.sp=0x100; lhx_project(&c);
    assert(slow_calls==1 && c.sp==0x102);
    free(c.mem); puts("projection arithmetic passed");
}
''')
            binary = tmp / "check"
            subprocess.run(["cc", "-std=c11", "-O2", "-I", str(tmp), "-I",
                            str(ROOT / "runtime"), str(source), "-o", str(binary)],
                           check=True, capture_output=True, text=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("projection arithmetic passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
