"""Boundary and reconstruction checks for the binary preparation step."""

from pathlib import Path
import hashlib
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from prepare import iso_files, mz_header, unpack_exepack, write_preserving


def fixture(opcode=0xB1, count=48):
    # Synthetic EXEPACK: keep a 16-byte prefix, expand a 48-byte suffix.
    packed = b"A" * 16 + b"X" + struct.pack("<H", count) + bytes([opcode]) + b"\xff" * 12
    block = bytearray(0x140)
    struct.pack_into("<9H", block, 0, 0, 0, 0, len(block), 32, 2, 4, 1, 0x4252)
    block[115:127] = b"\xbe\x20\x01\x0e\x1f\x8b\x1e\x04\x00\xfc\x33\xd2"
    module = packed + block
    total = 32 + len(module)
    header = struct.pack("<16H", 0x5A4D, total % 512, (total + 511) // 512,
                         0, 2, 0, 0xFFFF, 0, 128, 0, 18, 2, 28, 0, 0, 0)
    return header + module


class PreparationTests(unittest.TestCase):
    def test_backwards_fill_and_original_header(self):
        result, report = unpack_exepack(fixture())
        h, module = mz_header(result)
        self.assertEqual(module, b"A" * 16 + b"X" * 48)
        self.assertEqual((h[3], h[7], h[8], h[10], h[11]), (0, 2, 32, 0, 0))
        self.assertEqual(report["compression_records"], 1)

    def test_invalid_opcode_and_output_underflow(self):
        with self.assertRaisesRegex(ValueError, "opcode"):
            unpack_exepack(fixture(opcode=0xB5))
        with self.assertRaisesRegex(ValueError, "underflow"):
            unpack_exepack(fixture(count=100))

    def test_literal_copy_record(self):
        data = bytearray(fixture())
        # Backwards decoding expands the fill first, then copies the literal
        # block. Neither the literal order nor the untouched prefix reverses.
        literal = b"01234567"
        packed = (b"B" * 16 + literal + struct.pack("<H", 8) + b"\xb3"
                  + b"X" + struct.pack("<H", 40) + b"\xb0" + b"\xff")
        data[32:64] = packed
        result, _ = unpack_exepack(bytes(data))
        self.assertEqual(mz_header(result)[1], b"B" * 16 + literal + b"X" * 40)

    def test_refuses_to_replace_existing_user_file(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "game"
            write_preserving(p, b"original")
            write_preserving(p, b"original")
            with self.assertRaisesRegex(ValueError, "Refusing"):
                write_preserving(p, b"different")
            self.assertEqual(p.read_bytes(), b"original")

    def test_rejects_non_iso(self):
        with self.assertRaisesRegex(ValueError, "ISO"):
            iso_files(bytes(40000))

    @unittest.skipUnless(Path("bin/dos/LHX.EXE").exists(), "Retail data not installed")
    def test_supplied_release_reconstruction(self):
        source = Path("bin/dos/LHX.EXE").read_bytes()
        if hashlib.sha256(source).hexdigest() != "d0afb93033121ba2403cf4f82afa6865e4b889e961733e791aa5929b94e7d325":
            self.skipTest("Different release")
        unpacked, report = unpack_exepack(source)
        _, image = mz_header(unpacked)
        self.assertEqual(hashlib.sha256(image).hexdigest(),
                         "4d30628da15b38eb93cad06420d255574db7283cfec4292f9bfbc8b9d4369f9e")
        self.assertEqual(report["relocations"], 2650)
        self.assertEqual(report["compression_records"], 606)


if __name__ == "__main__":
    unittest.main()
