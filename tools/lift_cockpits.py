#!/usr/bin/env python3
"""Compile the supplied VGA cockpit draw callbacks ahead of time.

Each MCGA resource contains raster data, a palette-use list, and one native
blitter. Only header word zero is a code export; word two points to the list.
The original loader still supplies the raster bytes as data at runtime.
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
KNOWN = {
    "LHX-MCGA.BIN": (0xa26, "8df63798727ec01472556b367b7b3911f170345e53a2f42c8121edb55561c622"),
    "V-MCGA.BIN": (0xc9b, "e9569093f25f03a34c02f643d0e143e9f7b7e6165bbdef57eddab0ae7ae2cf56"),
    "UH-MCGA.BIN": (0xe8d, "9699bffd97f10d359d8b6a9256ecf17e94ef5014b09237a8fcf25be6d558f9c2"),
    "AH-MCGA.BIN": (0xc77, "50778522fc128dbda83a2aa99c03d19909374c8dc31ef7c2b27a79df6755b6c8"),
}


def cockpit_payloads(data_dir: Path) -> dict[str, bytes]:
    archive = (data_dir / "LHX3.LIB").read_bytes()
    if hashlib.sha256(archive).hexdigest() != ARCHIVE_SHA256:
        raise ValueError("Unsupported LHX3.LIB SHA256 for native VGA cockpit callbacks")
    payloads = {}
    for member in members(archive):
        name = member.name.upper()
        if name not in KNOWN:
            continue
        payload = member.decode()
        entry, palette = struct.unpack_from("<HH", payload)
        if (hashlib.sha256(payload).hexdigest() != KNOWN[name][1]
                or entry != KNOWN[name][0] or not 4 <= palette < entry
                or payload[entry:entry + 3] != b"\x55\x8b\xec"
                or payload[palette:entry][-1] != 0xff):
            raise ValueError(f"Unsupported native cockpit payload: {name}")
        if name in payloads:
            raise ValueError(f"Duplicate native cockpit payload: {name}")
        payloads[name] = payload
    if payloads.keys() != KNOWN.keys():
        raise ValueError("Missing one of the four supplied native VGA cockpits")
    return payloads


def discover(payload, Decoder, OpType):
    """Validate the pinned, straight-line blitter through its final RETF."""
    entry = struct.unpack_from("<H", payload)[0]
    decoder = Decoder(payload, 0)
    decoder.pos = entry
    body = []
    while decoder.pos < len(payload):
        ins = decoder.decode_one()
        if (ins is None or ins.mnemonic not in {"push", "pop", "mov", "movsb", "retf"}
                or not ins.length or ins.offset + ins.length > len(payload)):
            raise ValueError(f"Unexpected cockpit instruction at {decoder.pos:#x}")
        if ins.mnemonic == "movsb" and (ins.prefix != "rep" or ins.seg_override):
            raise ValueError("Unsupported cockpit string-copy form")
        if ins.mnemonic == "mov" and ins.op1.type in (OpType.MEM, OpType.MOFFS):
            if ins.op1.seg != "es" or ins.op1.base or ins.op1.index:
                raise ValueError("Cockpit writes outside its destination framebuffer")
        body.append(ins)
        if ins.mnemonic == "retf":
            if ins.op1 or decoder.pos != len(payload):
                raise ValueError("Unexpected cockpit return or trailing code")
            return entry, body
    raise ValueError("Cockpit draw callback has no far return")


def write_if_changed(path: Path, content: str) -> None:
    if not path.exists() or path.read_text() != content:
        path.write_text(content)


def generate(data_dir: Path, output: Path, pcrecomp: Path) -> dict:
    for name in ("disasm", "lift"):
        sys.path.insert(0, str(pcrecomp / "tools" / name))
    import decode16
    import lift16
    from decode16 import Decoder, OpType
    decode16.WRAP_NEAR_TARGETS, decode16.EMU87_INTS = True, False
    lift16._CODE_SEG = None
    files, records = {}, []
    payloads = cockpit_payloads(data_dir)
    header = ['#pragma once', '#include "cockpit_module.h"']
    for name, payload in sorted(payloads.items()):
        stem = name.split("-", 1)[0].lower()
        entry, body = discover(payload, Decoder, OpType)
        symbol = f"cockpit_{stem}_draw"
        lifter = lift16.Lifter(known_funcs={entry: symbol})
        lifter.dispatch, lifter.iret_frame = True, True
        source = lifter.lift_function(symbol, copy.deepcopy(body), 0, entry_addr=entry)
        if (lifter.func_calls or any(marker in source for marker in
                ("UNHANDLED", "needs dispatch", "/* ???", "write ???", "recomp_div0"))):
            raise ValueError(f"Unsupported emitted cockpit callback: {name}")
        files[f"cockpit_{stem}_code.c"] = (
            '/* Generated from the owner-supplied cockpit; do not commit. */\n'
            '#include "cockpit_gen.h"\n' + source + "\n")
        header.append(f"void {symbol}(CPU *);")
        records.append({"name": name, "stem": stem, "entry": entry,
                        "payload_bytes": len(payload), "instructions": len(body),
                        "payload_sha256": hashlib.sha256(payload).hexdigest()})
    identity = ['/* Generated immutable resource identity; never executed or decoded. */',
                '#include "cockpit_gen.h"']
    for record in records:
        payload, stem = payloads[record["name"]], record["stem"]
        identity += [f"static const uint8_t cockpit_{stem}_identity[] = {{",
                     *("    " + ",".join(f"0x{byte:02X}" for byte in payload[i:i + 16]) + ","
                       for i in range(0, len(payload), 16)), "};"]
    identity.append("const LhxCockpitModule g_cockpit_modules[] = {")
    for record in records:
        stem = record["stem"]
        identity.append(f'    {{"{record["name"]}", cockpit_{stem}_identity, '
                        f'{record["payload_bytes"]}, {record["entry"]}, cockpit_{stem}_draw}},')
    identity += ["};", f"const unsigned g_cockpit_module_count = {len(records)};", ""]
    report = {"archive_sha256": ARCHIVE_SHA256, "native_modules": records,
              "unhandled_instructions": [],
              "limitations": ["Only the exact supplied four VGA MCGA resources are supported.",
                              "All resource bytes are immutable and verified at each native dispatch."]}
    files["cockpit_identity.c"] = "\n".join(identity)
    files["cockpit_gen.h"] = "\n".join(header) + "\n"
    files["cockpit_lift.json"] = json.dumps(report, indent=2) + "\n"
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
    print(f"Lifted {len(report['native_modules'])} native VGA cockpit draw callbacks")


if __name__ == "__main__":
    main()
