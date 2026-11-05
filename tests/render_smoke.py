"""Exercise native world rendering through briefings, takeoff and view changes."""

import argparse
import json
from pathlib import Path
import subprocess
import tempfile

from flight_smoke import sequence, state


def native_blocks(rgb, width, scale):
    """Count world tiles containing finer detail than a scaled DOS pixel."""
    count = 0
    for y in range(12, 90):
        for x in range(4, 316):
            start = ((y * scale) * width + x * scale) * 3
            first = rgb[start:start + 3]
            if any(rgb[start + (dy * width + dx) * 3:
                       start + (dy * width + dx) * 3 + 3] != first
                   for dy in range(scale) for dx in range(scale)):
                count += 1
    return count


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lhx-native-render-") as directory:
        work = Path(directory)
        image, ram = work / "frame.ppm", work / "state.ram"
        report = work / "render.json"
        profile = work / "controls.ini"
        # Deliberately conflict with the CLI to verify its precedence.
        profile.write_text("[rendering]\nrenderer = original\nrender_scale = 1\n"
                           "visibility = original\nshow_fps = false\n")
        keys = sequence(True) + ",45:F1,48:F5,51:F1"
        result = subprocess.run([
            str(args.binary.resolve()), "--data-dir", str(args.data_dir.resolve()),
            "--save-dir", str(work / "saves"), "--headless", "--mute",
            "--renderer", "native", "--render-scale", "3", "--visibility", "extended",
            "--seconds", "54",
            "--keys", keys, "--screenshot", str(image), "--dump-memory", str(ram),
            "--render-stats", str(report)],
            cwd=work, capture_output=True, text=True, timeout=75)
        assert result.returncode == 0, result.stderr
        magic, dimensions, maximum, rgb = image.read_bytes().split(b"\n", 3)
        assert (magic, dimensions, maximum) == (b"P6", b"960 600", b"255")
        assert len(rgb) == 960 * 600 * 3
        finer = native_blocks(rgb, 960, 3)
        assert finer > 40, ("World was only enlarged from DOS pixels", finer)
        aircraft = state(ram.read_bytes())
        assert aircraft["power"] == aircraft["maximum"] > 0, aircraft
        assert aircraft["position"][1] > 2560, aircraft

        # The attitude horizon is cached independently of the main view. Its
        # blue upper half and brown lower half must survive F1/F5/F1 changes.
        def colors(x0, y0, x1, y1):
            for y in range(y0 * 3, y1 * 3):
                for x in range(x0 * 3, x1 * 3):
                    at = (y * 960 + x) * 3
                    yield tuple(rgb[at:at + 3])

        sky = sum(b > r + 30 and b > g + 30
                  for r, g, b in colors(70, 162, 88, 171))
        ground = sum(r > g + 30 and g > b + 30
                     for r, g, b in colors(70, 180, 88, 188))
        assert sky > 100 and ground > 100, ("Lost attitude horizon", sky, ground)
        extended = json.loads(report.read_text())["statistics"]
        original_ram, original_report = work / "original.ram", work / "original.json"
        original_image = work / "original.ppm"
        original = subprocess.run([
            str(args.binary.resolve()), "--data-dir", str(args.data_dir.resolve()),
            "--save-dir", str(work / "original-saves"), "--headless", "--mute",
            "--renderer", "native", "--render-scale", "3",
            "--visibility", "original", "--seconds", "54",
            "--keys", keys, "--dump-memory", str(original_ram),
            "--screenshot", str(original_image),
            "--render-stats", str(original_report)],
            cwd=work, capture_output=True, text=True, timeout=75)
        assert original.returncode == 0, original.stderr
        assert state(original_ram.read_bytes()) == aircraft
        baseline = json.loads(original_report.read_text())["statistics"]
        old = baseline["geometry_per_draw"]["meshes"]["last"]
        new = extended["geometry_per_draw"]["meshes"]["last"]
        assert new > old * 2, ("Extended discovery did not add distant models", old, new)
        assert extended["camera_distance_common_units"]["last_max_object_origin"] > \
               baseline["camera_distance_common_units"]["last_max_object_origin"] * 2
        assert baseline["host_captures"] == 0 < extended["host_captures"]
        assert baseline["original_visibility_per_main_view"] == \
               extended["original_visibility_per_main_view"]
        for visibility, expected_image, expected_ram in (
                ("extended", image, ram), ("original", original_image, original_ram)):
            # INI applies with no controller and no CLI rendering selection.
            profile.write_text("[rendering]\nrenderer = native\nrender_scale = 3\n"
                               f"visibility = {visibility}\nshow_fps = false\n")
            ini_image, ini_ram = work / "ini.ppm", work / "ini.ram"
            configured = subprocess.run([
                str(args.binary.resolve()), "--data-dir", str(args.data_dir.resolve()),
                "--save-dir", str(work / f"ini-{visibility}-saves"),
                "--headless", "--mute", "--no-controller", "--seconds", "54",
                "--keys", keys, "--screenshot", str(ini_image),
                "--dump-memory", str(ini_ram)],
                cwd=work, capture_output=True, text=True, timeout=75)
            assert configured.returncode == 0, configured.stderr
            assert ini_image.read_bytes() == expected_image.read_bytes(), visibility
            assert ini_ram.read_bytes() == expected_ram.read_bytes(), visibility
        # Exercise the optional path without depending on untracked photo assets.
        terrain = work / "terrain"
        terrain.mkdir()
        texture = (b"LHX texture 1\n16 16\n-1000000 1000000 -1000000 1000000\n" +
                   bytes(channel for y in range(16) for x in range(16)
                         for channel in (x * 16, y * 16, 64)))
        for theatre in ("gulf", "asia", "europe"):
            for label in ("near", "far"):
                (terrain / f"{theatre}_{label}.lht").write_bytes(texture)
        profile.write_text("[rendering]\nrenderer = native\nrender_scale = 3\n"
                           "visibility = extended\nlighting = true\nrunway_textures = true\n"
                           "antialiasing = true\nsphere_depth = true\n"
                           f"terrain_textures = true\nterrain_dir = {terrain}\n")
        enhanced_image, enhanced_ram = work / "enhanced.ppm", work / "enhanced.ram"
        enhanced = subprocess.run([
            str(args.binary.resolve()), "--data-dir", str(args.data_dir.resolve()),
            "--save-dir", str(work / "enhanced-saves"), "--headless", "--mute",
            "--no-controller", "--seconds", "54", "--keys", keys,
            "--screenshot", str(enhanced_image), "--dump-memory", str(enhanced_ram)],
            cwd=work, capture_output=True, text=True, timeout=75)
        assert enhanced.returncode == 0, enhanced.stderr
        assert "flat satellite ground loaded" in enhanced.stderr, enhanced.stderr
        original_memory = ram.read_bytes()
        assert enhanced_ram.read_bytes() == original_memory
        enhanced_rgb = enhanced_image.read_bytes().split(b"\n", 3)[3]
        assert enhanced_rgb != rgb
        for y in range(600):
            for x in range(960):
                if original_memory[0xa0000 + y // 3 * 320 + x // 3] != 255:
                    at = (y * 960 + x) * 3
                    assert enhanced_rgb[at:at + 3] == rgb[at:at + 3], ("Changed overlay", x, y)
        print(f"Native takeoff and camera changes verified; {finer} fine-detail tiles; "
              f"extended meshes {old} -> {new}, original aircraft state preserved; "
              "INI enabled/disabled replays match CLI images and complete RAM; "
              "lighting/textures preserve complete RAM and cockpit/HUD pixels")


if __name__ == "__main__":
    main()
