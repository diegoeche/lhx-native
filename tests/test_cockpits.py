"""Compare all four native cockpit blitters against their authored raster runs."""
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from lift_cockpits import cockpit_payloads, generate


def reference_frame(payload):
    """At test-build time extract fixed draw runs, independently of the lifter.

    This pinned renderer format has immediate ES pixel stores and literal
    destination/count pairs for a contiguous source raster stream. No guest
    CPU execution or runtime instruction decoding is involved.
    """
    entry, palette = struct.unpack_from("<HH", payload)
    prologue = bytes.fromhex("558bec56571e8cc88ed88b46068ec0bf0000be0400")
    assert payload[entry:entry + len(prologue)] == prologue
    assert payload[-5:] == bytes.fromhex("1f5f5e5dcb")
    body = payload[entry + len(prologue):-5]
    frame, source, dest, count, ip = bytearray([0xa5]) * 65536, 4, 0, 0, 0
    while ip < len(body):
        if body[ip] in (0xbf, 0xb9):
            value = struct.unpack_from("<H", body, ip + 1)[0]
            if body[ip] == 0xbf:
                dest = value
            else:
                count = value
            ip += 3
        elif body[ip:ip + 2] == b"\xf3\xa4":
            assert source + count <= palette and dest + count <= 64000
            frame[dest:dest + count] = payload[source:source + count]
            source, dest, count = source + count, dest + count, 0
            ip += 2
        elif body[ip:ip + 3] in (b"\x26\xc6\x06", b"\x26\xc7\x06"):
            size = 1 if body[ip + 1] == 0xc6 else 2
            pixel = struct.unpack_from("<H", body, ip + 3)[0]
            assert pixel + size <= 64000
            frame[pixel:pixel + size] = body[ip + 5:ip + 5 + size]
            ip += 5 + size
        else:
            raise AssertionError(f"Unexpected static raster command at {entry + len(prologue) + ip:#x}")
    assert count == 0
    return bytes(frame)


@unittest.skipUnless(shutil.which("cc") and (ROOT / "bin/dos/LHX3.LIB").exists(),
                     "C compiler and supplied cockpit files required")
class NativeCockpits(unittest.TestCase):
    def test_all_four_framebuffers_and_call_contracts(self):
        with tempfile.TemporaryDirectory(prefix="lhx-cockpits-") as directory:
            tmp = Path(directory)
            payloads = cockpit_payloads(ROOT / "bin/dos")
            report = generate(ROOT / "bin/dos", tmp, ROOT / "third_party/pcrecomp")
            self.assertEqual(len(report["native_modules"]), 4)
            self.assertEqual(sorted(m["instructions"] for m in report["native_modules"]),
                             [52, 168, 224, 267])
            self.assertEqual(report["unhandled_instructions"], [])
            for name, payload in payloads.items():
                frame = reference_frame(payload)
                self.assertGreater(sum(pixel != 0xa5 for pixel in frame), 2000)
                (tmp / name).write_bytes(frame)
            (tmp / "recomp_all.h").write_text("")
            source = tmp / "check.c"
            source.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cockpit_module.h"
static unsigned misses;
void recomp_unhandled(CPU*c,uint16_t s,uint16_t o,const char*why){
    (void)c;(void)s;(void)o;(void)why;misses++;
}
void recomp_dispatch(CPU*c,uint16_t s,uint16_t o){
    (void)c;(void)s;(void)o;assert(!"unexpected cockpit far-return trampoline");
}
int main(int argc,char **argv){
    assert(argc==2);
    CPU c={0};c.mem=calloc(1,MEM_SIZE);assert(c.mem);
    c.ss=0x3000;c.sp=0x8000;
    unsigned char expected[65536];
    for(unsigned i=0;i<g_cockpit_module_count;i++){
        const LhxCockpitModule*m=&g_cockpit_modules[i];
        char path[4096];snprintf(path,sizeof(path),"%s/%s",argv[1],m->name);
        FILE*f=fopen(path,"rb");assert(f);
        assert(fread(expected,1,sizeof(expected),f)==sizeof(expected));fclose(f);
        for(unsigned placement=0;placement<2;placement++){
            uint16_t seg=placement?0x7800:0x5000;
            memcpy(c.mem+(unsigned)seg*16,m->identity,m->size);
            memset(c.mem+0xa0000,0xa5,65536);
            c.cs=0x4321;c.ip=0xabcd;c.ds=0x2000;c.es=0x7777;
            c.ax=0;c.bx=0x1111;c.dx=0x2222;c.cx=0x3333;
            c.si=0x4444;c.di=0x5555;c.bp=0x6666;c.flags=0x0246;
            push16(&c,0xa000);push16(&c,c.cs);push16(&c,0xffff);
            assert(lhx_cockpit_dispatch(&c,seg,m->entry)==1);
            assert(c.sp==0x7ffe);c.sp+=2;
            assert(c.cs==0x4321 && c.ip==0xabcd && c.ds==0x2000);
            assert(c.si==0x4444 && c.di==0x5555 && c.bp==0x6666);
            assert(c.bx==0x1111 && c.dx==0x2222 && c.flags==0x0246);
            assert(c.es==0xa000 && c.ax==0xa000 && c.cx==0);
            assert(!memcmp(c.mem+0xa0000,expected,sizeof(expected)));
            assert(!memcmp(c.mem+(unsigned)seg*16,m->identity,m->size));
            assert(lhx_cockpit_dispatch(&c,seg,m->entry+1)==1);
            assert(misses==i*2+placement+1);
            c.mem[(unsigned)seg*16+m->entry]^=1;
            assert(lhx_cockpit_dispatch(&c,seg,m->entry)==0);
        }
    }
    assert(lhx_cockpit_dispatch(&c,0x4000,0)==0);
    free(c.mem);puts("all native cockpit pixels and call contracts passed");
}
''')
            binary = tmp / "check"
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(tmp), "-I", str(ROOT / "runtime"),
                str(source), str(ROOT / "runtime/cockpit_module.c"),
                *(str(path) for path in sorted(tmp.glob("cockpit_*.c"))), "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(binary), str(tmp)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("all native cockpit pixels and call contracts passed", result.stdout)


if __name__ == "__main__":
    unittest.main()
