"""Native setup event paths, SDL virtual controllers and atomic INI persistence."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"), "C/SDL2 required")
class ControllerSetup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if subprocess.run(["pkg-config", "--atleast-version=2.24.0", "sdl2"]).returncode:
            raise unittest.SkipTest("SDL2 virtual joystick support required")
        cls.directory = tempfile.TemporaryDirectory(prefix="lhx-setup-")
        cls.tmp = Path(cls.directory.name)
        (cls.tmp / "recomp_all.h").write_text("")
        cls.binary = cls.tmp / "probe"
        flags = subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True)
        built = subprocess.run([
            "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(cls.tmp), "-I", str(ROOT / "runtime"),
            str(ROOT / "tests/controller_setup_probe.c"),
            *(str(ROOT / "runtime" / name) for name in
              ("controller_setup.c", "controls.c", "gamepad.c", "controller_input.c", "controller_profile.c", "controller_model.c", "controller_flex_setup.c")),
            *shlex.split(flags), "-o", str(cls.binary)], capture_output=True, text=True)
        if built.returncode:
            raise AssertionError(built.stdout + built.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def run_probe(self, *args):
        run = subprocess.run([str(self.binary), *map(str, args)], timeout=30,
                             env=os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"},
                             capture_output=True, text=True)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run

    def test_ui_and_runtime(self):
        profile = self.tmp / "ui.ini"
        profile.write_text("# Pilot controls\n[audio]\nimproved_sounds = true\n"
                           "[rendering]\nbackend = gpu ; keep this\nsky_dir = bin/enhanced/sky-subtle\n")
        screenshot = self.tmp / "setup.bmp"
        run = self.run_probe("ui", profile, screenshot)
        self.assertIn("runtime actions and hotplug passed", run.stdout)
        self.assertGreater(Path(str(screenshot) + "-actions.bmp").stat().st_size, 1200 * 840 * 3)
        # Optional caller-selected evidence directory; never alter a live profile.
        evidence = os.environ.get("LHX_SETUP_EVIDENCE")
        if evidence:
            Path(evidence).mkdir(parents=True, exist_ok=True)
            for state in ("actions", "listening", "conflict", "feedback"):
                shutil.copy2(Path(str(screenshot) + "-" + state + ".bmp"), Path(evidence) / (state + ".bmp"))
            shutil.copy2(profile, Path(evidence) / "saved-test-profile.ini")

    def test_unmapped_native_joystick(self):
        profile = self.tmp / "native.ini"
        unchanged = ("# Pilot native and F310 coexist\n[audio]\nimproved_sounds = true ; retain\n"
                     "[rendering]\nbackend = gpu ; keep this\nsky_dir = bin/enhanced/sky-subtle\n")
        profile.write_text(unchanged)
        screenshot = self.tmp / "native.bmp"
        run = self.run_probe("native", profile, screenshot)
        self.assertIn("Unmapped native joystick", run.stdout)
        self.assertIn("[supported native joystick] 8 axes 15 buttons 1 hats", run.stdout)
        self.assertIn("[excluded: needs at least 2 axes and 2 buttons]", run.stdout)
        self.assertTrue(profile.read_text().startswith(unchanged))
        self.assertIn("axis3 = collective", profile.read_text())
        self.assertIn("axis7 = -fire", profile.read_text())
        self.assertIn("input_mode = joystick", profile.read_text())
        self.assertNotIn("/dev/input", profile.read_text())
        evidence = os.environ.get("LHX_SETUP_EVIDENCE")
        if evidence:
            Path(evidence).mkdir(parents=True, exist_ok=True)
            for state in ("actions", "listening", "live", "buttons", "hat", "conflict", "disconnected"):
                shutil.copy2(Path(str(screenshot) + "-native-" + state + ".bmp"),
                             Path(evidence) / ("native-" + state + ".bmp"))
            shutil.copy2(profile, Path(evidence) / "saved-native-test-profile.ini")
            (Path(evidence) / "native-probe-output.txt").write_text(run.stdout + run.stderr)

    def test_entry_and_save_preserve_multi_bindings(self):
        profile = self.tmp / "multi.ini"
        profile.write_text("# inherited multi-input layout\n[buttons]\na=fire\nx=fire\n"
                           "[axes]\nleftx=roll\nrightx=roll\nlefttrigger=fire\nrighttrigger=fire\n")
        self.run_probe("preserve", profile)

    def test_save_duplicate_sections_comments_and_missing_file(self):
        profile = self.tmp / "roundtrip.ini"
        original = (b"; KEEP HEADER\r\n[BuTtOnS] ; first\r\n  A  = confirm  ; first action\r\n"
                    b"[audio]\r\nimproved_sounds = TRUE ; audio stays\r\n"
                    b"[rendering]\r\nphoto_skies = true\r\nsky_dir = bin/enhanced/sky-subtle ; approved\r\n"
                    b"backend = gpu\r\n[buttons]\r\na = hover # duplicate\r\n"
                    b"[axes]\r\nlefty = pitch ; invert\r\n"
                    b"[settings]\r\ndeadzone = 6000\r\n[settings]\r\ndeadzone = 8000\r\n# last comment")
        profile.write_bytes(original)
        profile.chmod(0o640)
        self.run_probe("save", profile)
        saved = profile.read_bytes()
        self.assertIn(b"  A  = fire  ; first action\r\n", saved)
        self.assertIn(b"a = fire # duplicate\r\n", saved)
        self.assertIn(b"lefty = -pitch ; invert\r\n", saved)
        self.assertEqual(saved.count(b"deadzone = 7500\r\n"), 2)
        preserved = original[original.index(b"[audio]"):original.index(b"[buttons]", original.index(b"[audio]"))]
        self.assertIn(preserved, saved)
        self.assertIn(b"# last comment\r\n", saved)
        self.assertEqual(profile.stat().st_mode & 0o777, 0o640)
        self.assertNotIn(b"\n", saved.replace(b"\r\n", b""))
        # Saving twice replaces existing keys instead of growing duplicate sections.
        self.run_probe("save", profile)
        self.assertEqual(profile.read_bytes(), saved)
        missing = self.tmp / "new custom profile.ini"
        self.run_probe("save", missing)
        self.assertIn(b"[controller]", missing.read_bytes())
        self.assertEqual(list(self.tmp.glob("*.tmp.*")), [])

    def test_invalid_and_save_failure_leave_original(self):
        profile = self.tmp / "invalid.ini"
        for body in (b"[settings]\ndeadzone=40000\n", b"[controller]\nshow_setup=yes\n",
                     b"[controller]\nguid=bad\n", b"[controller]\nserial=abcd\n",
                     b"[controller]\nguid=00112233445566778899aabbccddeeff\nserial=abc\n",
                     b"[controller]\npath=/dev/input/event30\n",
                     b"[controller]\ninput_mode=unknown\n",
                     b"[joystick_axes]\naxis0=roll\n", b"[joystick_axes]\naxis33=roll\n",
                     b"[joystick_buttons]\nbutton65=fire\n", b"[joystick_buttons]\nhat1_diagonal=fire\n"):
            profile.write_bytes(body)
            self.run_probe("bad-save", profile)
            self.assertEqual(profile.read_bytes(), body)
        self.run_probe("bad-save", self.tmp / "missing-dir/controls.ini")
        target = self.tmp / "real.ini"
        target.write_bytes(b"# no symlink replacement\n")
        link = self.tmp / "link.ini"
        link.symlink_to(target)
        self.run_probe("bad-save", link)
        self.assertTrue(link.is_symlink())
        self.assertEqual(target.read_bytes(), b"# no symlink replacement\n")
        self.assertEqual(list(self.tmp.glob("*.tmp.*")), [])

    def test_real_window_event_loop(self):
        for mode in ("window-cancel", "window-session", "window-save"):
            with self.subTest(mode=mode):
                profile = self.tmp / (mode + ".ini")
                profile.write_text("# Actual window event loop\n")
                self.run_probe(mode, profile)


if __name__ == "__main__":
    unittest.main()
