#!/usr/bin/env python3
"""Compile all supplied scenario callback modules ahead of time.

The loader recognizes extension subtypes before an FF00, 16-bit length and
native callback payload. The subtype differs among scenarios; use the actual
payload boundaries and four exports instead of a guessed subtype byte.
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
ARCHIVE_SHA256 = "ee42163b40811a293762d13f785a78c2f5a2ab1d4c67c6b65b41d5db3df4290f"
KNOWN = {name: {"offset": offset, "size": size} for name, offset, size in (
    ("PILOT.S", 0xfb, 3841), ("ESCORT.S", 0x2a6, 3381),
    ("INTERCEP.S", 0x301, 2838), ("CHEM.S", 0x23f, 1360),
    ("PALACE.S", 0x183, 1328), ("EVAC.S", 0x24b, 3551),
    ("SUPPLY.S", 0x147, 2093), ("SUPPLY2.S", 0xc7, 1182),
    ("DEMO.S", 0x144, 187), ("CONVOY.S", 0x203, 1384),
    ("FREE.S", 0x73, 639), ("CONVOY1.S", 0x2da, 1559),
    ("STRIKE1.S", 0x2ea, 2270), ("POW0.S", 0x1b3, 1422),
    ("POW1.S", 0x196, 1459), ("POW2.S", 0x1af, 1368),
    ("CHEM1.S", 0x285, 1223), ("B2.S", 0x1f1, 1423),
    ("PICKUP.S", 0x21a, 1069), ("STRIKEA.S", 0x421, 4100),
    ("STRIKEB.S", 0x41b, 4077), ("STRIKEC.S", 0x41f, 3991),
)}
JCC = {"jo", "jno", "jb", "jae", "je", "jne", "jbe", "ja", "js", "jns",
       "jp", "jnp", "jl", "jge", "jle", "jg", "loop", "loopz", "loopnz", "jcxz"}
WRITES = {"mov", "add", "adc", "sub", "sbb", "inc", "dec", "and", "or", "xor",
          "not", "neg", "pop", "xchg", "shl", "shr", "sar", "rol", "ror"}


def module_blocks(data: bytes) -> list[tuple[int, int]]:
    """Find blocks copied by main image 0874a, excluding the final script FF."""
    blocks = []
    if not data or data[-1] != 0xff:
        return blocks
    for marker in range(len(data) - 14):
        if data[marker:marker + 2] != b"\xff\0":
            continue
        size = struct.unpack_from("<H", data, marker + 2)[0]
        start = marker + 4
        if size < 10 or start + size != len(data) - 1:
            continue
        entries = struct.unpack_from("<4H", data, start)
        if all(10 <= entry <= size - 3 and data[start + entry:start + entry + 3] == b"\x55\x8b\xec"
               for entry in entries):
            blocks.append((start, size))
    return blocks


def mission_payloads(data_dir: Path):
    archive = (data_dir / "LHX3.LIB").read_bytes()
    if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Unsupported LHX3.LIB SHA256 for native mission callbacks")
    native = {}
    for member in members(archive):
        if not member.name.upper().endswith(".S"):
            continue
        name, scenario = member.name.upper(), member.decode()
        if name not in KNOWN:
            raise ValueError(f"Unsupported scenario: {name}")
        expected = KNOWN[name]
        if module_blocks(scenario) != [(expected["offset"], expected["size"])]:
            raise ValueError(f"Unexpected native callback layout in {name}")
        offset, size = expected["offset"], expected["size"]
        native[name] = scenario[offset:offset + size]
    if native.keys() != KNOWN.keys():
        raise ValueError("Missing supported native scenario callback modules")
    return native, len(native)


def discover(payload, Decoder, OpType):
    """Recover each export's closure and bounded local switch table."""
    exports = struct.unpack_from("<4H", payload)
    bodies, jump_tables, all_instructions = {}, {}, {}
    for entry in exports:
        work, body, tables = [entry], {}, {}
        while work:
            ip = work.pop()
            if ip in body:
                continue
            if not 0 <= ip < len(payload):
                raise ValueError(f"Mission branch outside module at {ip:#x}")
            decoder = Decoder(payload, 0)
            decoder.pos = ip
            ins = decoder.decode_one()
            if ins is None or not ins.length or ins.mnemonic == "db" or ip + ins.length > len(payload):
                raise ValueError(f"Undecodable mission instruction at {ip:#x}")
            body[ip] = all_instructions[ip] = ins
            mnemonic, op = ins.mnemonic, ins.op1
            if mnemonic in ("call", "call far", "int", "hlt", "iret", "jmp far"):
                raise ValueError(f"Unexpected mission callback control flow at {ip:#x}")
            if mnemonic == "jmp":
                if op and op.type in (OpType.REL8, OpType.REL16):
                    work.append(op.disp & 0xffff)
                elif (op and op.type == OpType.MEM and op.seg == "cs" and op.base == "bx"
                      and not op.index and op.size == 2 and 0 <= op.disp < entry
                      and (entry - op.disp) % 2 == 0 and 1 <= (entry - op.disp) // 2 <= 64):
                    # This pinned MSC build puts its dense near jump table
                    # immediately before the callback's export prologue.
                    count = (entry - op.disp) // 2
                    arms = list(struct.unpack_from(f"<{count}H", payload, op.disp))
                    if not all(entry < target < len(payload) for target in arms):
                        raise ValueError(f"Invalid mission switch targets at {ip:#x}")
                    tables[ip] = {"offset": op.disp, "targets": arms}
                    work.extend(arms)
                else:
                    raise ValueError(f"Unrecovered mission indirect branch at {ip:#x}")
                continue
            if mnemonic in JCC:
                if not op or op.type not in (OpType.REL8, OpType.REL16):
                    raise ValueError(f"Invalid mission conditional branch at {ip:#x}")
                work.append(op.disp & 0xffff)
            if mnemonic not in ("ret", "retf"):
                work.append((ip + ins.length) & 0xffff)
            if len(body) > 4096:
                raise ValueError("Unbounded native mission callback")
        bodies[entry], jump_tables[entry] = body, tables
    return exports, bodies, jump_tables, all_instructions


def identity_ranges(instructions, jump_tables, OpType):
    immutable, mutable = set(range(8)), set()
    for ip, ins in instructions.items():
        immutable.update(range(ip, ip + ins.length))
        op = ins.op1
        if (ins.mnemonic in WRITES and op and op.type in (OpType.MEM, OpType.MOFFS)
                and op.seg in ("", "ds", "cs") and not op.base and not op.index):
            mutable.update((op.disp + i) & 0xffff for i in range(op.size))
    for tables in jump_tables.values():
        for table in tables.values():
            immutable.update(range(table["offset"], table["offset"] + len(table["targets"]) * 2))
    if overlap := immutable & mutable:
        raise ValueError(f"Mission callback rewrites code/table bytes: {sorted(overlap)}")
    ranges = []
    for offset in sorted(immutable):
        if ranges and ranges[-1][0] + ranges[-1][1] == offset:
            ranges[-1][1] += 1
        else:
            ranges.append([offset, 1])
    return ranges, sorted(mutable)


def write_if_changed(path: Path, text: str) -> None:
    if not path.exists() or path.read_text() != text:
        path.write_text(text)


def generate(data_dir: Path, output: Path, pcrecomp: Path) -> dict:
    for name in ("disasm", "lift"):
        sys.path.insert(0, str(pcrecomp / "tools" / name))
    import decode16
    import lift16
    from decode16 import Decoder, OpType
    decode16.WRAP_NEAR_TARGETS, decode16.EMU87_INTS = True, False
    lift16._CODE_SEG = None
    payloads, scenario_count = mission_payloads(data_dir)
    files, records, header = {}, [], ['#pragma once', '#include "mission_module.h"']
    for name, payload in sorted(payloads.items()):
        stem = Path(name).stem.lower()
        exports, bodies, tables, instructions = discover(payload, Decoder, OpType)
        ranges, mutable = identity_ranges(instructions, tables, OpType)
        names = {entry: f"mission_{stem}_fn_{entry:04X}" for entry in exports}
        lifter = lift16.Lifter(known_funcs=names)
        lifter.dispatch, lifter.iret_frame = True, True
        source = ['/* Generated from the owner-supplied mission; do not commit. */',
                  '#include "mission_gen.h"']
        for entry, symbol in names.items():
            lifter.jump_tables = {ip: table["targets"] for ip, table in tables[entry].items()}
            body = [copy.deepcopy(bodies[entry][ip]) for ip in sorted(bodies[entry])]
            text = lifter.lift_function(symbol, body, 0, entry_addr=entry)
            if any(marker in text for marker in ("UNHANDLED", "needs dispatch", "/* ???", "write ???")):
                raise ValueError(f"Unsupported emitted {name} callback")
            if lifter.func_calls:
                raise ValueError(f"Unexpected unresolved {name} callback call")
            source.append(text)
        source += [f"const RecompFunc g_mission_{stem}_functions[] = {{",
                   *(f"    {{0x{entry:04X}, {symbol}}}," for entry, symbol in names.items()), "};", ""]
        files[f"mission_{stem}_code.c"] = "\n".join(source)
        header.append(f"extern const RecompFunc g_mission_{stem}_functions[];")
        records.append({"name": name, "stem": stem, "payload_bytes": len(payload),
                        "payload_sha256": hashlib.sha256(payload).hexdigest(),
                        "source_offset": KNOWN[name]["offset"], "exports": list(exports),
                        "instructions": len(instructions), "identity_ranges": ranges,
                        "mutable_offsets": mutable,
                        "switch_tables": {f"{ip:04x}": table for group in tables.values()
                                          for ip, table in group.items()}})
    identity = ['/* Generated identity data; never executed or decoded. */', '#include "mission_gen.h"']
    for record in records:
        stem, payload = record["stem"], payloads[record["name"]]
        identity += [f"static const uint8_t mission_{stem}_identity[] = {{",
                     *("    " + ",".join(f"0x{byte:02X}" for byte in payload[i:i + 16]) + ","
                       for i in range(0, len(payload), 16)), "};",
                     f"static const LhxMissionSpan mission_{stem}_spans[] = {{",
                     *(f"    {{{start},{size}}}," for start, size in record["identity_ranges"]), "};"]
    identity.append("const LhxMissionModule g_mission_modules[] = {")
    for record in records:
        stem = record["stem"]
        identity.append(f'    {{"{record["name"]}", mission_{stem}_identity, {record["payload_bytes"]}, '
                        f"g_mission_{stem}_functions, 4, mission_{stem}_spans, {len(record['identity_ranges'])}}},")
    identity += ["};", f"const unsigned g_mission_module_count = {len(records)};", ""]
    report = {"scenario_files_examined": scenario_count, "archive_sha256": ARCHIVE_SHA256,
              "native_modules": records, "unhandled_instructions": [],
              "limitations": ["Only the exact supplied LHX3.LIB archive is supported.",
                              "Mission state stays mutable; every reachable code byte and switch table is verified at dispatch."]}
    files["mission_identity.c"] = "\n".join(identity)
    files["mission_gen.h"] = "\n".join(header) + "\n"
    files["mission_lift.json"] = json.dumps(report, indent=2) + "\n"
    output.mkdir(parents=True, exist_ok=True)
    for name, content in files.items():
        write_if_changed(output / name, content)
    return report


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data-dir", type=Path, default=ROOT / "bin/dos")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--pcrecomp", type=Path, default=ROOT / "third_party/pcrecomp")
    args = parser.parse_args()
    report = generate(args.data_dir, args.output, args.pcrecomp)
    count = sum(len(record["exports"]) for record in report["native_modules"])
    print(f"Lifted {count} native callbacks from {len(report['native_modules'])} mission modules "
          f"({report['scenario_files_examined']} scenarios examined)")


if __name__ == "__main__":
    main()
