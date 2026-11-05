#!/usr/bin/env python3
"""Extract the supplied LHX CD and unpack EXEPACK without running DOS code."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

SECTOR = 2048
GAME_DIRECTORY = "HITSQUAD/LHX/"
REQUIRED_FILES = {"LHX.EXE", *(f"LHX{i}.LIB" for i in range(5))}


def u16(data: bytes, offset: int) -> int:
    if not 0 <= offset <= len(data) - 2:
        raise ValueError(f"16-bit field outside input at {offset:#x}")
    return struct.unpack_from("<H", data, offset)[0]


def sha256(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def iso_files(data: bytes) -> dict[str, bytes]:
    """Read regular, single-extent ISO 9660 files; never mount or follow links."""
    pvd = 16 * SECTOR
    if data[pvd:pvd + 7] != b"\x01CD001\x01":
        raise ValueError("Expected an ISO 9660 primary volume descriptor")
    if struct.unpack_from("<H", data, pvd + 128)[0] != SECTOR:
        raise ValueError("Only 2048-byte logical sectors are supported")
    files: dict[str, bytes] = {}
    seen: set[tuple[int, int]] = set()

    def record(offset: int, limit: int) -> tuple[int, int, int, str, int]:
        size = data[offset]
        if size < 34 or offset + size > limit:
            raise ValueError("Truncated ISO directory record")
        extent, count = (struct.unpack_from("<I", data, offset + i)[0]
                         for i in (2, 10))
        if extent != struct.unpack_from(">I", data, offset + 6)[0]:
            raise ValueError("Inconsistent ISO extent")
        if count != struct.unpack_from(">I", data, offset + 14)[0]:
            raise ValueError("Inconsistent ISO length")
        start = extent * SECTOR + data[offset + 1] * SECTOR
        if start + count > len(data):
            raise ValueError("ISO extent outside input")
        flags = data[offset + 25]
        if flags & 0x80 or data[offset + 26] or data[offset + 27]:
            raise ValueError("Multi-extent or interleaved ISO files unsupported")
        name_len = data[offset + 32]
        if 33 + name_len > size:
            raise ValueError("Invalid ISO filename length")
        name = data[offset + 33:offset + 33 + name_len].decode("ascii")
        return start, count, flags, name, size

    def walk(start: int, count: int, prefix: str) -> None:
        key = start, count
        if key in seen:
            raise ValueError("Repeated/cyclic ISO directory")
        seen.add(key)
        offset, end = start, start + count
        while offset < end:
            if data[offset] == 0:
                offset = (offset // SECTOR + 1) * SECTOR
                continue
            pos, length, flags, name, size = record(offset, end)
            offset += size
            if name in ("\x00", "\x01"):
                continue
            name = name.split(";")[0]
            if not name or name in (".", "..") or any(c in name for c in "/\\:"):
                raise ValueError(f"Unsafe ISO name: {name!r}")
            path = prefix + name
            if flags & 2:
                walk(pos, length, path + "/")
            elif path in files:
                raise ValueError(f"Duplicate ISO path: {path}")
            else:
                files[path] = data[pos:pos + length]

    start, count, flags, _, _ = record(pvd + 156, pvd + SECTOR)
    if not flags & 2:
        raise ValueError("Invalid ISO root directory")
    walk(start, count, "")
    return files


def mz_header(data: bytes) -> tuple[list[int], bytes]:
    if len(data) < 28 or data[:2] != b"MZ":
        raise ValueError("Expected a DOS MZ executable")
    h = list(struct.unpack_from("<14H", data))
    size = (h[2] - 1) * 512 + (h[1] or 512)
    header_bytes = h[4] * 16
    if h[2] == 0 or h[1] > 511 or not 28 <= header_bytes <= size <= len(data):
        raise ValueError("Invalid MZ size/header")
    if size != len(data):
        raise ValueError("Appended MZ overlays are not supported")
    if h[12] + 4 * h[3] > header_bytes:
        raise ValueError("MZ relocation table outside header")
    return h, data[header_bytes:size]


def unpack_exepack(data: bytes) -> tuple[bytes, dict]:
    """Decode the 18-byte EXEPACK variant used by the supplied CD release.

    The actual stub is checked for its relocation-table instruction. Commands
    are read backwards, exactly as its STD/LODS/REP MOVS/STOS loop does.
    This is file decompression, not CPU execution.
    """
    h, module = mz_header(data)
    packed_length = h[11] * 16
    entry = packed_length + h[10]
    if h[10] != 18 or module[entry - 2:entry] != b"RB":
        raise ValueError("Expected LHX's 18-byte EXEPACK header")
    if h[3] != 0 or packed_length + 18 > len(module):
        raise ValueError("Unexpected packed MZ relocation/header layout")
    ip, cs, _, block_size, sp, ss, paragraphs, skip, magic = struct.unpack_from(
        "<9H", module, packed_length)
    if magic != 0x4252 or skip != 1 or paragraphs == 0:
        raise ValueError("Unsupported EXEPACK variant")
    if packed_length + block_size != len(module):
        raise ValueError("EXEPACK block size does not cover MZ module")
    packed = module[:packed_length]
    output = bytearray(paragraphs * 16)
    if len(packed) > len(output):
        raise ValueError("Packed input exceeds output size")
    output[:len(packed)] = packed
    src, dst = len(packed) - 1, len(output) - 1
    while src >= 0 and packed[src] == 0xFF:
        src -= 1
    if len(packed) - src - 1 >= 16:
        raise ValueError("Too much EXEPACK alignment padding")
    commands = 0
    while True:
        if src < 2:
            raise ValueError("EXEPACK command outside input")
        opcode = packed[src]
        count = u16(packed, src - 2)
        src -= 3
        if count > dst + 1:
            raise ValueError("EXEPACK output underflow")
        if opcode & 0xFE == 0xB0:
            if src < 0:
                raise ValueError("EXEPACK fill byte missing")
            output[dst - count + 1:dst + 1] = bytes([packed[src]]) * count
            src -= 1
        elif opcode & 0xFE == 0xB2:
            if count > src + 1:
                raise ValueError("EXEPACK literal input underflow")
            output[dst - count + 1:dst + 1] = packed[src - count + 1:src + 1]
            src -= count
        else:
            raise ValueError(f"Unknown EXEPACK opcode {opcode:#x}")
        dst -= count
        commands += 1
        if opcode & 1:
            break
    if src != dst:
        raise ValueError("EXEPACK uncompressed-prefix positions disagree")

    stub = module[entry:packed_length + block_size]
    pattern = b"\x0e\x1f\x8b\x1e\x04\x00\xfc\x33\xd2"
    marker = stub.find(pattern, 100, 200)
    if marker < 3 or stub[marker - 3] != 0xBE:
        raise ValueError("Unrecognized EXEPACK relocation loop")
    table_start = u16(stub, marker - 2)
    cursor = packed_length + table_start
    relocs = []
    for bank in range(16):
        count = u16(module, cursor)
        cursor += 2
        for _ in range(count):
            offset = u16(module, cursor)
            cursor += 2
            if bank * 65536 + offset + 2 > len(output):
                raise ValueError("Relocation outside unpacked image")
            relocs.append((offset, bank * 0x1000))
    if cursor != len(module):
        raise ValueError("EXEPACK relocation table has trailing/missing bytes")
    if cs * 16 + ip >= len(output) or ss * 16 + sp > len(output):
        raise ValueError("Unpacked entry/stack outside image")
    header_size = (28 + len(relocs) * 4 + 15) // 16 * 16
    total = header_size + len(output)
    h[1:5] = [total % 512, (total + 511) // 512, len(relocs), header_size // 16]
    h[5:13] = [0, 0xFFFF, ss, sp, 0, ip, cs, 28]
    header = bytearray(header_size)
    struct.pack_into("<14H", header, 0, *h)
    for i, pair in enumerate(relocs):
        struct.pack_into("<HH", header, 28 + 4 * i, *pair)
    unpacked = bytes(header + output)
    return unpacked, {
        "packed_sha256": sha256(data), "unpacked_sha256": sha256(unpacked),
        "image_sha256": sha256(output), "image_bytes": len(output),
        "relocations": len(relocs), "compression_records": commands,
        "entry_cs": cs, "entry_ip": ip, "stack_ss": ss, "stack_sp": sp,
        "relocation_table_offset_in_exepack_block": table_start,
    }


def write_preserving(path: Path, data: bytes) -> None:
    if path.exists():
        if path.read_bytes() != data:
            raise ValueError(f"Refusing to replace different existing file: {path}")
    else:
        path.write_bytes(data)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iso", type=Path, default=Path("bin/LHX.iso"))
    parser.add_argument("--archive", type=Path, help="Read the ISO directly from a ZIP")
    parser.add_argument("--output", type=Path, default=Path("bin/dos"))
    args = parser.parse_args()
    if args.archive:
        with zipfile.ZipFile(args.archive) as archive:
            members = [m for m in archive.infolist() if m.filename.lower().endswith(".iso")]
            if len(members) != 1 or members[0].file_size > 128 * 1024 * 1024:
                raise ValueError("Expected one ISO smaller than 128 MiB in the ZIP")
            iso = archive.read(members[0])
    else:
        iso = args.iso.read_bytes()
    files = {path.removeprefix(GAME_DIRECTORY): data
             for path, data in iso_files(iso).items()
             if path.startswith(GAME_DIRECTORY) and "/" not in path[len(GAME_DIRECTORY):]}
    if missing := REQUIRED_FILES - files.keys():
        raise ValueError(f"CD is missing game files: {sorted(missing)}")
    unpacked, report = unpack_exepack(files["LHX.EXE"])
    args.output.mkdir(parents=True, exist_ok=True)
    for name, data in files.items():
        write_preserving(args.output / name, data)
    write_preserving(args.output / "LHX.unpacked.EXE", unpacked)
    report["iso_sha256"] = sha256(iso)
    report["files"] = {name: {"bytes": len(data), "sha256": sha256(data)}
                       for name, data in sorted(files.items())}
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Prepared {len(files)} game files in {args.output}")
    print(f"Unpacked {report['image_bytes']} bytes, {report['relocations']} relocations")


if __name__ == "__main__":
    main()
