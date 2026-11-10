#!/usr/bin/env python3
"""Lift the owner's unpacked LHX MZ into ahead-of-time compiled C.

No instructions are decoded at run time. Function bodies are recovered by
recursive descent; indirect dispatch only selects already-compiled functions.
The decoder/lifter comes from sp00nznet/pcrecomp (MIT). Discovery and output
layout follow sp00nznet/wolf3d-recomp/tools/lift.py (MIT), without its game
addresses, renderer hooks, Borland startup, or interpreter fallback.

Generated C includes runtime/lifted.h. See lift.json for incomplete coverage.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import struct
import sys

LOAD_SEG = 0x110
PSP_SEG = 0x100
# Semantically recovered bridges for the supplied Hit Squad release. These
# offsets apply only to this exact unpacked module, never to an arbitrary EXE.
LHX_IMAGE_SHA256 = "4d30628da15b38eb93cad06420d255574db7283cfec4292f9bfbc8b9d4369f9e"
LHX_NATIVE_HOOKS = {0x0030c: "lhx_int86", 0x186ca: "lhx_project"}
LHX_NATIVE_OBSERVERS = {0x15f8a: ("lhx_model_compile_begin", "lhx_model_compile_end")}
LHX_NATIVE_POST_OBSERVERS = {0x11096: "lhx_scene_event(cpu, 0x11096)"}
# Exact instruction boundaries established from the original scene renderer.
# These are observations of data; original instructions still execute normally.
LHX_SCENE_EVENTS = {0x1468a, 0x14792, 0x146be, 0x146c3,
                    0x1aa0d, 0x1aa10, 0x1cd76}
LHX_SCENE_CACHE_CHECK, LHX_SCENE_CACHE_MISS = 0x1cbae, 0x1cc5f
LHX_NATIVE_PRE_OBSERVERS = {
    # The original frame clock clamps elapsed time to at least one IRQ tick.
    # Pace before it samples the clock so a fast native loop cannot advance
    # simulation time repeatedly between real timer interrupts.
    0x0bc16: "lhx_frame_pace",
    0x164a8: "lhx_model_compile_point",
}
# 006e:0462 is CMP byte DS:[d855],0. Controller axes must arrive before
# this test and the following TOW save/clear, rather than inside helicopter
# integration. Both native copies of the original main loop include this site.
LHX_NATIVE_INSTRUCTION_OBSERVERS = {
    0x00b42: ("lhx_gamepad_apply", bytes.fromhex("80 3e 55 d8 00")),
}
# The authored line backends select direction by replacing these opcode bytes.
# Each alternative is decoded and compiled here, never decoded at runtime.
# Addresses are covered by the exact unpacked-image digest above.
LHX_OPCODE_VARIANTS = {
    0x12466: (b"\x03\x3e", b"\x2b\x3e"),  # ADD/SUB DI,[row stride]
    0x12487: (b"\x81\xc6", b"\x81\xee"),  # ADD/SUB SI,immediate
    0x12528: (b"\x03\x3e", b"\x2b\x3e"),
    0x12553: (b"\x03\x36", b"\x2b\x36"),  # ADD/SUB SI,[slope]
    0x125ca: (b"\xd0\xcd", b"\xd0\xc5"),  # ROR/ROL CH,1
    0x125cc: (b"\x83\xd7", b"\x83\xdf"),  # ADC/SBB DI,0
    0x12688: (b"\xd0\xcd", b"\xd0\xc5"),
    0x1268a: (b"\x83\xd7", b"\x83\xdf"),
    0x1268f: (b"\xfe\xc0", b"\xfe\xc8"),  # INC/DEC AL
    0x12717: (b"\x81\xc7", b"\x81\xef"),  # MCGA ADD/SUB DI,320
    0x12784: (b"\x47", b"\x4f"),          # MCGA INC/DEC DI
    0x12832: (b"\x47", b"\x4f"),
    0x12833: (b"\x47", b"\x4f"),
    0x128f6: (b"\xd0\xcd", b"\xd0\xc5"),
    0x128f8: (b"\xd0\xcd", b"\xd0\xc5"),
    0x128fa: (b"\x83\xd2", b"\x83\xda"),  # ADC/SBB DX,0
    0x129c1: (b"\x81\xc7", b"\x81\xef"),
    0x129cb: (b"\x81\xc7", b"\x81\xef"),
    0x12a86: (b"\xd2\xcd", b"\xd2\xc5"),  # ROR/ROL CH,CL
    0x12a88: (b"\x83\xd7", b"\x83\xdf"),
}
TERMINATORS = {"ret", "retf", "iret", "hlt"}
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns",
       "jp", "jnp", "jl", "jge", "jle", "jg", "loop", "loopz", "loopnz", "jcxz"}
WRITES = {"mov", "add", "sub", "adc", "sbb", "and", "or", "xor", "inc", "dec",
          "not", "neg", "shl", "sal", "shr", "sar", "rol", "ror", "pop", "xchg"}
PROLOGUES = (b"\x55\x8b\xec", b"\x55\x89\xe5", b"\x1e\x50", b"\x50\x53\x51\x52",
             b"\x60", b"\x1e\x56\x57", b"\x56\x57", b"\x1e\xb8", b"\x9c\x50")


class Image:
    def __init__(self, path):
        self.raw = Path(path).read_bytes()
        if len(self.raw) < 28 or self.raw[:2] != b"MZ":
            raise ValueError("expected an unpacked DOS MZ executable")
        h = struct.unpack_from("<14H", self.raw)
        self.hdr = h[4] * 16
        size = (h[2] - 1) * 512 + (h[1] or 512)
        if not 28 <= self.hdr <= size <= len(self.raw):
            raise ValueError("MZ size/header extends outside the file")
        self.img = self.raw[self.hdr:size]
        self.ss, self.sp, self.ip, self.cs = h[7], h[8], h[10], h[11]
        if h[12] + h[3] * 4 > self.hdr:
            raise ValueError("relocation table extends outside header")
        self.relocs = [struct.unpack_from("<HH", self.raw, h[12] + i * 4)
                       for i in range(h[3])]
        self.reloc_at = {seg * 16 + off for off, seg in self.relocs}
        if any(p + 2 > len(self.img) for p in self.reloc_at):
            raise ValueError("relocation extends outside load image")
        self.code_end = len(self.img)
        self.decoders = {}

    def decode(self, cs, ip):
        if not (0 <= ip <= 0xffff and 0 <= cs * 16 + ip < self.code_end):
            return None
        if cs not in self.decoders:
            self.decoders[cs] = Decoder(self.img[cs * 16:cs * 16 + 0x10000], cs * 16)
        dec = self.decoders[cs]
        dec.pos = ip
        try:
            ins = dec.decode_one()
            if ins and ins.length and cs * 16 + ip + ins.length <= self.code_end:
                return ins
        except (IndexError, KeyError, ValueError, struct.error):
            pass
        return None

    def detect_code_end(self):
        """MSC startup loads its relocated DGROUP into SS before C startup.

        Match that relationship, rather than baking in LHX's current offset.
        --code-end can override this boundary for another executable revision.
        """
        regs = {}
        ip = self.ip
        for _ in range(80):
            ins = self.decode(self.cs, ip)
            if not ins:
                break
            if ins.mnemonic == "mov" and ins.op1 and ins.op2:
                if ins.op1.type == OpType.REG16 and ins.op2.type == OpType.IMM16:
                    if ins.offset + ins.length - 2 in self.reloc_at:
                        regs[repr(ins.op1)] = ins.op2.disp & 0xffff
                elif repr(ins.op1) == "ss" and repr(ins.op2) in regs:
                    end = regs[repr(ins.op2)] * 16
                    if self.cs * 16 + self.ip < end <= len(self.img):
                        return end
            ip = (ip + ins.length) & 0xffff
        return len(self.img)


def switch_arms(im, fn, ins):
    """Bounded dense CS-relative near jump tables used by MSC switches."""
    op = ins.op1
    if not (op and op.type == OpType.MEM and op.seg in ("cs", "ds", "")
            and op.base in ("bx", "si", "di") and not op.index and op.size == 2):
        return []
    seq = [fn[p] for p in sorted(fn) if ins.address - 40 <= p < ins.address]
    bound = None
    for j in seq:
        if (j.mnemonic == "cmp" and j.op2 and j.op2.type in (OpType.IMM8, OpType.IMM16)
                and j.op1 and j.op1.type == OpType.REG16):
            bound = (j.op2.disp & 0xffff) + 1
    base = ins.offset - ins.address
    start = op.disp & 0xffff
    table_base = base if op.seg == "cs" else im.code_end
    if bound is None and op.seg in ("ds", ""):
        # LHX's video dispatch selects a driver with DS:[enum*2 + table].
        # Its short table ends in zero and all arms are neighboring blocks in
        # this same routine. Recover a bounded table, not arbitrary data words.
        arms = []
        for k in range(64):
            p = table_base + ((start + k * 2) & 0xffff)
            if p + 2 > len(im.img):
                return []
            target = struct.unpack_from("<H", im.img, p)[0]
            if target == 0:
                return arms if len(arms) >= 2 and len(set(arms)) >= 2 else []
            decoded = im.decode(base // 16, target)
            if abs(target - ins.address) > 0x2000 or decoded is None or decoded.mnemonic == "db":
                return []
            arms.append(target)
        return []
    if bound is None or not 1 <= bound <= 512:
        return []
    points = [table_base + ((start + k * 2) & 0xffff) for k in range(bound)]
    if any(p + 2 > len(im.img) for p in points):
        return []
    arms = [struct.unpack_from("<H", im.img, p)[0] for p in points]
    if any(im.decode(base // 16, a) is None for a in arms):
        return []
    return arms


def discover(im, seeds, max_functions, max_body):
    entries, bodies, tables, bad = {}, {}, {}, []
    work = []

    def add(cs, ip):
        if not (0 <= cs <= 0xffff and 0 <= ip <= 0xffff):
            return
        lin = cs * 16 + ip
        if lin >= im.code_end or lin < 0:
            return
        if lin not in entries:
            if len(entries) >= max_functions:
                raise ValueError(f"function limit {max_functions} reached; inspect recovery")
            entries[lin] = cs
            work.append(lin)

    add(im.cs, im.ip)
    pending = sorted(seeds)
    while work or pending:
        if not work:
            for cs, ip in pending:
                add(cs, ip)
            pending = []
            continue
        lin = work.pop()
        cs = entries[lin]
        body, tbl = {}, {}
        todo = [lin - cs * 16]
        while todo:
            ip = todo.pop()
            if ip in body:
                continue
            ins = im.decode(cs, ip)
            if ins is None or ins.mnemonic == "db":
                bad.append({"cs": cs, "ip": ip, "function": lin, "reason": "undecodable"})
                # Keep an explicit trap at this address so an otherwise valid
                # branch is never silently dropped from the generated closure.
                ins = decode16.Instruction()
                ins.address, ins.offset = ip, cs * 16 + ip
                ins.mnemonic, ins.length, ins.raw = "lhx_bad", 0, b""
                body[ip] = ins
                continue
            body[ip] = ins
            if len(body) > max_body:
                raise ValueError(f"body {cs:04x}:{lin-cs*16:04x} exceeds {max_body} instructions")
            m, op = ins.mnemonic, ins.op1
            nxt = (ip + ins.length) & 0xffff
            if m in TERMINATORS or m == "jmp far":
                if op and op.type == OpType.FAR:
                    add(op.far_seg, op.disp)
                continue
            if m == "jmp":
                if op and op.type in (OpType.REL8, OpType.REL16):
                    todo.append(op.disp & 0xffff)
                elif op and op.type == OpType.FAR:
                    add(op.far_seg, op.disp)
                else:
                    arms = switch_arms(im, body, ins)
                    if arms:
                        tbl[ip] = arms
                        todo.extend(arms)
                continue
            if m in JCC and op:
                todo.append(op.disp & 0xffff)
            elif m == "call" and op:
                if op.type == OpType.REL16:
                    add(cs, op.disp & 0xffff)
                elif op.type == OpType.FAR:
                    add(op.far_seg, op.disp)
            # DOS termination cannot return to the following string/data.
            if m == "int" and op and op.disp in (0x20, 0x27):
                continue
            if m == "int" and op and op.disp == 0x21:
                previous = body.get(ip - 3) or body.get(ip - 2)
                if previous and previous.mnemonic == "mov" and previous.op1 and previous.op2:
                    reg, val = repr(previous.op1), previous.op2.disp
                    if previous.op2.type in (OpType.IMM8, OpType.IMM16):
                        if (reg == "ax" and val >> 8 == 0x4c) or (reg == "ah" and val == 0x4c):
                            continue
            todo.append(nxt)
        bodies[lin], tables[lin] = body, tbl
    return entries, bodies, tables, bad


def stored_pointers(im, entries, bodies):
    """Recover relocated far pointers and explicitly stored near callbacks."""
    out = set()
    segs = sorted(set(entries.values()))
    covered = {ins.offset + k for b in bodies.values() for ins in b.values()
               for k in range(ins.length)}
    called_slots = {}

    def ds_slot(op):
        if (op and op.type in (OpType.MEM, OpType.MOFFS) and op.seg in ("ds", "")
                and not op.base and not op.index and op.size == 2):
            return op.disp & 0xffff
        return None

    for lin, body in bodies.items():
        for ins in body.values():
            slot = ds_slot(ins.op1)
            if ins.mnemonic in ("call", "jmp") and slot is not None:
                called_slots.setdefault(slot, set()).add(entries[lin])

    def add_near_table(table, cs_context):
        first, run = None, []
        for k in range(64):
            p = im.code_end + (table & 0xffff) + 2 * k
            if p + 2 > len(im.img):
                break
            off = struct.unpack_from("<H", im.img, p)[0]
            # Count-indexed unrolled loops reserve slot zero for the case
            # their preceding zero-count check already excludes.
            if k == 0 and off == 0:
                continue
            if first is None:
                first = off
            decoded = im.decode(cs_context, off)
            if (not off or off == 0xffff or abs(off - first) > 0x1000
                    or decoded is None or decoded.mnemonic == "db"):
                break
            run.append(off)
        if len(run) >= 2:
            out.update((cs_context, off) for off in run)

    def valid(cs, ip, prologue=False):
        lin = cs * 16 + ip
        later = [s * 16 for s in segs if s > cs]
        end = min(im.code_end, later[0] if later else im.code_end)
        if lin >= end or lin < 0 or im.decode(cs, ip) is None:
            return False
        return not prologue or im.img[lin:lin + 5].startswith(PROLOGUES)

    for p in sorted(im.reloc_at):
        seg = struct.unpack_from("<H", im.img, p)[0]
        if p >= 2 and p not in covered and seg in segs:
            off = struct.unpack_from("<H", im.img, p - 2)[0]
            if valid(seg, off):
                out.add((seg, off))

    for lin, body in bodies.items():
        cs = entries[lin]
        seq = [body[p] for p in sorted(body)]
        called_regs = {repr(ins.op1) for ins in seq if ins.mnemonic in ("call", "jmp")
                       and ins.op1 and ins.op1.type == OpType.REG16}
        for i, ins in enumerate(seq):
            # Assembly helpers often have no frame/prologue: mov CX,offset;
            # mov BX,data; call CX. Prove the register reaches an indirect call
            # before it is overwritten, instead of demanding a C-style entry.
            if (ins.mnemonic == "mov" and ins.op1 and repr(ins.op1) in called_regs
                    and ins.op2 and ins.op2.type == OpType.IMM16):
                reg = repr(ins.op1)
                for following in seq[i + 1:i + 9]:
                    if following.mnemonic in ("call", "jmp"):
                        if following.op1 and following.op1.type == OpType.REG16 and repr(following.op1) == reg:
                            off = ins.op2.disp & 0xffff
                            if valid(cs, off):
                                out.add((cs, off))
                        break
                    if (following.mnemonic in WRITES | {"lea", "les", "lds", "lss"}
                            and following.op1 and repr(following.op1) == reg):
                        break
                    if following.mnemonic in TERMINATORS:
                        break
            # A stored far pointer can be constructed from an immediate offset
            # and CS, rather than appearing in the MZ relocation table. Timer
            # installation writes the IVT this way (ES:[20]=offset, ES:[22]=CS).
            if (ins.mnemonic == "mov" and ins.op1 and ins.op1.type in (OpType.MEM, OpType.MOFFS)
                    and not ins.op1.base and not ins.op1.index and ins.op2
                    and ins.op2.type == OpType.SREG and repr(ins.op2) == "cs"):
                for prev in reversed(seq[max(0, i - 4):i]):
                    if (prev.mnemonic == "mov" and prev.op1 and prev.op1.type in (OpType.MEM, OpType.MOFFS)
                            and not prev.op1.base and not prev.op1.index and prev.op1.seg == ins.op1.seg
                            and (prev.op1.disp + 2) & 0xffff == ins.op1.disp & 0xffff
                            and prev.op2 and prev.op2.type == OpType.IMM16):
                        off = prev.op2.disp & 0xffff
                        if valid(cs, off):
                            out.add((cs, off))
                        break
            if (ins.mnemonic == "mov" and ins.op1 and repr(ins.op1) in called_regs
                    and ins.op2 and ins.op2.type == OpType.MEM and ins.op2.seg in ("ds", "")
                    and (ins.op2.base or ins.op2.index) and ins.op2.disp and ins.op2.size == 2):
                add_near_table(ins.op2.disp, cs)
            # A renderer initializer copies one entry from a driver table into
            # a near callback slot: mov AX,DS:[BX+table]; mov DS:[slot],AX.
            # The slot's indirect callers establish CS, so a bounded cluster
            # of neighboring near routine pointers can be lifted statically.
            if (ins.mnemonic == "mov" and ins.op1 and ins.op1.type == OpType.REG16
                    and ins.op2 and ins.op2.type == OpType.MEM and ins.op2.seg in ("ds", "")
                    and (ins.op2.base or ins.op2.index) and ins.op2.disp
                    and i + 1 < len(seq)):
                dest = seq[i + 1]
                slot = ds_slot(dest.op1)
                if (dest.mnemonic == "mov" and slot in called_slots and dest.op2
                        and repr(dest.op2) == repr(ins.op1)):
                    for cs_context in called_slots[slot]:
                        add_near_table(ins.op2.disp, cs_context)
            if ins.mnemonic not in ("mov", "push"):
                continue
            ops = [op for op in (ins.op1, ins.op2) if op and op.type == OpType.IMM16]
            if not ops:
                continue
            value = ops[-1].disp & 0xffff
            if ins.offset + ins.length - 2 in im.reloc_at:
                for other in seq[max(0, i - 3):i + 4]:
                    if other is ins or other.mnemonic not in ("mov", "push"):
                        continue
                    for op in (other.op1, other.op2):
                        if op and op.type == OpType.IMM16 and valid(value, op.disp & 0xffff, True):
                            out.add((value, op.disp & 0xffff))
            elif valid(cs, value, True):
                out.add((cs, value))
    return out


def smc_targets(entries, bodies):
    out = set()
    for lin, body in bodies.items():
        cs = entries[lin]
        for ins in body.values():
            op = ins.op1
            if (ins.mnemonic in WRITES and op and op.type in (OpType.MEM, OpType.MOFFS)
                    and op.seg == "cs" and not op.base and not op.index):
                out.update(cs * 16 + ((op.disp + i) & 0xffff) for i in range(op.size or 2))
    return out


def seal_fallthrough(insts):
    out = []
    for i, ins in enumerate(insts):
        out.append(ins)
        if ins.mnemonic in TERMINATORS | {"jmp", "jmp far", "lhx_bad"}:
            continue
        nxt = (ins.address + ins.length) & 0xffff
        if i + 1 < len(insts) and insts[i + 1].address == nxt:
            continue
        j = decode16.Instruction()
        j.mnemonic, j.address, j.offset, j.length, j.raw = "jmp", -1, ins.offset, 0, b""
        j.op1 = decode16.Operand(type=OpType.REL16, disp=nxt, size=2)
        out.append(j)
    return out


def prepare_instructions(insts):
    """Copy a recovered closure and adapt repetition for the upstream lifter."""
    instructions = seal_fallthrough(copy.deepcopy(insts))
    # F2 and F3 both repeat MOVS/STOS/INS/OUTS unconditionally. The MSC
    # string library uses F2; upstream lift16's repeated-copy path only
    # recognizes its 'rep' spelling. SCAS/CMPS retain conditional repetition.
    for ins in instructions:
        if ins.prefix == "repnz" and ins.mnemonic in (
                "movsb", "movsw", "movsd", "stosb", "stosw", "stosd",
                "outsb", "outsw", "insb", "insw"):
            ins.prefix = "rep"
    return instructions


def configure_frame_unwind(lifter, instructions):
    """Interrupt handlers may temporarily pop their own interrupted frame."""
    # LHX's INT 0 handler saves IP/CS in module data before calling its near
    # saturation helpers. SP is then legitimately above the handler's entry SP.
    # Ordinary assembly helpers still need the nonlocal-return guard.
    lifter.unwind_sp = not any(ins.mnemonic == "iret" for ins in instructions)


def emit_with_fault_resume(lifter, emit, instruction, start):
    """Stop a native continuation when an INT 0 handler redirects execution."""
    before = len(lifter.output)
    emit(instruction, start)
    if instruction.mnemonic not in ("div", "idiv"):
        return
    expected = (instruction.address + instruction.length) & 0xffff
    replacements = 0
    for index in range(before, len(lifter.output)):
        lifter.output[index], count = re.subn(
            r'\brecomp_div0\(("[^"\n]*")\);',
            lambda match: f"if (recomp_div0({match[1]}, 0x{expected:04X})) return;",
            lifter.output[index])
        replacements += count
    if replacements != 1:
        raise ValueError(f"Expected one divide fault path at {instruction.address:#x}")


def emit_authored_opcode_variants(lifter, instruction, start, cs, emit):
    """Compile LHX's finite, measured line-direction instruction alternatives."""
    prefixes = LHX_OPCODE_VARIANTS.get(instruction.offset)
    if prefixes is None:
        return False
    width = len(prefixes[0])
    if not instruction.raw.startswith(prefixes[0]) or width not in (1, 2):
        raise ValueError(f"Unexpected authored opcode at image {instruction.offset:#x}")
    variants = []
    for prefix in prefixes:
        if len(prefix) != width:
            raise ValueError("Authored opcode alternatives must have equal widths")
        decoder = decode16.Decoder(prefix + instruction.raw[width:])
        variant = decoder.decode_one()
        if variant is None or variant.length != instruction.length:
            raise ValueError(f"Invalid authored opcode alternative at {instruction.offset:#x}")
        variant.address, variant.offset = instruction.address, instruction.offset
        for source, target in zip((instruction.op1, instruction.op2, instruction.op3),
                                  (variant.op1, variant.op2, variant.op3)):
            if source is not None and target is not None and hasattr(source, "lhx_disp"):
                target.lhx_disp = source.lhx_disp
        variants.append((int.from_bytes(prefix, "little"), variant))

    lifter._emit_label(instruction.address)
    lifter._emit(f"switch (mem_read{width * 8}(cpu, SEG_{cs:04X}, "
                 f"0x{instruction.address:04X})) {{", "authored line direction")
    lifter.indent += 1
    for value, variant in variants:
        lifter._emit(f"case 0x{value:0{width * 2}X}: {{")
        lifter.indent += 1
        emit(variant, start)
        lifter._emit("break;")
        lifter.indent -= 1
        lifter._emit("}")
    reason = json.dumps("unsupported authored line opcode")
    lifter._emit(f"default: recomp_unhandled(cpu, SEG_{cs:04X}, "
                 f"0x{instruction.address:04X}, {reason}); return;")
    lifter.indent -= 1
    lifter._emit("}")
    return True


