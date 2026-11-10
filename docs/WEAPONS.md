# Weapons and TOW guidance

The default AH-64 loadout in Libya's Alpha Strike mission has a 30 mm cannon,
two Hellfire hardpoints and Sidewinders. You can change the loadout on the
weapon configuration page of the briefing: use the arrows to select a
hardpoint, then Space to change its weapon. For example, changing the selected
inner wing once replaces its eight Hellfires with eight TOWs.

| Input | Action |
| --- | --- |
| `[` / `]` | Previous / next weapon hardpoint |
| Click right stick | Next weapon hardpoint |
| Enter / controller A | Request the next target |
| Space / RT | Fire the selected hardpoint |
| Arrows / left stick during TOW flight | Steer the TOW |
| Release and press fire again during TOW flight | Detonate the TOW |

Check the selected weapon name and ammunition in the fire-control display.
The two Hellfire hardpoints are separate selections, so pressing next weapon
once more can still show Hellfire.

TOW uses the original game's manual guidance. Launch it, release fire, and
use the stick or arrows to guide it toward the intended target. A TOW can
launch without an acquired target; launching does not establish that it is
tracking one. During manual guidance the original game assigns the shared
flight-stick axes to the missile and clears those axes for helicopter physics.
The helicopter continues its existing flight. A deliberate second fire press
detonates the missile.

The controller bridge now supplies analog input before that original axis
handoff. Holding RT launches one TOW and suppresses automatic repeat for that
press. Releasing RT resets the suppression, so pressing it again retains the
original detonation action. Automatic fire repeat remains available for the
cannon and other weapons. These are input fixes and require no INI option.

Earlier native builds supplied the controller axes inside the helicopter's
temporary integration interval, leaving them unavailable to TOW guidance.
Their held-fire repeat could also detonate a TOW about 350 ms after launch,
then continue consuming ammunition. A brief keyboard Space press followed by
arrow steering provides a useful comparison when investigating an older build.

The focused checks use the original menu to equip TOWs on the default AH-64
mission, then verify ammunition use, projectile existence and movement,
keyboard steering, deliberate detonation and relaunch. A separate SDL virtual
controller regression checks held fire and the original axis handoff:

```sh
python3 tests/tow_smoke.py --binary build/cmake/lhx-native --data-dir bin/dos
python3 tests/tow_smoke.py --binary build/cmake/lhx-native --data-dir bin/dos --renderer native
python3 tests/tow_gamepad_probe.py --require-fixed
```

These checks run muted with dummy SDL and isolated saves. They establish native
behavior for the supported Hit Squad game files. They do not establish a DOS
runtime comparison, successful target damage or mission completion.
