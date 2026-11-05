#!/usr/bin/env python3
"""Ahead-of-time compile the supplied CD's PC speaker driver into native C.

The driver is extracted and statically decoded at build time. Runtime dispatch
only selects compiled functions; it contains no instruction decoder or fallback.
"""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
from pathlib import Path
import struct
import sys

from assets import members

ROOT = Path(__file__).resolve().parents[1]
DRIVER_SHA256 = "17dc7ea8077160c658397883364317347534da6e49050776c32b0c3e6eea3688"
PREFIX_BYTES = 0x171  # Immutable code/table prefix, before mutable voice records.
TERMINATORS = {"ret", "retf", "iret", "hlt", "jmp far"}
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns",
       "jp", "jnp", "jl", "jge", "jle", "jg", "loop", "loopz", "loopnz", "jcxz"}


def driver_payload(data_dir: Path) -> bytes:
    entries = members((data_dir / "LHX2.LIB").read_bytes())
    selected = [entry for entry in entries if entry.name.upper() == "IBMDRIVE.DRV"]
    if len(selected) != 1:
        raise ValueError("Expected one IBMDRIVE.DRV member in LHX2.LIB")
    driver = selected[0].decode()
    digest = hashlib.sha256(driver).hexdigest()
    if digest != DRIVER_SHA256:
        raise ValueError(f"Unsupported IBMDRIVE.DRV SHA256: {digest}")
    size, header_bytes = struct.unpack_from("<HH", driver)
    if size != len(driver) or header_bytes != 16 or any(driver[4:16]):
        raise ValueError("Unexpected IBMDRIVE.DRV header")
    return driver[header_bytes:]


def discover(payload: bytes, Decoder, OpType):
    """Follow each function's branch closure, separately discovering callees.

    The indirect API table and one control callback are measured from this
    SHA-256-gated driver. The IRQ vector is installed by its init function.
    """
    api = struct.unpack_from("<13H", payload, 0x1E)
    functions = {0, 0x363, 0x167, *api}
    pending = list(functions)
    bodies = {}
    all_instructions = {}

    def decode(ip):
        if not 0 <= ip < len(payload):
            raise ValueError(f"Audio branch outside driver payload at {ip:#x}")
        decoder = Decoder(payload, 0)
        decoder.pos = ip
        instruction = decoder.decode_one()
        if (instruction is None or not instruction.length
                or instruction.offset + instruction.length > len(payload)
                or instruction.mnemonic == "db"):
            raise ValueError(f"Unsupported/truncated audio instruction at {ip:#x}")
        return instruction

    while pending:
        entry = pending.pop()
        if entry in bodies:
            continue
        work, body = [entry], {}
        while work:
            ip = work.pop()
            if ip in body:
                continue
            ins = decode(ip)
            body[ip] = ins
            all_instructions[ip] = ins
            op, mnemonic = ins.op1, ins.mnemonic
            if mnemonic == "call" and op and op.type == OpType.REL16:
                target = op.disp & 0xFFFF
                if target not in functions:
                    functions.add(target)
                    pending.append(target)
            elif mnemonic == "call" and op and op.type == OpType.FAR:
                raise ValueError("Unexpected direct far call in position-independent driver")
            if mnemonic == "jmp":
                if not op or op.type not in (OpType.REL8, OpType.REL16):
                    raise ValueError(f"Unexpected computed near audio jump at {ip:#x}")
                work.append(op.disp & 0xFFFF)
                continue
            if mnemonic in JCC:
                if not op or op.type not in (OpType.REL8, OpType.REL16):
                    raise ValueError(f"Invalid audio branch at {ip:#x}")
                work.append(op.disp & 0xFFFF)
            if mnemonic not in TERMINATORS:
                work.append((ip + ins.length) & 0xFFFF)
            if len(body) > 4096:
                raise ValueError("Audio function exceeded bounded discovery")
        bodies[entry] = body
    if functions != bodies.keys():
        raise ValueError("Audio function discovery did not close")
    return bodies, all_instructions, api


def write_if_changed(path: Path, data: str) -> None:
    if not path.exists() or path.read_text() != data:
        path.write_text(data)


