"""Exercise real native rendering and keyboard-driven menu navigation."""

import argparse
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--scenario", choices=("title", "mission-menu"), default="title")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix="lhx-native-boot-") as directory:
        work = Path(directory)
        frame = work / "title.ppm"
        command = [str(args.binary.resolve()), "--data-dir", str(args.data_dir.resolve()),
             "--save-dir", str(work / "saves"), "--headless", "--seconds", "30",
             "--screenshot", str(frame)]
        if args.scenario == "mission-menu":
            command += ["--keys", "2:enter,3:D,3.1:I,3.2:E,3.3:G,3.4:O,4:enter,5:enter"]
        run = subprocess.run(command, cwd=work, capture_output=True, timeout=45)
        if run.returncode:
            raise AssertionError(run.stderr.decode("utf-8", "replace"))
        magic, dimensions, maximum, rgb = frame.read_bytes().split(b"\n", 3)
        assert (magic, dimensions, maximum) == (b"P6", b"320 200", b"255")
        assert len(rgb) == 320 * 200 * 3
        colours = {"red": 0, "green": 0, "blue": 0, "white": 0}
        for r, g, b in zip(rgb[::3], rgb[1::3], rgb[2::3]):
            colours["red"] += r > 150 and g < r * .7 and b < r * .7
            colours["green"] += g > 100 and r < g * .7 and b < g * .7
            colours["blue"] += b > 150 and r < b * .7 and g < b * .7
            colours["white"] += min(r, g, b) > 220
        if args.scenario == "title":
            # White LHX lettering and all three coloured EA shapes must appear.
            # A blank frame or the monochrome disk-error dialog fails.
            assert min(colours.values()) > 100, colours
        else:
            selected = paper = 0
            for y in range(200):
                for x in range(320):
                    i = (y * 320 + x) * 3
                    r, g, b = rgb[i:i + 3]
                    if 30 <= x < 235 and 45 <= y < 55:
                        selected += g > 180 and r < g * .8 and b < g * .8
                    if 30 <= x < 235 and 65 <= y < 180:
                        paper += min(r, g, b) > 210
            # Alpha Strike is highlighted on the original paper mission list.
            assert selected > 1000 and paper > 10000, (selected, paper)
        print(f"Native {args.scenario} passed after 30 virtual seconds:", colours)


if __name__ == "__main__":
    main()
