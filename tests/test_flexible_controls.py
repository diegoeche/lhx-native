"""Multi-device runtime and the real offscreen action/behavior/calibration UI."""
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

@unittest.skipUnless(shutil.which("cc") and shutil.which("pkg-config"), "C / SDL2 required")
class FlexibleControls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.directory = tempfile.TemporaryDirectory(prefix="lhx-flex-")
        cls.tmp = Path(cls.directory.name)
        (cls.tmp / "recomp_all.h").write_text("")
        cls.binary = cls.tmp / "probe"
        flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
        build = subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-Wno-misleading-indentation", "-I", str(cls.tmp), "-I", str(ROOT / "runtime"),
            str(ROOT / "tests/flexible_controls_probe.c"),
            *(str(ROOT / "runtime" / x) for x in ("controller_setup.c", "controller_flex_setup.c",
             "controller_model.c", "controller_profile.c", "controller_input.c", "controls.c", "gamepad.c")),
            *flags, "-o", str(cls.binary)], capture_output=True, text=True)
        if build.returncode:
            raise AssertionError(build.stdout + build.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.directory.cleanup()

    def probe(self, *args):
        run = subprocess.run([str(self.binary), *map(str, args)], capture_output=True, text=True,
            timeout=30, env=os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy"})
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        return run

    def test_virtual_devices_ui_runtime_calibration_and_persistence(self):
        profile = self.tmp / "upgraded.ini"
        original = (b"; KEEP exact old values\r\n[buttons]\r\na = fire ; old mapping\r\n"
                    b"[axes]\r\nlefty = -pitch\r\n[audio]\r\nimproved_sounds = true ; untouched\r\n"
                    b"[rendering]\r\nbackend = gpu\r\nsky_dir = bin/enhanced/sky-subtle\r\n")
        profile.write_bytes(original)
        profile.chmod(0o640)
        run = self.probe("ui", profile, self.tmp)
        saved = profile.read_bytes()
        self.assertTrue(saved.startswith(original))
        self.assertNotIn(b"\n", saved.replace(b"\r\n", b""))
        self.assertEqual(profile.stat().st_mode & 0o777, 0o640)
        self.assertNotIn(b"occurrence", saved)
        self.assertNotIn(b"/dev/input", saved)
        self.probe("roundtrip", profile)
        self.assertEqual(saved, profile.read_bytes())
        evidence = os.environ.get("LHX_FLEX_EVIDENCE")
        if evidence:
            out = Path(evidence);out.mkdir(parents=True, exist_ok=True)
            for image in self.tmp.glob("*.bmp"):
                shutil.copy2(image, out / image.name)
            shutil.copy2(profile, out / "saved-test-profile.ini")
            (out / "probe.log").write_text(run.stdout + run.stderr)

    def test_strict_usb_schema_and_invalid_calibration(self):
        profile = self.tmp / "strict.ini"
        device = ("[usb_controls]\nversion=2\ndevices=1\nbindings=0\n"
                  "[usb_device1]\nguid=00112233445566778899aabbccddeeff\nserial=none\nname=none\nenabled=1\nmapped=0\nsession_only=0\n")
        bad = ["[usb_controls]\n", "[usb_device1]\n", "[usb_unknown]\n", "[usb_controls]\nversion=3\n", "[usb_controls]\nversion=2\ndevices=17\nbindings=0\n",
               device.replace("bindings=0", "bindings=129"), device.replace("mapped=0\n", ""),
               device + "enabled=1\n", device.replace("guid=00112233445566778899aabbccddeeff", "guid=bad"),
               device + "[usb_calibration1]\naxis33=centered,-32768,0,32767,0,0\n",
               device.replace("devices=1", "devices=0"),
               device.replace("bindings=0", "bindings=1") + "[usb_controls]\nbinding1=device2,axis1,roll,analog,1,50,16000,v2\n",
               device.replace("bindings=0", "bindings=1") + "[usb_controls]\nbinding1=device1,button1,roll,direct,1,50,16000,v2\n"]
        for calibration in ("centered,0,0,0,0,0", "centered,-32768,-32768,32767,0,0",
                            "centered,-32768,0,32767,2,0", "lever,0,0,0,0,0",
                            "centered,2147483647,-2147483648,32767,0,0",
                            "lever,-2147483648,0,2147483647,0,0",
                            "lever,-32768,99999999999999999,32767,0,0",
                            "centered,-100,0,100,0,0", "centered,-4096,0,4096,0,6000"):
            bad.append(device + "[usb_calibration1]\naxis1=" + calibration + "\n")
        for index, body in enumerate(bad):
            with self.subTest(index=index):
                profile.write_text(body)
                self.probe("bad", profile)
                self.assertEqual(profile.read_text(), body)
        for body in ("", "[joystick_axes]\naxis8=-fire\n", "[usb_controls]\nversion=2\ndevices=0\nbindings=0\n", device):
            profile.write_text(body)
            self.probe("roundtrip", profile)
        self.assertEqual(list(self.tmp.glob("*.tmp.*")), [])

if __name__ == "__main__":
    unittest.main()