def read_seeds(args):
    values = list(args.seed)
    if args.seeds and args.seeds.exists():
        for line in args.seeds.read_text().splitlines():
            match = re.search(r"\b([0-9a-fA-F]{4}):([0-9a-fA-F]{4})\b", line)
            if match:
                values.append(match.group(0))
    out = set()
    for value in values:
        seg, off = (int(p, 16) for p in value.split(":"))
        # Runtime miss logs contain relocated CS values.
        if args.relative_seeds:
            out.add((seg, off))
        elif seg >= LOAD_SEG:
            out.add((seg - LOAD_SEG, off))
        else:
            raise ValueError(f"seed {value} precedes load segment; use --relative-seeds")
    return out


def read_profile(path):
    out = set()
    for line_number, line in enumerate(path.read_text().splitlines(), 1):
        value = line.partition("#")[0].strip()
        if not value:
            continue
        if not re.fullmatch(r"[0-9a-fA-F]{4}:[0-9a-fA-F]{4}", value):
            raise ValueError(f"invalid relative CS:IP in {path}:{line_number}")
        out.add(tuple(int(part, 16) for part in value.split(":")))
    return out


def write_changed(path, data):
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def emit_native_instruction_observer(lifter, ins):
    observer = LHX_NATIVE_INSTRUCTION_OBSERVERS.get(ins.offset)
    if observer is None:
        return
    name, expected = observer
    if ins.raw != expected:
        raise ValueError(f"native instruction observer {name} bytes differ at {ins.offset:05x}")
    lifter._emit_label(ins.address)
    lifter._emit(f"{name}(cpu);")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=Path("bin/dos/LHX.unpacked.EXE"))
    parser.add_argument("--output", type=Path, default=Path("build/generated"))
    parser.add_argument("--pcrecomp", type=Path,
                        default=Path(os.environ.get("PCRECOMP_HOME", Path(__file__).resolve().parents[1] / "third_party/pcrecomp")))
    parser.add_argument("--seed", action="append", default=[], help="extra runtime CS:IP (hex)")
    parser.add_argument("--seeds", type=Path, help="previous runtime dispatch miss log")
    parser.add_argument("--profile", type=Path, default=Path(__file__).with_name("lhx.seeds"),
                        help="version-guarded image-relative callback profile")
    parser.add_argument("--relative-seeds", action="store_true", help="seeds have image-relative segments")
    parser.add_argument("--code-end", type=lambda x: int(x, 0), help="exclusive image offset of code")
    parser.add_argument("--chunk-size", type=int, default=80)
    parser.add_argument("--max-functions", type=int, default=12000)
    parser.add_argument("--max-body", type=int, default=20000)
    args = parser.parse_args()
    for name in ("disasm", "lift"):
        sys.path.insert(0, str(args.pcrecomp / "tools" / name))
    global Decoder, OpType, decode16
    import decode16
    import lift16
    from decode16 import Decoder, OpType
    decode16.WRAP_NEAR_TARGETS = True
    decode16.EMU87_INTS = False

    im = Image(args.exe)
    image_sha256 = hashlib.sha256(im.img).hexdigest()
    if image_sha256 != LHX_IMAGE_SHA256:
        raise ValueError("unsupported LHX release: native bridge addresses require image SHA256 "
                         f"{LHX_IMAGE_SHA256}, got {image_sha256}")
    hooks = LHX_NATIVE_HOOKS
    im.code_end = args.code_end or im.detect_code_end()
    profile_seeds = read_profile(args.profile)
    seeds = read_seeds(args) | profile_seeds
    recovered = set()
    for iteration in range(12):
        entries, bodies, tables, bad = discover(im, seeds | recovered, args.max_functions, args.max_body)
        more = stored_pointers(im, entries, bodies) - recovered
        if not more:
            break
        recovered |= more
    else:
        raise ValueError("stored-pointer recovery did not converge in 12 passes")
    names = {lin: f"fn_{lin:05X}" for lin in entries}
    smc = smc_targets(entries, bodies)
    missing, unsupported = {}, []
    lifter = lift16.Lifter(known_funcs=names, reloc_words=im.reloc_at, load_seg=LOAD_SEG)
    lifter.far_base, lifter.dispatch, lifter.iret_frame = 0, True, True
    lifter.smc_imm, lifter.x87 = smc, True
    # Assembly near helpers can branch into their caller's far-return epilogue.
    # Once that guest frame is gone, its suspended C continuation must unwind
    # too; executing it would pop the caller's caller and corrupt the stack.
    lifter.unwind_sp = True
    lift16.DIV0_FN = "recomp_div0"

    def tail_jump(target):
        if target in names:
            lifter.func_calls.add(names[target])
            return f"{names[target]}(cpu); return;"
        return (f"recomp_dispatch(cpu, 0x{LOAD_SEG + (target >> 4):04X}, "
                f"0x{target & 15:04X}); return;")

    lifter._tail_jump = tail_jump

    # The hand-written renderer also changes ModR/M displacements. Read those
    # displacements as data; the instruction's operation is still compiled C.
    original_mem_addr = lift16._mem_addr

    def mem_addr(op):
        dynamic = getattr(op, "lhx_disp", None)
        if dynamic is None:
            return original_mem_addr(op)
        seg = f"SEG_{current_cs[0]:04X}" if op.seg == "cs" else f"cpu->{op.seg or 'ds'}"
        disp_ip, width = dynamic
        disp = f"mem_read{width * 8}(cpu, SEG_{current_cs[0]:04X}, 0x{disp_ip:04X})"
        if width == 1:
            disp = f"(int8_t){disp}"
        parts = [f"cpu->{name}" for name in (op.base, op.index) if name]
        return seg, f"(uint16_t)({' + '.join(parts + [disp])})"

    lift16._mem_addr = mem_addr

    def modified_displacement(ins):
        raw = ins.raw
        at = 0
        while at < len(raw) and raw[at] in (0x26, 0x2e, 0x36, 0x3e, 0x64, 0x65, 0x66, 0x67, 0xf2, 0xf3):
            at += 1
        if at >= len(raw):
            return
        opcode = raw[at]
        at += 1
        if opcode == 0x0f:
            at += 1
        if opcode in (0xa0, 0xa1, 0xa2, 0xa3):
            width = 2
        elif at < len(raw):
            modrm = raw[at]
            at += 1
            mod, rm = modrm >> 6, modrm & 7
            width = 1 if mod == 1 else 2 if mod == 2 or (mod == 0 and rm == 6) else 0
        else:
            return
        if width and any(ins.offset + at + i in smc for i in range(width)):
            for op in (ins.op1, ins.op2, ins.op3):
                if op and op.type in (OpType.MEM, OpType.MOFFS):
                    op.lhx_disp = ((ins.address + at) & 0xffff, width)

    # Turn every unsupported instruction into a runtime failure at its actual
    # address; missing handling must never become a silently skipped statement.
    original_lift = lifter.lift_instruction
    current_cs = [0]

    def lift_instruction(ins, start):
        before = len(lifter.output)
        modified_displacement(ins)
        emit_native_instruction_observer(lifter, ins)
        if ins.offset in LHX_SCENE_EVENTS:
            lifter._emit_label(ins.address)
            lifter._emit(f"lhx_scene_event(cpu, 0x{ins.offset:05X});")
        if ins.offset == LHX_SCENE_CACHE_CHECK:
            target = LHX_SCENE_CACHE_MISS - current_cs[0] * 16
            if target not in lifter.valid_addrs:
                raise ValueError("Native scene cache-miss path missing from function closure")
            lifter.labels_needed.add(target)
            lifter._emit_label(ins.address)
            lifter._emit(f"if (g_lhx_scene_enabled) goto {lift16._label(target, lifter.func_name)};")
        if emit_authored_opcode_variants(lifter, ins, start, current_cs[0], original_lift):
            return
        op = ins.op1
        if (op and op.type == OpType.FAR and ins.mnemonic in ("call", "call far", "jmp", "jmp far")
                and any(p in smc for p in range(ins.offset + ins.length - 4, ins.offset + ins.length))):
            # Interrupt chaining patches an EA offset:segment instruction with
            # the saved BIOS vector. Both words are data, not compiled literals.
            lifter._emit_label(ins.address)
            off_ip = (ins.address + ins.length - 4) & 0xffff
            seg_ip = (ins.address + ins.length - 2) & 0xffff
            if ins.mnemonic.startswith("call"):
                lifter._emit(f"push16(cpu, SEG_{current_cs[0]:04X}); push16(cpu, 0xFFFF);")
                dispatch_name, tail = "dispatch_far", ""
            else:
                dispatch_name, tail = "recomp_dispatch", " return;"
            lifter._emit(f"{dispatch_name}(cpu, mem_read16(cpu, SEG_{current_cs[0]:04X}, 0x{seg_ip:04X}), "
                         f"mem_read16(cpu, SEG_{current_cs[0]:04X}, 0x{off_ip:04X}));{tail}", repr(ins))
            return
        if ins.mnemonic == "jmp far" and op and op.type == OpType.FAR:
            lifter._emit_label(ins.address)
            lifter._emit(f"recomp_dispatch(cpu, 0x{(op.far_seg+LOAD_SEG)&0xffff:04X}, 0x{op.disp:04X}); return;", repr(ins))
            return
        if (ins.mnemonic in ("call", "jmp") and op and op.type == OpType.REL16
                and any(p in smc for p in range(ins.offset + ins.length - 2, ins.offset + ins.length))):
            lifter._emit_label(ins.address)
            disp_ip = (ins.address + ins.length - 2) & 0xffff
            target = (f"(uint16_t)(0x{(ins.address + ins.length) & 0xffff:04X} + "
                      f"mem_read16(cpu, SEG_{current_cs[0]:04X}, 0x{disp_ip:04X}))")
            if ins.mnemonic == "call":
                lifter._emit(f"push16(cpu, 0xFFFF); dispatch_near(cpu, SEG_{current_cs[0]:04X}, {target});", repr(ins))
            else:
                lifter._emit(f"recomp_dispatch(cpu, SEG_{current_cs[0]:04X}, {target}); return;", repr(ins))
            return
        if ins.mnemonic == "int":
            lifter._emit(f"cpu->ip = 0x{ins.address:04X};")
        try:
            emit_with_fault_resume(lifter, original_lift, ins, start)
        except (ValueError, IndexError, AttributeError) as error:
            del lifter.output[before:]
            lifter._emit_label(ins.address)
            lifter._emit("/* UNHANDLED: " + str(error).replace("*/", "") + " */")
        lines = lifter.output[before:]
        if any("UNHANDLED" in line or "needs dispatch" in line or "/* ??? */" in line for line in lines):
            # Keep labels and original assembly comment for diagnostics.
            del lifter.output[before:]
            if ins.address in lifter.labels_needed:
                lifter.output.append(lift16._label(ins.address, lifter.func_name) + ":;")
            reason = f"{ins.mnemonic} at image {ins.offset:05x}"
            lifter._emit(f"recomp_unhandled(cpu, SEG_{current_cs[0]:04X}, 0x{ins.address & 0xffff:04X}, {json.dumps(reason)}); return;")
            unsupported.append({"cs": current_cs[0], "ip": ins.address, "reason": reason})

    lifter.lift_instruction = lift_instruction
    args.output.mkdir(parents=True, exist_ok=True)
    order, files, calls = sorted(entries), {}, set()
    for ci in range(0, len(order), args.chunk_size):
        text = ['/* Generated from the supplied game; do not commit. */',
                '#include "lifted.h"', '#include "scene_filter.h"', ""]
        for lin in order[ci:ci + args.chunk_size]:
            cs = entries[lin]
            current_cs[0] = cs
            lift16._CODE_SEG = f"{cs:04X}"
            lifter.jump_tables = {ip: [cs * 16 + off for off in arms] for ip, arms in tables[lin].items()}
            instructions = prepare_instructions([bodies[lin][p] for p in sorted(bodies[lin])])
            configure_frame_unwind(lifter, instructions)
            if lin in hooks:
                code = f"void {names[lin]}_body(CPU *cpu) {{ {hooks[lin]}(cpu); }}"
            else:
                code = lifter.lift_function(names[lin] + "_body", instructions, cs * 16, entry_addr=lin - cs * 16)
                calls.update(lifter.func_calls)
            wrapper = (f"void {names[lin]}(CPU *cpu) {{ uint16_t _cs=cpu->cs, _ip=cpu->ip; "
                       f"cpu->cs=SEG_{cs:04X}; cpu->ip=0x{lin-cs*16:04X}; "
                       + (f"unsigned _model_token={LHX_NATIVE_OBSERVERS[lin][0]}(cpu); "
                          if lin in LHX_NATIVE_OBSERVERS else "")
                       + (f"{LHX_NATIVE_PRE_OBSERVERS[lin]}(cpu); "
                          if lin in LHX_NATIVE_PRE_OBSERVERS else "")
                       + f"{names[lin]}_body(cpu); "
                       + (f"{LHX_NATIVE_OBSERVERS[lin][1]}(cpu,_model_token); "
                          if lin in LHX_NATIVE_OBSERVERS else "")
                       + (f"{LHX_NATIVE_POST_OBSERVERS[lin]}; "
                          if lin in LHX_NATIVE_POST_OBSERVERS else "")
                       + "cpu->cs=_cs; cpu->ip=_ip; }")
            text.extend([code, wrapper, ""])
        files[f"recomp_{ci // args.chunk_size:03d}.c"] = "\n".join(text)

    # Direct calls which could not be recovered must retain observable behavior:
    # ask the dispatcher, whose missing-address path is fatal and records seeds.
    strays = sorted(calls - set(names.values()))
    header = ["/* Generated; do not commit. */", "#pragma once"]
    for cs in sorted(set(entries.values())):
        header.append(f"#define SEG_{cs:04X} 0x{(cs + LOAD_SEG) & 0xffff:04X}")
    header += [f"void {name}(CPU *cpu);" for name in names.values()]
    header += [f"void {name}(CPU *cpu);" for name in strays]
    files["recomp_all.h"] = "\n".join(header) + "\n"
    dispatch = ['/* Generated; do not commit. */', '#include "lifted.h"',
                f"const uint16_t g_load_seg=0x{LOAD_SEG:04X}, g_psp_seg=0x{PSP_SEG:04X};",
                f"const uint16_t g_entry_cs=0x{im.cs:04X}, g_entry_ip=0x{im.ip:04X};",
                f"const uint16_t g_entry_ss=0x{im.ss:04X}, g_entry_sp=0x{im.sp:04X};",
                f"const unsigned g_func_count={len(order)};", "const RecompFunc g_funcs[] = {"]
    dispatch += [f"  {{0x{lin:05X}, {names[lin]}}}," for lin in order]
    dispatch += ["};", ""]
    for name in strays:
        far = re.fullmatch(r"far_([0-9A-F]{4})_([0-9A-F]{4})", name)
        near = re.fullmatch(r"res_([0-9A-F]+)", name)
        if far:
            seg, off = (int(x, 16) for x in far.groups())
            dispatch.append(f"void {name}(CPU *cpu) {{ recomp_dispatch(cpu, 0x{(seg+LOAD_SEG)&0xffff:04X}, 0x{off:04X}); }}")
        elif near:
            lin = int(near.group(1), 16)
            dispatch.append(f"void {name}(CPU *cpu) {{ recomp_dispatch(cpu, 0x{LOAD_SEG+(lin>>4):04X}, 0x{lin&15:04X}); }}")
        else:
            raise ValueError(f"unrecognized unresolved function {name}")
    files["recomp_dispatch.c"] = "\n".join(dispatch) + "\n"
    image = ['/* Generated from supplied game; do not commit. */', "#include <stdint.h>",
             f"const unsigned g_image_len={len(im.img)}, g_nrelocs={len(im.relocs)};",
             "const uint16_t g_relocs[][2] = {"]
    image += [f"  {{0x{off:04X},0x{seg:04X}}}," for off, seg in im.relocs]
    image += ["};", "const uint8_t g_image[] = {"]
    image += [",".join(str(b) for b in im.img[i:i + 32]) + "," for i in range(0, len(im.img), 32)]
    image += ["};", ""]
    files["image.c"] = "\n".join(image)
    metadata = {"exe_sha256": hashlib.sha256(im.raw).hexdigest(),
                "image_sha256": hashlib.sha256(im.img).hexdigest(), "image_bytes": len(im.img),
                "code_end": im.code_end, "load_seg": LOAD_SEG, "psp_seg": PSP_SEG,
                "entry_cs": im.cs, "entry_ip": im.ip, "entry_ss": im.ss, "entry_sp": im.sp,
                "functions": len(order), "instructions": sum(map(len, bodies.values())),
                "code_segments": sorted(set(entries.values())), "recovered_pointers": len(recovered),
                "undecodable": bad, "unsupported": unsupported, "unresolved_calls": strays,
                "self_modified_bytes": sorted(smc),
                "authored_opcode_variants": {
                    f"{address:05x}": [prefix.hex() for prefix in prefixes]
                    for address, prefixes in LHX_OPCODE_VARIANTS.items()},
                "native_hooks": {f"{a:05x}": name for a, name in hooks.items()},
                "native_observers": {f"{a:05x}": names for a, names in LHX_NATIVE_OBSERVERS.items()},
                "native_pre_observers": {f"{a:05x}": name for a, name in LHX_NATIVE_PRE_OBSERVERS.items()},
                "native_instruction_observers": {
                    f"{a:05x}": {"name": name, "bytes": raw.hex()}
                    for a, (name, raw) in LHX_NATIVE_INSTRUCTION_OBSERVERS.items()},
                "native_post_observers": {f"{a:05x}": name for a, name in LHX_NATIVE_POST_OBSERVERS.items()},
                "native_scene_instruction_events": [f"{a:05x}" for a in sorted(LHX_SCENE_EVENTS)],
                "native_scene_cache_bypass": {"check": f"{LHX_SCENE_CACHE_CHECK:05x}",
                                              "miss": f"{LHX_SCENE_CACHE_MISS:05x}"},
                "profile_seeds": [f"{cs:04x}:{ip:04x}" for cs, ip in sorted(profile_seeds)],
                "function_addresses": {f"{lin:05x}": f"{entries[lin]:04x}:{lin-entries[lin]*16:04x}" for lin in order},
                "limitations": ["Static discovery cannot prove complete indirect-call coverage.",
                                "Self-modified operands and authored graphics opcode alternatives are supported; general opcode rewriting is not.",
                                "Runtime hardware and DOS shims require gameplay validation."]}
    files["lift.json"] = json.dumps(metadata, indent=2) + "\n"
    for name, content in files.items():
        write_changed(args.output / name, content)
    # Remove only this generator's obsolete chunks after output is ready.
    for old in args.output.glob("recomp_*.c"):
        if old.name not in files:
            old.unlink()
    print(f"{len(order)} functions, {metadata['instructions']} instructions, "
          f"{len(bad)} undecodable, {len(unsupported)} unsupported, {len(strays)} unresolved calls")
    print(f"Code boundary {im.code_end:#x}; image {len(im.img):#x}; output {args.output}")


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError) as error:
        sys.exit(f"lift: {error}")
