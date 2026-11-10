"""Bounded visual motion; authoritative near/disabled paths and native trace replay."""
import ctypes
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NEAR = 49152.0


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class SceneMotion(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="lhx-scene-motion-")
        tmp = Path(cls.directory.name)
        (tmp / "size.c").write_text('''
#include <stddef.h>
#include "scene_motion.h"
size_t motion_size(void){return sizeof(LhxSceneMotion);}
unsigned motion_used(const LhxSceneMotion*m){
 unsigned n=0;
 for(unsigned i=0;i<LHX_SCENE_MOTION_CAPACITY;i++){
  n+=m->entries[i].identity!=0;
 }
 return n;
}
''')
        result = subprocess.run([
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-shared", "-fPIC", "-I", str(ROOT / "runtime"),
            str(tmp / "size.c"), str(ROOT / "runtime/scene_motion.c"),
            "-lm", "-o", str(tmp / "motion.so")], capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.lib = ctypes.CDLL(str(tmp / "motion.so"))
        cls.lib.motion_size.restype = ctypes.c_size_t
        cls.lib.motion_used.argtypes = [ctypes.c_void_p]
        cls.lib.motion_used.restype = ctypes.c_uint
        cls.lib.lhx_scene_motion_reset.argtypes = [ctypes.c_void_p]
        cls.lib.lhx_scene_motion_offset.argtypes = [
            ctypes.c_void_p, ctypes.c_int, ctypes.c_uint64,
            ctypes.POINTER(ctypes.c_double), ctypes.c_double, ctypes.c_double,
            ctypes.POINTER(ctypes.c_double)]

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def setUp(self):
        self.history = ctypes.create_string_buffer(self.lib.motion_size())

    def offset(self, t, position, identity=1, distance=60000, enabled=True):
        point = (ctypes.c_double * 3)(*position)
        before = bytes(point)
        out = (ctypes.c_double * 3)(123, 123, 123)
        self.lib.lhx_scene_motion_offset(self.history, enabled, identity, point,
                                         distance, t, out)
        self.assertEqual(bytes(point), before, "authored input changed")
        self.assertTrue(all(math.isfinite(v) for v in out))
        return tuple(out)

    def visual(self, t, position, **kwargs):
        return tuple(a + b for a, b in zip(position, self.offset(t, position, **kwargs)))

    def test_held_samples_and_variable_intervals_retarget_displayed_origin(self):
        self.assertEqual(self.offset(0, (0, 0, 0)), (0, 0, 0))
        self.assertEqual(self.visual(4, (400, 80, -40)), (0, 0, 0))
        self.assertEqual(self.visual(5, (400, 80, -40)), (100, 20, -10))
        old = self.visual(6, (400, 80, -40))
        self.assertEqual(old, (200, 40, -20))
        # A shorter following interval starts at the display's current origin.
        self.assertEqual(self.visual(6, (600, 120, -60)), old)
        self.assertEqual(self.visual(7, (600, 120, -60)), (400, 80, -40))
        self.assertEqual(self.offset(8, (600, 120, -60)), (0, 0, 0))
        self.assertEqual(self.offset(20, (600, 120, -60)), (0, 0, 0))

    def test_disabled_first_near_and_range_transition(self):
        self.offset(0, (0, 0, 0))
        self.offset(4, (400, 0, 0))
        self.assertEqual(self.offset(5, (400, 0, 0)), (-300, 0, 0))
        self.assertEqual(self.offset(5, (400, 0, 0), distance=NEAR + 1),
                         (-300, 0, 0))
        for distance in (NEAR, 45000, 7936, 6000, 0, -1):
            self.assertEqual(self.offset(5, (400, 0, 0), distance=distance), (0, 0, 0))
        self.assertEqual(self.offset(5, (400, 0, 0)), (0, 0, 0))
        self.offset(9, (800, 0, 0))
        self.assertNotEqual(self.offset(10, (800, 0, 0)), (0, 0, 0))
        self.assertEqual(self.offset(10, (800, 0, 0), enabled=False), (0, 0, 0))
        self.assertFalse(any(self.history.raw))
        self.assertEqual(self.offset(10, (800, 0, 0)), (0, 0, 0))

    def test_clock_reset_invalid_data_and_duplicate_timestamps(self):
        self.offset(0, (0, 0, 0))
        self.offset(2, (100, 0, 0))
        self.assertEqual(self.offset(3, (100, 0, 0)), (-50, 0, 0))
        self.assertEqual(self.offset(3, (100, 0, 0)), (-50, 0, 0))
        self.assertEqual(self.offset(1, (100, 0, 0)), (0, 0, 0))
        self.assertEqual(self.offset(1, (200, 0, 0)), (0, 0, 0))
        for t in (math.nan, math.inf, -1):
            self.assertEqual(self.offset(t, (300, 0, 0)), (0, 0, 0))
            self.assertFalse(any(self.history.raw))
        for position in ((math.nan, 0, 0), (0, math.inf, 0), (0, 0, -math.inf)):
            self.assertEqual(self.offset(4, position), (0, 0, 0))
        for distance in (math.nan, math.inf):
            self.assertEqual(self.offset(4, (400, 0, 0), distance=distance), (0, 0, 0))
        self.assertEqual(self.offset(4, (400, 0, 0), identity=0), (0, 0, 0))

    def test_reuse_teleport_absence_and_bounded_cache(self):
        self.offset(0, (0, 0, 0))
        self.offset(4, (400, 0, 0))
        self.assertEqual(self.offset(5, (400, 0, 0), identity=(1 << 32) | 1),
                         (0, 0, 0))
        self.assertEqual(self.offset(6, (20000, 0, 0)), (0, 0, 0))
        self.offset(8, (20100, 0, 0))
        self.assertEqual(self.offset(17, (20200, 0, 0)), (0, 0, 0))
        self.lib.lhx_scene_motion_reset(self.history)
        self.offset(0, (0, 0, 0))
        self.offset(4, (400, 0, 0))
        for identity in range(2, 130):
            self.assertEqual(self.offset(5, (0, 0, 0), identity=identity), (0, 0, 0))
        self.assertEqual(self.lib.motion_used(self.history), 128)
        self.assertEqual(self.offset(6, (500, 0, 0)), (0, 0, 0))
        self.assertEqual(self.lib.motion_used(self.history), 128)

    def test_interval_bounds_and_world_space_history(self):
        self.offset(0, (0, 0, 0))
        for t in range(1, 9):
            self.offset(t, (0, 0, 0))
        self.assertEqual(self.visual(8, (400, 0, 0)), (0, 0, 0))
        self.assertEqual(self.visual(9, (400, 0, 0)), (100, 0, 0))
        self.assertEqual(self.offset(12, (400, 0, 0)), (0, 0, 0))
        self.lib.lhx_scene_motion_reset(self.history)
        self.offset(0, (0, 0, 0))
        self.offset(.001, (1, 0, 0))
        self.assertAlmostEqual(self.visual(.001 + 1 / 120, (1, 0, 0))[0], .5)
        # The camera's distant range changes while world origin is stationary.
        self.lib.lhx_scene_motion_reset(self.history)
        for t, distance in enumerate((60000, 70000, 80000, 55000)):
            self.assertEqual(self.offset(t, (1e6, -100, 32768), distance=distance),
                             (0, 0, 0))
        self.assertEqual(self.offset(5, (-1e308, 0, 1e308)), (0, 0, 0))
        self.assertEqual(self.offset(6, (1e308, 0, -1e308)), (0, 0, 0))

    def test_recorded_native_hip_four_to_two_second_updates(self):
        # Exact native commits in extended visibility; original-visibility
        # observations matched the same authored positions.
        # Aircraft 0x751e7; SHA-256 of the native binary:
        # 9f004e3aa409bf37c9f71349d8be7e135115e6da8cdca2f6375070e3e9bdea22.
        # Times are virtual microseconds; coordinates retain original Q8 values.
        samples = [
            (52610733, 20207726, 328171, 31056383),
            (56610733, 20074606, 328171, 31053567),
            (60610733, 19941486, 328171, 31050751),
            (64610733, 19808366, 328171, 31047935),
            (68610733, 19675246, 328171, 31045119),
            (72594066, 19542894, 329707, 31042303),
            (74594066, 19476334, 330475, 31040767),
            (76594066, 19409774, 331243, 31039231),
            (78594066, 19343214, 332011, 31037695),
            (80594066, 19276654, 332779, 31036159),
            (82594066, 19210094, 333547, 31034623),
            (84594066, 19143534, 334315, 31033087),
        ]
        times = [(s[0] - samples[0][0]) / 1e6 for s in samples]
        points = [tuple(v / 256 for v in s[1:]) for s in samples]
        frames = sorted(set(times + [i / 60 for i in range(math.ceil((times[-1] + 4) * 60) + 1)]))
        index = 0
        prior = None
        largest_step = 0
        moving_holds = 0
        for t in frames:
            if index + 1 < len(times) and t >= times[index + 1]:
                before = self.visual(t, points[index])
                index += 1
                after = self.visual(t, points[index])
                self.assertLess(math.dist(before, after), 1e-8, "sample arrival jumped")
            shown = self.visual(t, points[index])
            if prior:
                step = math.dist(prior[1], shown)
                largest_step = max(largest_step, step)
                if index == prior[2] and step > .001:
                    moving_holds += 1
            prior = (t, shown, index)
        native_step = max(math.dist(a, b) for a, b in zip(points, points[1:]))
        self.assertGreater(native_step, 500)
        self.assertLess(largest_step, 5)
        self.assertGreater(moving_holds, 1500)
        self.assertEqual(self.offset(frames[-1], points[-1]), (0, 0, 0))
        print(f"Native Hip replay: largest source step {native_step:.3f} feet; "
              f"visual frame step {largest_step:.3f}; moving held frames {moving_holds}")


if __name__ == "__main__":
    unittest.main()
