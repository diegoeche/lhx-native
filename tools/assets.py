#!/usr/bin/env python3
"""Inspect LHX's EALIB archives and optionally extract their stored members.

Type 1 members use an LZSS stream prefixed by the unpacked 32-bit byte count.
Type 0 members are raw; other types (including PXPK pictures) are preserved.
This tool never executes the executable sound-driver members.
"""

from __future__ import annotations

import argparse
from dataclasses import dataclass
import hashlib
import json
from pathlib import Path
import struct


@dataclass(frozen=True)
class Member:
    name: str
    kind: int
    offset: int
    stored: bytes

    def decode(self) -> bytes:
        return decompress_lzss(self.stored) if self.kind == 1 else self.stored


def members(data: bytes) -> list[Member]:
    if len(data) < 7 or data[:5] != b"EALIB":
        raise ValueError("Expected EALIB header")
    count = struct.unpack_from("<H", data, 5)[0]
    table_end = 7 + (count + 1) * 18
    if table_end > len(data):
        raise ValueError("Truncated EALIB directory")
    rows = []
    names: set[str] = set()
    for i in range(count + 1):
        pos = 7 + i * 18
        raw_name, kind, start = struct.unpack_from("<13sBI", data, pos)
        name = raw_name.split(b"\0", 1)[0].decode("ascii")
        if i == count:
            if name or kind != 0 or start != len(data):
                raise ValueError("Invalid EALIB end-of-file sentinel")
        else:
            if not name or name in (".", "..") or any(c in name for c in "/\\:"):
                raise ValueError(f"Unsafe EALIB name: {name!r}")
            if name.upper() in names:
                raise ValueError(f"Duplicate EALIB name: {name}")
            names.add(name.upper())
        if not table_end <= start <= len(data):
            raise ValueError("EALIB member offset outside file data")
        if rows and start < rows[-1][2]:
            raise ValueError("EALIB member offsets are not ordered")
        rows.append((name, kind, start))
    return [Member(name, kind, start, data[start:rows[i + 1][2]])
            for i, (name, kind, start) in enumerate(rows[:-1])]


def decompress_lzss(data: bytes) -> bytes:
    if len(data) < 4:
        raise ValueError("Truncated EALIB LZSS length")
    size = struct.unpack_from("<I", data)[0]
    if size > 16 * 1024 * 1024:
        raise ValueError("EALIB LZSS output exceeds 16 MiB")
    # The original decoder (image 2F295 / 2F405) fills the initialized
    # history with spaces; its final 18-byte lookahead area starts at zero.
    ring = bytearray(b" " * 0xFEE + b"\0" * 18)
    write = 0xFEE
    cursor = 4
    out = bytearray()

    def byte() -> int:
        nonlocal cursor
        if cursor >= len(data):
            raise ValueError("Truncated EALIB LZSS stream")
        value = data[cursor]
        cursor += 1
        return value

    def emit(value: int) -> None:
        nonlocal write
        if len(out) >= size:
            raise ValueError("EALIB LZSS reference exceeds declared output")
        out.append(value)
        ring[write] = value
        write = (write + 1) & 4095

    while len(out) < size:
        flags = byte()
        for bit in range(8):
            if len(out) == size:
                break
            if flags & (1 << bit):
                emit(byte())
            else:
                lo, hi = byte(), byte()
                read = lo | ((hi & 0xF0) << 4)
                for n in range((hi & 15) + 3):
                    emit(ring[(read + n) & 4095])
    if cursor != len(data):
        raise ValueError("Trailing bytes after EALIB LZSS stream")
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", type=Path, nargs="+")
    parser.add_argument("--extract", type=Path,
                        help="Write decoded type 1 / unchanged other members here")
    parser.add_argument("--only", action="append", default=[],
                        help="Select an exact member name (repeatable)")
    args = parser.parse_args()
    wanted = {name.upper() for name in args.only}
    report = []
    for archive in args.archives:
        for member in members(archive.read_bytes()):
            if wanted and member.name.upper() not in wanted:
                continue
            decoded = member.decode()
            report.append({"archive": str(archive), "name": member.name,
                           "type": member.kind, "offset": member.offset,
                           "stored_bytes": len(member.stored),
                           "decoded_bytes": len(decoded),
                           "decoded_sha256": hashlib.sha256(decoded).hexdigest()})
            if args.extract:
                directory = args.extract / archive.stem
                directory.mkdir(parents=True, exist_ok=True)
                target = directory / member.name
                if target.exists() and target.read_bytes() != decoded:
                    raise ValueError(f"Refusing to replace different file: {target}")
                if not target.exists():
                    target.write_bytes(decoded)
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