def generate(data_dir: Path, output: Path, pcrecomp: Path) -> dict:
    for name in ("disasm", "lift"):
        sys.path.insert(0, str(pcrecomp / "tools" / name))
    import decode16
    import lift16
    from decode16 import Decoder, OpType
    decode16.WRAP_NEAR_TARGETS = True
    decode16.EMU87_INTS = False
    payload = driver_payload(data_dir)
    bodies, instructions, api = discover(payload, Decoder, OpType)
    names = {offset: f"audio_ibm_fn_{offset:04X}" for offset in sorted(bodies)}
    lifter = lift16.Lifter(known_funcs=names)
    lifter.dispatch, lifter.iret_frame, lifter.far_base = True, True, 0
    lift16._CODE_SEG = None  # The driver is allocated dynamically, unlike LHX.EXE.

    def tail_jump(target):
        if target not in names:
            raise ValueError(f"Unrecovered audio tail target {target:#x}")
        lifter.func_calls.add(names[target])
        return f"{names[target]}(cpu); return;"

    lifter._tail_jump = tail_jump
    declarations = [f"static void {name}(CPU *);" for name in names.values()]
    code = ['/* Generated from the owner-supplied IBMDRIVE.DRV; do not commit. */',
            '#include "audio_ibm_gen.h"', *declarations, ""]
    for entry, body in sorted(bodies.items()):
        ordered = [copy.deepcopy(body[ip]) for ip in sorted(body)]
        body_name = names[entry].replace("_fn_", "_code_")
        text = lifter.lift_function(body_name, ordered, 0, entry_addr=entry)
        bad_markers = ("UNHANDLED", "needs dispatch", "/* ???", "write ???")
        if any(marker in text for marker in bad_markers):
            raise ValueError(f"Unsupported emitted audio code in function {entry:#x}")
        if unknown := lifter.func_calls - set(names.values()):
            raise ValueError(f"Unresolved native audio calls: {sorted(unknown)}")
        code.append(text)
        code.append(f"static void {names[entry]}(CPU *cpu) {{\n"
                    "    uint16_t old_ip = cpu->ip;\n"
                    f"    cpu->ip = 0x{entry:04X};\n"
                    f"    {body_name}(cpu);\n"
                    "    cpu->ip = old_ip;\n}")
    code += ["const RecompFunc g_audio_ibm_funcs[] = {",
             *(f"    {{0x{entry:04X}, {name}}}," for entry, name in names.items()),
             "};", f"const unsigned g_audio_ibm_func_count = {len(names)};", ""]
    header = '\n'.join([
        "#pragma once", '#include "lifted.h"',
        "extern const RecompFunc g_audio_ibm_funcs[];",
        "extern const unsigned g_audio_ibm_func_count;",
        "extern const unsigned g_audio_ibm_prefix_bytes, g_audio_ibm_payload_bytes;",
        "extern const uint8_t g_audio_ibm_prefix[];", ""])
    prefix = [f"    0x{value:02X}," for value in payload[:PREFIX_BYTES]]
    identity = '\n'.join([
        "/* Immutable module identification only; never executed or decoded. */",
        '#include "audio_ibm_gen.h"', "const uint8_t g_audio_ibm_prefix[] = {",
        *prefix, "};", f"const unsigned g_audio_ibm_prefix_bytes = {PREFIX_BYTES};",
        f"const unsigned g_audio_ibm_payload_bytes = {len(payload)};", ""])
    report = {
        "driver": "IBMDRIVE.DRV", "driver_sha256": DRIVER_SHA256,
        "payload_sha256": hashlib.sha256(payload).hexdigest(),
        "payload_bytes": len(payload), "header_bytes": 16,
        "runtime_segment": "dynamic CPU.cs; no fixed load segment or relocations",
        "runtime_prefix_bytes": PREFIX_BYTES,
        "function_offsets": list(names), "instruction_count": len(instructions),
        "decoded_code_bytes": sum(ins.length for ins in instructions.values()),
        "api_commands": {f"0x{2 * i:02x}": offset for i, offset in enumerate(api)},
        "irq8_offset": 0x363, "unhandled_instructions": [],
        "limitations": ["Only the exact supplied PC speaker driver is supported.",
                        "Native execution still requires PIT/speaker output and timer IRQ delivery.",
                        "The original voice/envelope and song bytes remain data in owner files."],
    }
    output.mkdir(parents=True, exist_ok=True)
    write_if_changed(output / "audio_ibm_gen.h", header)
    write_if_changed(output / "audio_ibm_code.c", '\n'.join(code))
    write_if_changed(output / "audio_ibm_identity.c", identity)
    write_if_changed(output / "audio_lift.json", json.dumps(report, indent=2) + "\n")
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "bin/dos")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pcrecomp", type=Path, default=ROOT / "third_party/pcrecomp")
    args = parser.parse_args()
    report = generate(args.data_dir, args.output, args.pcrecomp)
    print(f"Lifted PC speaker driver: {len(report['function_offsets'])} native functions, "
          f"{report['instruction_count']} instructions")


if __name__ == "__main__":
    main()
