"""Visibility measurements use synthetic world lists; no retail assets required."""

import hashlib
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
from measure_visibility import (MEMORY_SIZE, Memory, cutoff_metrics, make_report,
                                nominal_lod, world_inventory)

DS, OBJECTS, GRID, PAYLOAD = 0x1000, 0x2000, 0x3000, 0x4000


def fixture():
    data = bytearray(MEMORY_SIZE)

    def word(seg, off, value):
        struct.pack_into("<H", data, seg * 16 + off, value)

    word(DS, 0xF019, 0x100)
    for field, value in ((4, 2), (6, 1), (8, 0x20), (10, OBJECTS),
                         (12, 0x600), (14, GRID)):
        word(DS, 0x100 + field, value)
    word(GRID, 0x20, 0x100)
    word(GRID, 0x24, 0x400)
    word(DS, 0x120, 0x100)
    word(DS, 0x122, OBJECTS)

    def obj(off, model=0x400, flags=1, sibling=0, x=0, child=0):
        for field, value in ((0, model), (2, flags), (4, sibling)):
            word(OBJECTS, off + field, value)
        struct.pack_into("<iii", data, OBJECTS * 16 + off + 6, x, 0, 0)
        if flags & 0x20:
            word(OBJECTS, off + 0x18, child)

    obj(0x100, sibling=0x200)
    obj(0x200, x=3 * 65536)
    obj(0x300, x=31 * 65536)
    obj(0x400, flags=0x21, child=0x500)
    obj(0x500, model=0x500, sibling=0x200)  # Shared tail must be counted once.
    obj(0x600, flags=0x20, sibling=0x300, child=0x700)
    obj(0x700)  # Child of render-disabled group is deliberately unreachable.
    for record in (0x400, 0x500):
        word(DS, record + 0xE, 31)
        word(DS, record + 0x10, 3)
    for field, value in ((0x12, 0x600), (0x14, 0x620), (0x1E, 0x700)):
        word(DS, 0x400 + field, value)
    data[DS * 16 + 0x600:DS * 16 + 0x602] = bytes((5, 8))
    data[DS * 16 + 0x620:DS * 16 + 0x622] = bytes((20, 32))
    data[DS * 16 + 0x700:DS * 16 + 0x706] = b"near\0\0"
    for field, value in ((0x12, 0x9000), (0x14, 0x9010), (0x1E, 0x9020),
                         (0x20, 0x100), (0x22, PAYLOAD), (0x24, 0x9000),
                         (0x26, 0x25), (0x18, 0x123), (0x1A, 0x456)):
        word(DS, 0x500 + field, value)
    data[PAYLOAD * 16 + 0x100:PAYLOAD * 16 + 0x102] = bytes((7, 9))
    data[PAYLOAD * 16 + 0x110:PAYLOAD * 16 + 0x112] = bytes((30, 50))
    data[PAYLOAD * 16 + 0x120:PAYLOAD * 16 + 0x125] = b"far!\0"
    return data, word


class DistanceMetrics(unittest.TestCase):
    def test_exclusive_original_boundaries(self):
        self.assertEqual(nominal_lod(3 * 65536 - 1, 31, 3), "rich")
        self.assertEqual(nominal_lod(3 * 65536, 31, 3), "simple")
        self.assertEqual(nominal_lod(31 * 65536 - 1, 31, 3), "simple")
        self.assertEqual(nominal_lod(31 * 65536, 31, 3), "distance_omitted")
        self.assertEqual(nominal_lod(0, 0, 0), "distance_omitted")
        # Preserve original word wrapping explicitly rather than mistaking it
        # for a meaningful far-distance measurement.
        self.assertEqual(nominal_lod(0xFFFF0000, 31, 3), "rich")
        with self.assertRaises(ValueError):
            nominal_lod(-1, 31, 3)

    def test_feet_and_miles_and_non_spherical_cutoff(self):
        result = cutoff_metrics(31)
        self.assertEqual(result["l1_boundary_feet_exclusive"], 7936)
        self.assertAlmostEqual(result["axis_cutoff_miles"], 7936 / 5280)
        self.assertAlmostEqual(result["equal_xz_diagonal_cutoff_miles"],
                               7936 / math.sqrt(2) / 5280)
        self.assertAlmostEqual(result["equal_xyz_diagonal_cutoff_miles"],
                               7936 / math.sqrt(3) / 5280)
        self.assertEqual(cutoff_metrics(0)["axis_cutoff_miles"], 0)
        with self.assertRaises(ValueError):
            cutoff_metrics(65536)


class Inventory(unittest.TestCase):
    def test_loaded_lists_groups_dedup_far_models_and_scope(self):
        data, _ = fixture()
        before = hashlib.sha256(data).digest()
        result = make_report(data, DS)
        self.assertEqual(result["object_counts"], {
            "total": 6, "render_enabled": 5, "render_disabled": 1,
            "groups": 2, "render_enabled_groups": 1,
            "ordinary_objects": 4, "render_enabled_ordinary_objects": 4})
        self.assertEqual(result["nominal_distance_only_counts"],
                         {"rich": 2, "simple": 1, "distance_omitted": 1})
        near, far = result["models"]
        self.assertEqual(near["name"], "near")
        self.assertEqual(far["name"], "far!")
        self.assertTrue(far["far_payload"])
        self.assertEqual(far["lods"]["rich"]["vertex_count"], 50)
        self.assertEqual(far["selector_callback"], "0456:0123")
        self.assertIn("not actual visibility", result["scope"])
        self.assertNotIn("main_view_counts", result)
        self.assertEqual(hashlib.sha256(data).digest(), before)

    def test_last_scene_reference_is_explicitly_auxiliary(self):
        data, _ = fixture()
        struct.pack_into("<iii", data, DS * 16 + 0xF01B, 3 * 65536, 0, 0)
        result = make_report(data, DS, "last-scene")
        self.assertEqual(result["reference"]["position_feet"], [768, 0, 0])
        self.assertIn("auxiliary", result["reference"]["description"])
        self.assertEqual(result["nominal_distance_only_counts"]["distance_omitted"], 0)

    def test_cycle_and_out_of_segment_records_fail_clearly(self):
        data, word = fixture()
        word(OBJECTS, 0x204, 0x100)
        with self.assertRaisesRegex(ValueError, "Cycle"):
            world_inventory(Memory(data), DS)
        data, word = fixture()
        word(DS, 0x108, 0xFFFC)
        with self.assertRaisesRegex(ValueError, "segment boundary"):
            world_inventory(Memory(data), DS)
        with self.assertRaisesRegex(ValueError, "snapshot"):
            Memory(bytes(100))

    def test_far_payload_pointer_bounds(self):
        data, word = fixture()
        word(DS, 0x514, 0x9020)  # Name fits; a14-byte LOD here cannot fit.
        with self.assertRaisesRegex(ValueError, "outside far payload"):
            make_report(data, DS)

    def test_cli_json_and_input_preservation(self):
        data, _ = fixture()
        with tempfile.TemporaryDirectory(prefix="lhx-visibility-") as directory:
            ram = Path(directory) / "snapshot.ram"
            ram.write_bytes(data)
            command = [sys.executable, str(ROOT / "tools/measure_visibility.py"),
                       str(ram), "--ds", hex(DS)]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            report = json.loads(result.stdout)
            self.assertEqual(report["object_counts"]["render_enabled"], 5)
            self.assertEqual(ram.read_bytes(), data)
            result = subprocess.run(command + ["--output", str(ram)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("Output must differ", result.stderr)
            self.assertEqual(ram.read_bytes(), data)


if __name__ == "__main__":
    unittest.main()
