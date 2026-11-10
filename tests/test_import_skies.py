"""Exercise real PNG/JPG sky conversion and preserve each pack's attribution."""
import base64
import hashlib
import importlib.util
import json
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SPEC = importlib.util.spec_from_file_location("lhx_import_skies", ROOT / "tools/import_skies.py")
IMPORTER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(IMPORTER)

# Tiny RGB swatches, strictly importer fixtures (2x1 red / blue PNGs).
PNG = {
    "day": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGP4z8AARAAI/gH/xp559wAAAABJRU5ErkJggg==",
    "night": "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGNgYPgPRAAFAgH/wSuWnwAAAABJRU5ErkJggg==",
}
COLOURS = {"day": b"\xff\0\0", "night": b"\0\0\xff"}


@unittest.skipUnless(shutil.which("magick"), "ImageMagick required for optional sky import")
class ImportSkies(unittest.TestCase):
    def sources(self, directory, source_format="png"):
        directory.mkdir(parents=True)
        for name, encoded in PNG.items():
            path = directory / (name+".png")
            path.write_bytes(base64.b64decode(encoded))
            if source_format == "jpg":
                subprocess.run(["magick", str(path), str(path.with_suffix(".jpg"))],
                               check=True, capture_output=True)
                path.unlink()
        attribution = b"Test artwork by the fixture author. No third-party licence implied.\n"
        (directory / "SOURCE.txt").write_bytes(attribution)
        return {p.name: p.read_bytes() for p in directory.iterdir()}

    def check_pack(self, source, output, source_format, licence, original):
        self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, original)
        self.assertEqual((output / "SOURCE.txt").read_bytes(), original["SOURCE.txt"])
        manifest = json.loads((output / "SOURCE.json").read_text())
        self.assertEqual(manifest["licence"], licence)
        self.assertEqual(manifest["source_format"], source_format)
        self.assertEqual(manifest["source"], str(source.resolve()))
        for name, colour in COLOURS.items():
            payload = (output / (name+".lhs")).read_bytes()
            self.assertEqual(payload[:16], b"LHXSKY1\0"+struct.pack("<II", 512, 256))
            self.assertEqual(len(payload), 16+512*256*3)
            if source_format == "png":
                self.assertEqual(payload[16:], colour*(512*256))
            else:
                # JPG is lossy, but a flat colour must survive decoding/resizing.
                for channel in range(3):
                    self.assertTrue(all(abs(value-colour[channel]) <= 2
                                        for value in payload[16+channel::3]))
            self.assertEqual(manifest["images"][name]["sha256"], hashlib.sha256(payload).hexdigest())
            self.assertEqual(manifest["images"][name]["source_sha256"],
                             hashlib.sha256(original[name+"."+source_format]).hexdigest())

    def test_png_cli_custom_attribution_and_nested_sources(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-png-") as directory:
            output = Path(directory) / "pack"
            source = output / "source"
            original = self.sources(source)
            licence = "AI-generated test artwork; rights described in SOURCE.txt"
            result = subprocess.run(["python3", str(ROOT / "tools/import_skies.py"),
                "--source", str(source), "--output", str(output), "--width", "512",
                "--source-format", "png", "--licence", licence], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.check_pack(source, output, "png", licence, original)

    def test_jpg_cli_retains_reference_defaults(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-jpg-") as directory:
            source, output = Path(directory) / "reference", Path(directory) / "native"
            original = self.sources(source, "jpg")
            result = subprocess.run(["python3", str(ROOT / "tools/import_skies.py"),
                "--source", str(source), "--output", str(output), "--width", "512"],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            self.check_pack(source, output, "jpg", "Poly Haven CC0", original)

    def test_nearest_resize_keeps_block_edges_without_extra_colours(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-nearest-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "native"
            self.sources(source)
            # Two adjacent RGB texels must remain two solid blocks after resizing.
            (source / "day.png").write_bytes(base64.b64decode(
                "iVBORw0KGgoAAAANSUhEUgAAAAIAAAABCAIAAAB7QOjdAAAADUlEQVR4nGP4zwAE/wEHAAH/4iOeWQAAAABJRU5ErkJggg=="))
            result = subprocess.run(["python3", str(ROOT / "tools/import_skies.py"),
                "--source", str(source), "--output", str(output), "--width", "512",
                "--source-format", "png", "--licence", "Test licence", "--nearest"],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            row = COLOURS["day"]*256+COLOURS["night"]*256
            self.assertEqual((output / "day.lhs").read_bytes()[16:], row*256)
            manifest = json.loads((output / "SOURCE.json").read_text())
            self.assertEqual(manifest["resize_filter"], "point")

    def test_png_without_licence_does_not_emit_a_mislabelled_pack(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-licence-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "native"
            original = self.sources(source)
            result = subprocess.run(["python3", str(ROOT / "tools/import_skies.py"),
                "--source", str(source), "--output", str(output), "--width", "512",
                "--source-format", "png"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("explicit --licence", result.stderr)
            self.assertFalse(output.exists())
            self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, original)

    def test_incomplete_pair_does_not_replace_an_existing_pack(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-incomplete-") as directory:
            source, output = Path(directory) / "source", Path(directory) / "native"
            self.sources(source)
            IMPORTER.convert(source, output, 512, source_format="png", licence="Test licence")
            original = {p.name: p.read_bytes() for p in output.iterdir()}
            (source / "night.png").unlink()
            with self.assertRaises(subprocess.CalledProcessError):
                IMPORTER.convert(source, output, 512, source_format="png", licence="Test licence")
            self.assertEqual({p.name: p.read_bytes() for p in output.iterdir()}, original)

    def test_destination_cannot_write_inside_source(self):
        with tempfile.TemporaryDirectory(prefix="lhx-sky-readonly-") as directory:
            source = Path(directory) / "source"
            original = self.sources(source)
            for output in (source, source / "native"):
                with self.subTest(output=output), self.assertRaisesRegex(ValueError, "outside the source"):
                    IMPORTER.convert(source, output, 512, source_format="png", licence="Test licence")
            self.assertEqual({p.name: p.read_bytes() for p in source.iterdir()}, original)


if __name__ == "__main__":
    unittest.main()
