"""Execute real generated C for instruction forms used in LHX's string CRT.

These tests require the vendored pcrecomp files and a C compiler, not game data.
"""

from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path[:0] = [str(ROOT / "tools"),
                str(ROOT / "third_party/pcrecomp/tools/disasm"),
                str(ROOT / "third_party/pcrecomp/tools/lift")]
import decode16
import lift16
import lift


class NativeStringCopies(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler required for native lifting regression")
        lift.decode16 = decode16
        lift.OpType = decode16.OpType
        cls.temp = tempfile.TemporaryDirectory(prefix="lhx-lift-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        cls.directory = Path(cls.temp.name)
        functions = []
        # The second sequence is precisely MSC's size-driven copy strategy:
        # count/2 words, then the low-bit carry becomes a byte count of 0 or 1.
        for name, raw in [("copy_words", bytes.fromhex("f2 a5 c3")),
                          ("copy_bytes", bytes.fromhex("f2 a4 c3")),
                          ("copy_mixed", bytes.fromhex("d1 e9 f2 a5 13 c9 f2 a4 c3"))]:
            decoder = decode16.Decoder(raw)
            instructions = []
            while decoder.pos < len(raw):
                instructions.append(decoder.decode_one())
            lift16._CODE_SEG = "0000"
            functions.append(lift16.Lifter().lift_function(
                name, lift.prepare_instructions(instructions), 0))
        # A near assembly helper removes its return, restores its parent's BP,
        # then executes the parent's far return. The parent C continuation must
        # stop, preserving AX rather than executing MOV AX,DEAD and another RETF.
        raw = bytes.fromhex("55 8b ec e8 05 00 b8 ad de 5d cb 5b 5d cb")
        decoder = decode16.Decoder(raw)
        all_instructions = []
        while decoder.pos < len(raw):
            all_instructions.append(decoder.decode_one())
        lifter = lift16.Lifter(known_funcs={11: "fn_0000B"})
        lifter.dispatch = True
        lifter.unwind_sp = True
        functions += ["void recomp_dispatch(CPU *c,uint16_t s,uint16_t o) {(void)c;(void)s;(void)o;abort();}",
                      "void fn_0000B(CPU *cpu);",
                      lifter.lift_function("unwind_parent", lift.prepare_instructions(
                          [i for i in all_instructions if i.address < 11]), 0),
                      lifter.lift_function("fn_0000B", lift.prepare_instructions(
                          [i for i in all_instructions if i.address >= 11]), 0)]
        cpu_header = ROOT / "third_party/pcrecomp/runtime/cpu.h"
        source = '#include <stdlib.h>\n#include <stdio.h>\n#include "' + str(cpu_header) + '"\n'
        source += "#define SEG_0000 0x110\n" + "\n".join(functions)
        source += r'''
static int check(void (*fn)(CPU*), unsigned count, unsigned bytes) {
    CPU c = {0};
    c.mem = calloc(1, MEM_SIZE);
    if (!c.mem) return 10;
    c.ds=0x200; c.es=0x300; c.si=0x20; c.di=0x50; c.sp=0xf000;
    c.cx=count; c.flags=0;
    for(unsigned i=0;i<64;i++) {
        mem_write8(&c,c.ds,0x20+i,(uint8_t)(0x31+i));
        mem_write8(&c,c.es,0x50+i,0xee);
    }
    fn(&c);
    int bad = c.cx != 0 || c.si != 0x20+bytes || c.di != 0x50+bytes || c.sp != 0xf002;
    for(unsigned i=0;i<64;i++) {
        uint8_t want = i < bytes ? (uint8_t)(0x31+i) : 0xee;
        if(mem_read8(&c,c.es,0x50+i) != want) bad=1;
    }
    if(bad) fprintf(stderr,"copy count=%u bytes=%u CX=%u SI=%x DI=%x\n",count,bytes,c.cx,c.si,c.di);
    free(c.mem);
    return bad;
}
int main(int argc,char **argv) {
    if(argc != 3) return 2;
    unsigned count=(unsigned)strtoul(argv[2],NULL,10);
    if(argv[1][0]=='u') {
        CPU c={0}; c.mem=calloc(1,MEM_SIZE); if(!c.mem) return 10;
        c.ss=0x400; c.sp=0xf000; c.bp=0x5678; c.ax=0x1234;
        mem_write16(&c,c.ss,c.sp,0xffff); mem_write16(&c,c.ss,c.sp+2,0x110);
        unwind_parent(&c);
        int bad=c.ax!=0x1234 || c.bp!=0x5678 || c.sp!=0xf004;
        if(bad) fprintf(stderr,"unwind AX=%x BP=%x SP=%x\n",c.ax,c.bp,c.sp);
        free(c.mem); return bad;
    }
    if(argv[1][0]=='w') return check(copy_words,count,count*2);
    if(argv[1][0]=='b') return check(copy_bytes,count,count);
    return check(copy_mixed,count,count);
}
'''
        src = cls.directory / "copy.c"
        src.write_text(source)
        cls.executable = cls.directory / "copy"
        subprocess.run([compiler, "-std=c11", "-O2", str(src), "-o", str(cls.executable)],
                       check=True, capture_output=True, text=True)

    def check_copy(self, kind, count):
        result = subprocess.run([str(self.executable), kind, str(count)],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_repne_movsw_counts(self):
        for count in (0, 1, 4):
            with self.subTest(count=count):
                self.check_copy("words", count)

    def test_repne_movsb_counts(self):
        for count in (0, 1, 4):
            with self.subTest(count=count):
                self.check_copy("bytes", count)

    def test_msc_word_then_odd_byte_copy(self):
        for count in (0, 1, 2, 3, 4, 8, 9, 16, 17):
            with self.subTest(count=count):
                self.check_copy("mixed", count)

    def test_near_helper_unwinds_parent_far_frame(self):
        self.check_copy("unwind", 0)


class NativeDivideFaults(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("C compiler required for native lifting regression")
        lift.decode16, lift.OpType = decode16, decode16.OpType
        cls.temp = tempfile.TemporaryDirectory(prefix="lhx-divide-tests-")
        cls.addClassCleanup(cls.temp.cleanup)
        directory = Path(cls.temp.name)

        # Match LHX's interrupt-frame strategy: pop IP/CS into module data,
        # call a near saturation helper while SP is above the ISR entry SP,
        # then rebuild the frame and IRET. Zero-divisor recovery changes IP.
        raw = bytearray.fromhex(
            "2e 8f 06 00 03 2e 8f 06 02 03 53 e8 00 00 5b "
            "2e ff 36 02 03 2e ff 36 00 03 cf")
        helper = 0x100 + len(raw)
        raw[12:14] = (helper - 0x10e).to_bytes(2, "little")
        raw += bytearray.fromhex(
            "83 ff 00 74 0a b8 00 7f 2e 83 06 00 03 02 c3 "
            "b8 10 27 2e c7 06 00 03 20 00 c3")

        def decode(data, base=0):
            decoder = decode16.Decoder(data, 0)
            result = []
            while decoder.pos < len(data):
                instruction = decoder.decode_one()
                instruction.address += base
                instruction.offset += base
                for operand in (instruction.op1, instruction.op2):
                    if operand and operand.type in (decode16.OpType.REL8, decode16.OpType.REL16):
                        operand.disp += base
                result.append(instruction)
            return result

        instructions = decode(bytes(raw), 0x100)
        helper_name = f"fn_{helper:05X}"
        lifter = lift16.Lifter(known_funcs={helper: helper_name})
        lifter.dispatch, lifter.iret_frame = True, True
        lift16._CODE_SEG = "0000"
        original_emit = lifter.lift_instruction
        lifter.lift_instruction = lambda instruction, start: lift.emit_with_fault_resume(
            lifter, original_emit, instruction, start)
        functions = []
        for name, body, entry in [
                ("fault_isr_body", [i for i in instructions if i.address < helper], 0x100),
                (helper_name, [i for i in instructions if i.address >= helper], helper),
                ("faulting_body", decode(bytes.fromhex("f7 ff b9 11 11 cb")), 0),
                ("redirect_body", decode(bytes.fromhex("b9 22 22 cb"), 0x20), 0x20)]:
            body = lift.prepare_instructions(body)
            lift.configure_frame_unwind(lifter, body)
            functions.append(lifter.lift_function(name, body, 0, entry_addr=entry))

        # Compile the actual runtime fault dispatcher, avoiding SDL/application
        # setup. The test's dispatch table selects only the generated C above.
        runtime = (ROOT / "runtime/main.c").read_text()
        fault_runtime = runtime[runtime.index("int recomp_div0("):runtime.index("\nint cpu_lar(")]
        header = ROOT / "third_party/pcrecomp/runtime/cpu.h"
        source = '#include <stdlib.h>\n#include <stdio.h>\n#include "' + str(header) + '"\n'
        source += ("#define SEG_0000 0x110\nstatic CPU machine;\n"
                   "void recomp_dispatch(CPU*,uint16_t,uint16_t);\n"
                   "void recomp_unhandled(CPU*,uint16_t,uint16_t,const char*);\n"
                   "int recomp_div0(const char*,uint16_t);\n"
                   f"void {helper_name}(CPU*);\n" + "\n".join(functions) + "\n" + fault_runtime)
        source += r'''
static unsigned interrupts, redirects;
void recomp_unhandled(CPU *c,uint16_t s,uint16_t o,const char *why) {
    (void)c;(void)s;(void)o;fprintf(stderr,"unexpected fault %s\n",why);abort();
}
void recomp_dispatch(CPU *c,uint16_t seg,uint16_t off) {
    if(seg!=0x110) abort();
    if(off==0x100) {
        interrupts++;
        if(mem_read16(c,c->ss,c->sp)!=0 || mem_read16(c,c->ss,c->sp+2)!=0x110 ||
           mem_read16(c,c->ss,c->sp+4)!=(FLAG_IF|FLAG_TF|FLAG_CF) ||
           (c->flags & (FLAG_IF|FLAG_TF))) abort();
        uint16_t old_cs=c->cs,old_ip=c->ip;
        c->cs=seg;c->ip=off;fault_isr_body(c);c->cs=old_cs;c->ip=old_ip;
    } else if(off==0x20) {
        redirects++;
        if(c->sp!=0xf000) abort();
        redirect_body(c);
    } else abort();
}
int main(int argc,char **argv) {
    (void)argv;
    CPU *c=&machine;c->mem=calloc(1,MEM_SIZE);if(!c->mem)return 10;
    c->cs=0x110;c->ss=0x400;c->ds=0x200;c->sp=0xf000;c->bx=0xbeef;
    c->ax=0x8b00;c->dx=0x65e1;c->di=argc>1?0:0x7530;
    c->flags=FLAG_IF|FLAG_TF|FLAG_CF;
    mem_write16(c,c->ss,c->sp,0xffff);mem_write16(c,c->ss,c->sp+2,0x110);
    mem_write16(c,0,0,0x100);mem_write16(c,0,2,0x110);
    faulting_body(c);
    int zero=argc>1;
    int bad=interrupts!=1 || redirects!=(unsigned)zero || c->sp!=0xf004 ||
        c->bx!=0xbeef || c->ax!=(zero?0x2710:0x7f00) ||
        c->cx!=(zero?0x2222:0x1111) || c->flags!=(FLAG_IF|FLAG_TF|FLAG_CF) ||
        mem_read16(c,0x110,0x300)!=(zero?0x20:2) ||
        mem_read16(c,0x110,0x302)!=0x110;
    if(bad)fprintf(stderr,"fault zero=%d AX=%x BX=%x CX=%x SP=%x flags=%x ISR=%u redirects=%u\n",
        zero,c->ax,c->bx,c->cx,c->sp,c->flags,interrupts,redirects);
    free(c->mem);return bad;
}
'''
        path = directory / "divide.c"
        path.write_text(source)
        cls.executable = directory / "divide"
        subprocess.run([compiler, "-std=c11", "-O2", str(path), "-o", str(cls.executable)],
                       check=True, capture_output=True, text=True)

    def test_overflow_saturates_and_resumes_after_divide(self):
        result = subprocess.run([str(self.executable)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_zero_divisor_dispatches_redirect_and_stops_old_continuation(self):
        result = subprocess.run([str(self.executable), "zero"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
