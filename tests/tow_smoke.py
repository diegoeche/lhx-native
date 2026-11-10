"""Check the original TOW loadout, launch, steering and explicit detonation.

Uses an existing native binary; never builds the shared tree. All inputs go
through --keys and the original menus. RAM writes are not used. This is a
native replay, not a DOS emulator comparison or a target-hit test.
"""

import argparse
import hashlib
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile

MAIN = 0x38AB0
MODULE_SHA256 = "4d30628da15b38eb93cad06420d255574db7283cfec4292f9bfbc8b9d4369f9e"


def sequence(tow=True):
    events = [(2, "enter"), (3, "D"), (3.1, "I"), (3.2, "E"),
              (3.3, "G"), (3.4, "O"), (4, "enter"), (5, "enter")]
    events += [(6 + i * 2.5, "enter") for i in range(13)]
    # Briefing page 3: inner wing is selected; Space changes Hellfire to TOW.
    if tow:
        events += [(12, "Space")]
    events += [(36.2, "5"), (37, "]")]
    return ",".join(f"{at:g}:{key}" for at, key in sorted(events))


def state(ram):
    assert len(ram) == 1_114_112, len(ram)

    def word(at):
        return struct.unpack_from("<H", ram, at)[0]

    def pointer(at):
        address = word(at) + word(at + 2) * 16
        assert 0 < address < len(ram) - 0x350, hex(address)
        return address

    scene = word(MAIN + 0xEA) * 16
    player = pointer(MAIN + 0x120)
    weapon = word(MAIN + 0xF35A)
    projectiles = []
    for i in range(10):
        slot = 0xC579 - i * 23
        at = MAIN + slot
        if not word(at):
            continue
        entity = scene + word(at + 2)
        assert entity + 24 <= len(ram), hex(entity)
        projectiles.append({
            "slot": slot, "weapon": word(at), "owner": word(at + 6),
            "target": word(at + 8),
            "position": struct.unpack_from("<3i", ram, entity + 6),
            "orientation": struct.unpack_from("<3h", ram, entity + 18),
            "speed": struct.unpack_from("<i", ram, at + 12)[0],
        })
    return {
        "aircraft": word(MAIN + 0x102),
        "selected": word(MAIN + 0xF368), "weapon": weapon,
        "weapon_flags": ram[MAIN + weapon + 0x2A],
        "ammo": struct.unpack_from("<4H", ram, MAIN + 0xF36A),
        "manual": ram[MAIN + 0xD855], "target": word(MAIN + 0x11C),
        "player_offset": word(MAIN + 0x120),
        "player_position": struct.unpack_from("<3i", ram, player + 6),
        "player_orientation": struct.unpack_from("<3h", ram, player + 18),
        "projectiles": projectiles,
    }


def run(binary, data, directory, name, seconds, extra="", tow=True,
        renderer="original"):
    work = directory / name
    work.mkdir()
    command = [str(binary), "--data-dir", str(data), "--save-dir", str(work / "saves"),
               "--headless", "--mute", "--no-controller", "--backend", "cpu",
               "--renderer", renderer, "--render-scale", "1", "--seconds", str(seconds),
               "--keys", sequence(tow) + extra, "--dump-memory", str(work / "state.ram"),
               "--screenshot", str(work / "frame.ppm")]
    (work / "command.json").write_text(json.dumps(command, indent=2) + "\n")
    result = subprocess.run(command, cwd=work, capture_output=True, text=True,
                            env=os.environ | {"SDL_VIDEODRIVER": "dummy",
                                              "SDL_AUDIODRIVER": "dummy"}, timeout=60)
    (work / "stderr.txt").write_text(result.stderr)
    assert result.returncode == 0, (name, result.stderr)
    return state((work / "state.ram").read_bytes())


def verify(binary, data, directory, renderer):
    cases = [
        ("before", 41.9, "", True),
        ("launch", 42.1, ",42:Space", True),
        ("motion", 45, ",42:Space", True),
        ("steer", 45, ",42:Space,42.2:Left,42.4:Left,42.6:Left", True),
        ("detonate", 43, ",42:Space,42.4:Space", True),
        ("relaunch", 43, ",42:Space,42.4:Space,42.8:Space", True),
        ("hellfire", 45, ",42:Space", False),
    ]
    results = {name: run(binary, data, directory, name, seconds, extra, tow, renderer)
               for name, seconds, extra, tow in cases}
    before, launch, motion, steer, detonate, relaunch, hellfire = (
        results[name] for name, *_ in cases)
    assert before["aircraft"] == 0 and before["selected"] == 1, before
    assert before["weapon"] == 0x141C and before["weapon_flags"] == 0x13, before
    assert before["ammo"] == (1200, 8, 8, 2) and not before["projectiles"], before
    for result in (launch, motion, steer):
        assert result["ammo"] == (1200, 7, 8, 2), result
        assert result["manual"] == 1 and len(result["projectiles"]) == 1, result
        projectile = result["projectiles"][0]
        assert projectile["weapon"] == 0x141C, result
        assert projectile["owner"] == result["player_offset"], result
    assert motion["projectiles"][0]["position"] != launch["projectiles"][0]["position"]
    assert motion["projectiles"][0]["speed"] > launch["projectiles"][0]["speed"]
    assert steer["projectiles"][0]["orientation"] != motion["projectiles"][0]["orientation"]
    assert steer["player_orientation"] == motion["player_orientation"] == (0, 0, 0)
    # A second deliberate press detonates, and a later third press launches again.
    assert detonate["ammo"] == (1200, 7, 8, 2) and not detonate["projectiles"], detonate
    assert relaunch["ammo"] == (1200, 6, 8, 2) and len(relaunch["projectiles"]) == 1, relaunch
    assert hellfire["weapon"] == 0x1448 and hellfire["manual"] == 0, hellfire
    assert hellfire["ammo"] == (1200, 7, 8, 2) and len(hellfire["projectiles"]) == 1, hellfire
    report = {"binary": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
              "renderer": renderer, "results": results,
              "limits": ["Native AOT replay; no DOS execution comparison.",
                         "Keyboard guidance and explicit detonation; no physical controller.",
                         "Target acquisition and damage/hit are not asserted."]}
    (directory / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print("TOW menu loadout, ammunition, launch, movement, keyboard guidance, detonation and relaunch passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--data-dir", required=True, type=Path)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--renderer", choices=("original", "native"), default="original")
    args = parser.parse_args()
    data, binary = args.data_dir.resolve(), args.binary.resolve()
    raw = (data / "LHX.unpacked.EXE").read_bytes()
    header = struct.unpack_from("<H", raw, 8)[0] * 16
    assert hashlib.sha256(raw[header:]).hexdigest() == MODULE_SHA256, "Unsupported DOS module"
    if args.output_dir:
        directory = args.output_dir.resolve()
        directory.mkdir(parents=True, exist_ok=False)
        verify(binary, data, directory, args.renderer)
    else:
        with tempfile.TemporaryDirectory(prefix="lhx-tow-") as work:
            verify(binary, data, Path(work), args.renderer)


if __name__ == "__main__":
    main()
