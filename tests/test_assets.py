"""Format checks using synthetic bytes plus the optional, supplied retail CD."""

from pathlib import Path
import hashlib
import struct
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from assets import decompress_lzss, members


def archive(payload=b"ABC", name=b"TEST.BIN", kind=0):
    start = 7 + 2 * 18
    return (b"EALIB" + struct.pack("<H", 1)
            + struct.pack("<13sBI", name, kind, start)
            + struct.pack("<13sBI", b"", 0, start + len(payload))
            + payload)


class LZSS(unittest.TestCase):
    def test_literals(self):
        self.assertEqual(decompress_lzss(struct.pack("<I", 3) + b"\x07ABC"), b"ABC")

    def test_overlapping_backreference(self):
        # The first literal goes at ring 0xfee. A backreference reads it and
        # then reads its own newly appended bytes, producing a run of six A's.
        self.assertEqual(decompress_lzss(struct.pack("<I", 6) + b"\x01A\xee\xf2"),
                         b"AAAAAA")

    def test_space_filled_initial_history(self):
        self.assertEqual(decompress_lzss(struct.pack("<I", 3) + b"\x00\x00\x00"),
                         b"   ")

    def test_zeroed_lookahead_and_history_wrap(self):
        self.assertEqual(decompress_lzss(struct.pack("<I", 3) + b"\x00\xfd\xf0"),
                         b"\0\0\0")
        self.assertEqual(decompress_lzss(struct.pack("<I", 3) + b"\x00\xff\xf0"),
                         b"\0  ")

    def test_truncated_stream(self):
        for data in (b"", b"\x01", struct.pack("<I", 1),
                     struct.pack("<I", 1) + b"\x01",
                     struct.pack("<I", 3) + b"\x00\xee"):
            with self.subTest(data=data), self.assertRaises(ValueError):
                decompress_lzss(data)

    def test_output_overflow(self):
        with self.assertRaisesRegex(ValueError, "exceeds declared"):
            decompress_lzss(struct.pack("<I", 2) + b"\x00\x00\x00")

    def test_trailing_input_rejected(self):
        with self.assertRaisesRegex(ValueError, "Trailing"):
            decompress_lzss(struct.pack("<I", 1) + b"\x01Aextra")

    def test_allocation_bound(self):
        with self.assertRaisesRegex(ValueError, "exceeds 16 MiB"):
            decompress_lzss(struct.pack("<I", 16 * 1024 * 1024 + 1))


class EALIB(unittest.TestCase):
    def test_single_member(self):
        entry, = members(archive())
        self.assertEqual((entry.name, entry.offset, entry.decode()),
                         ("TEST.BIN", 43, b"ABC"))

    def test_compressed_member(self):
        entry, = members(archive(struct.pack("<I", 3) + b"\x07ABC", kind=1))
        self.assertEqual(entry.decode(), b"ABC")

    def test_other_type_preserved(self):
        entry, = members(archive(b"PXPK", kind=3))
        self.assertEqual(entry.decode(), b"PXPK")

    def test_directory_bounds(self):
        for data in (b"EALIB\x01\x00", archive()[:25], archive()[:-1]):
            with self.subTest(data=data), self.assertRaises(ValueError):
                members(data)

    def test_offset_inside_directory(self):
        data = bytearray(archive())
        struct.pack_into("<I", data, 21, 7)
        with self.assertRaisesRegex(ValueError, "outside file data"):
            members(data)

    def test_sentinel_must_match_file_size(self):
        data = bytearray(archive())
        struct.pack_into("<I", data, 39, len(data) + 1)
        with self.assertRaisesRegex(ValueError, "sentinel"):
            members(data)

    def test_filename_cannot_escape_output_directory(self):
        for name in (b"../TEST", b"C:TEST", b"TEST\\NAME", b".."):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, "Unsafe"):
                members(archive(name=name))

    @unittest.skipUnless((ROOT / "bin/dos/LHX4.LIB").exists(), "retail files absent")
    def test_supplied_cd(self):
        counts = [60, 53, 11, 64, 29]
        driver_hashes = {
            "IBMDRIVE.DRV": "17dc7ea8077160c658397883364317347534da6e49050776c32b0c3e6eea3688",
            "TNDDRIVE.DRV": "5671a16734f0ac7ffd56e5adecbe9d4ad8e3e4e283b935d48eca10e032ce61ef",
            "ADLDRIVE.DRV": "f7786992dcf36c6e1452e3c29d884e5043622447b81f5279ad5ae7947c8bf2df",
            "AH-MCGA.BIN": "50778522fc128dbda83a2aa99c03d19909374c8dc31ef7c2b27a79df6755b6c8",
        }
        total = 0
        for index, count in enumerate(counts):
            entries = members((ROOT / f"bin/dos/LHX{index}.LIB").read_bytes())
            self.assertEqual(len(entries), count)
            for member in entries:
                decoded = member.decode()
                if member.name in driver_hashes:
                    self.assertEqual(hashlib.sha256(decoded).hexdigest(),
                                     driver_hashes[member.name])
                if member.name == "AH-MCGA.BIN":
                    self.assertEqual(decoded[0x1ac:0x1b0], b"    ")
            total += len(entries)
        self.assertEqual(total, 217)

    @unittest.skipUnless((ROOT / "build/menu-aircraft.ram").exists(),
                         "recorded cockpit RAM evidence absent")
    def test_ah_cockpit_matches_original_decoder_ram(self):
        entries = members((ROOT / "bin/dos/LHX3.LIB").read_bytes())
        payload = next(member.decode() for member in entries
                       if member.name == "AH-MCGA.BIN")
        ram = (ROOT / "build/menu-aircraft.ram").read_bytes()
        # The captured original loader placed AH-MCGA.BIN at 78D6:0000.
        self.assertEqual(ram[0x78d60:0x78d60 + len(payload)], payload)


if __name__ == "__main__":
    unittest.main()
