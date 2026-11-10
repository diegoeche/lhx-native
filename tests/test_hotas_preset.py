"""Known-model metadata guards and isolated SDL HOTAS preset UI/runtime."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"), "C / SDL2 required")
class HotasPreset(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if subprocess.run(["pkg-config", "--atleast-version=2.24.0", "sdl2"]).returncode:
            raise unittest.SkipTest("SDL2 virtual devices required")
        cls.directory = tempfile.TemporaryDirectory(prefix="lhx-hotas-")
        cls.tmp = Path(cls.directory.name)
        (cls.tmp / "recomp_all.h").write_text("")
        cls.binary = cls.tmp / "probe"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
        run = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(cls.tmp), "-I", str(ROOT / "runtime"), str(ROOT / "tests/hotas_preset_probe.c"),
            *(str(ROOT / "runtime" / f) for f in ("controller_setup.c", "controller_flex_setup.c",
                "controller_model.c", "controller_profile.c", "controller_input.c", "controls.c", "gamepad.c")),
            *("-Wl,--wrap=" + f for f in ("SDL_GetPlatform", "SDL_JoystickGetVendor", "SDL_JoystickGetProduct",
                "SDL_JoystickGetDeviceVendor", "SDL_JoystickGetDeviceProduct")),
            *flags, "-o", str(cls.binary)], capture_output=True, text=True)
        if run.returncode:
            raise AssertionError(run.stdout + run.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def probe(self, mode):
        profile = self.tmp / (mode + ".ini")
        original = b"; KEEP controls\r\n[buttons]\r\na = fire ; custom mapped layout\r\n[audio]\r\nimproved_sounds=true\r\n[rendering]\r\nbackend=gpu ; untouched\r\n"
        profile.write_bytes(original)
        profile.chmod(0o640)
        out = self.tmp / mode
        out.mkdir()
        run = subprocess.run([str(self.binary), mode, str(profile), str(out)], timeout=30,
            env=os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"}, capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        if mode == "fresh":
            self.assertIn(b"backend=gpu ; untouched\r\n", profile.read_bytes())
            self.assertIn(b"a = fire ; custom mapped layout\r\n", profile.read_bytes())
            self.assertNotIn(b"\n", profile.read_bytes().replace(b"\r\n", b""))
            self.assertEqual(profile.stat().st_mode & 0o777, 0o640)
        else:
            self.assertEqual(profile.read_bytes(), original)
        evidence = os.environ.get("LHX_HOTAS_EVIDENCE")
        if evidence:
            dest = Path(evidence)
            dest.mkdir(parents=True, exist_ok=True)
            for f in out.glob("*.bmp"):
                shutil.copy2(f, dest / f.name)
            (dest / (mode + ".log")).write_text(run.stdout + run.stderr)
            if mode == "fresh":
                shutil.copy2(profile, dest / "saved-test-profile.ini")

    def test_exact_names_and_guards(self):
        self.probe("names")

    def test_initial_offer_preset_and_persistence(self):
        self.probe("fresh")

    def test_custom_conflict_calibration_and_legacy_preservation(self):
        self.probe("custom")

    def test_actual_sdl_virtual_axis_behavior(self):
        self.probe("runtime")

    def test_ambiguity_and_unknown_baseline(self):
        self.probe("identity")


if __name__ == "__main__":
    unittest.main()
