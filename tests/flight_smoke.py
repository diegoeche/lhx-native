"""Prove input-driven takeoff in the original Free Flight scenario."""

import argparse
from pathlib import Path
import re
import struct
import subprocess
import tempfile


def sequence(powered):
    events = [(2, "enter"), (3, "D"), (3.1, "I"), (3.2, "E"),
              (3.3, "G"), (3.4, "O"), (4, "enter"), (5, "enter")]
    events += [(6 + i * .2, "Down") for i in range(11)]
    events += [(8.5, "enter")]
    events += [(9.5 + i * 2.5, "enter") for i in range(13)]
    events += [(41, "F5")]
    if powered:
        events += [(42, "5")]
    return ",".join(f"{at:g}:{key}" for at, key in events)


def state(ram):
    # Addresses belong to the exact module hash enforced by tools/lift.py.
    main = 0x38AB0

    def pointer(at):
        offset, segment = struct.unpack_from("<HH", ram, at)
        address = segment * 16 + offset
        assert address + 0x280 <= len(ram), (offset, segment)
        return address

    player = pointer(main + 0x120)
    aircraft = pointer(main + 0xF626)
    return {
        "position": struct.unpack_from("<3i", ram, player + 6),
        "power": struct.unpack_from("<i", ram, aircraft + 0x21C)[0],
        "maximum": struct.unpack_from("<h", ram, aircraft + 0x240)[0] << 8,
    }


def run(binary, data, directory, powered):
    ram_path = directory / "state.ram"
    result = subprocess.run([
        str(binary), "--data-dir", str(data), "--save-dir", str(directory / "saves"),
        "--headless", "--mute", "--seconds", "60", "--keys", sequence(powered),
        "--dump-memory", str(ram_path)], cwd=directory,
        capture_output=True, text=True, timeout=45)
    assert result.returncode == 0, result.stderr
    elapsed = re.search(r"\[runtime\] elapsed=([\d.]+)", result.stderr)
    assert elapsed and float(elapsed[1]) >= 60, result.stderr
    pacing = re.search(r"\[pacing\] target=(\d+) frames=(\d+) span=([\d.]+)",
                       result.stderr)
    assert pacing, result.stderr
    target, frames, span = int(pacing[1]), int(pacing[2]), float(pacing[3])
    assert target == 60 and frames > 100, result.stderr
    # Count complete intervals: the first frame has no preceding deadline.
    assert span >= ((frames - 1) * 1_000_000 // target) / 1_000_000, result.stderr
    ram = ram_path.read_bytes()
    assert len(ram) == 1_114_112, len(ram)
    return {**state(ram), "frames": frames, "frame_span": span}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    args = parser.parse_args()
    binary, data = args.binary.resolve(), args.data_dir.resolve()
    with tempfile.TemporaryDirectory(prefix="lhx-native-flight-") as work:
        root = Path(work)
        ground_dir, air_dir = root / "ground", root / "air"
        ground_dir.mkdir()
        air_dir.mkdir()
        ground = run(binary, data, ground_dir, False)
        air = run(binary, data, air_dir, True)
        assert ground["power"] == 0, ground
        assert air["power"] == air["maximum"] and air["power"] > 0, air
        assert air["position"][1] > ground["position"][1] + 2560, (ground, air)
        print("Ground control:", ground)
        print("Collective 5:", air)
        print("Native takeoff verified in two complete 60-second runs")


if __name__ == "__main__":
    main()
