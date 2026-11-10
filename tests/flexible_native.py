"""Whole lifted game with a test-only virtual USB fixture, headless and muted.

Caller must hold .local/coordination/canonical-build.lock. Does not mutate
canonical generated code or product main.c. Copies main.c with precisely the
headless controller-disable condition removed, links the actual CMake objects,
and wraps input polling and the existing pre-TOW observer for measurements.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shlex
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tests"))
from flight_smoke import sequence, state

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args();out = args.output.resolve();out.mkdir(parents=True, exist_ok=True)
    build = ROOT / "build/cmake"
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "sdl2"], text=True))
    common = ["cc", "-std=c11", "-O2", "-D_DEFAULT_SOURCE", "-DLHX_HAVE_OPENGL=1", "-I", str(ROOT / "runtime"), "-I", str(build / "generated")]
    fixture = ROOT / "tests/flexible_native_fixture.c"
    subprocess.run([*common, "-DFIXTURE_IDENTITY_MAIN", str(fixture), *flags, "-ldl", "-o", str(out / "identity")], check=True)
    env = os.environ | {"SDL_VIDEODRIVER": "dummy", "SDL_AUDIODRIVER": "dummy", "SDL_JOYSTICK_ALLOW_BACKGROUND_EVENTS": "1",
        "SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT": "0xfeed/0xd100"}
    identity = subprocess.check_output([str(out / "identity")], env=env, text=True).strip()
    profile = ("[rendering]\nrenderer=original\nrender_scale=1\n[usb_controls]\nversion=2\ndevices=1\nbindings=3\n"
        "binding1=device1,axis1,throttle,direct,1,50,16000,v2\n"
        "binding2=device1,button1,throttle,preset,1,37,16000,v2\n"
        "binding3=device1,button3,throttle,decrease,1,50,16000,v2\n"
        f"[usb_device1]\nguid={identity}\nserial=none\nname=none\nenabled=1\nmapped=0\nsession_only=0\n")
    (out / "controls.ini").write_text(profile)
    main_source = (ROOT / "runtime/main.c").read_text()
    old = "  if (headless)\n    controller_enabled = 0;"
    assert main_source.count(old) == 1
    (out / "fixture-main.c").write_text(main_source.replace(old,"  /* Test fixture: keep virtual controllers enabled with headless video. */"))
    subprocess.run([*common, "-c", str(out / "fixture-main.c"), *flags, "-o", str(out / "fixture-main.o")], check=True)
    subprocess.run([*common, "-c", str(fixture), *flags, "-o", str(out / "fixture.o")], check=True)
    command = shlex.split((build / "CMakeFiles/lhx-native.dir/link.txt").read_text())
    # Link actual build objects, replacing only the product entry-point object.
    objects = [str(build / x) for x in command if x.endswith(".o") and not x.endswith("runtime/main.c.o")]
    binary = out / "lhx-flex-native"
    subprocess.run(["cc", *objects, str(out / "fixture-main.o"), str(out / "fixture.o"),
        "-Wl,--wrap=lhx_gamepad_poll", "-Wl,--wrap=lhx_gamepad_apply", *flags, "-lGL", "-lm", "-ldl", "-o", str(binary)], check=True)
    memory = out / "state.ram"
    run = subprocess.run([str(binary), "--data-dir", str(ROOT / "bin/dos"), "--save-dir", str(out / "saves"),
        "--controls", str(out / "controls.ini"), "--headless", "--mute", "--seconds", "60", "--keys", sequence(False) + ",52:5",
        "--dump-memory", str(memory)], cwd=out, env=env, capture_output=True, text=True, timeout=70)
    (out / "run.log").write_text(run.stdout + run.stderr)
    run.check_returncode()
    assert "[flex-native] observed=9/9" in run.stderr, run.stderr[-6000:]
    final = state(memory.read_bytes());assert final["power"] == final["maximum"] == 25600, final
    result = {"stages":9, "final":final,"product_main_sha256":hashlib.sha256(main_source.encode()).hexdigest(),
        "fixture_main_sha256":hashlib.sha256((out / "fixture-main.c").read_bytes()).hexdigest(),
        "scope":"Whole native lifted game, original flight and keyboard routines, real virtual SDL native input. Test-only headless controller enable; wrappers observe registers and memory, do not write physics."}
    (out / "result.json").write_text(json.dumps(result, indent=2) + "\n");print(json.dumps(result, indent=2))

if __name__ == "__main__":
    main()
