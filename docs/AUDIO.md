# Sound

The supplied `controls.ini` enables procedural stereo audio:

```ini
[audio]
improved_sounds = true
```

Restart the game after changing this option. Set it to `false` for the original
PC-speaker output. Omitting the option also selects the original output.
`--mute` disables either mode.

The enhanced mode adds a rotor/engine pulse that follows the game's engine-rate
control, fuller weapon bursts, softer tones and filtered noise. It mixes the
original driver's four sound slots in stereo, so an effect can remain audible
alongside a higher-priority sound. Short attack/release envelopes and smooth
pitch changes reduce clicks. Stereo width is fixed by voice rather than world
position. No downloaded recordings or additional art pack are needed.

The cannon uses a short crack, a falling low thump and a decaying noise tail.
Repeated impacts follow the original driver's envelope restarts, including
brief transitions that happen between audio callbacks. Extending a running
burst does not add an extra impact. These are synthesized timbre choices,
rather than a recording or an attempt to reproduce a particular real cannon.
Other sound levels and the classic output are unchanged.

## How the original sound reaches the speakers

The supported release supplies `IBMDRIVE.DRV` inside `LHX2.LIB`. The existing
build pipeline checks its exact hash and translates its code into native C.
That original code chooses sound states, advances envelopes and music notes,
and writes PC timer/speaker ports. The classic output turns the selected timer
divisor and speaker gate into a mono square wave at 48 kHz.

The driver maintains four state slots, then reduces their output to a single
speaker tone. Enhanced output observes those slots after the original driver
runs. It copies the resulting sound parameters to the audio thread using
atomics; the audio callback never reads simulation memory, allocates resources
or waits for the simulation thread. All samples are synthesized locally.

The new timbres are creative additions. The original driver continues to select
events, control timing, handle pauses and advance its own state. Flight physics,
weapons and mission logic use the same code in either audio mode. Music-note
states retain their pitches and are handled separately from engine effects.

## Validation

Source tests cover simultaneous tones, stereo mixing, rotor tempo, silence,
release envelopes, buffer continuity, output bounds and supported sample rates.
Cannon checks exercise repeated bursts with brief inactive transitions, command
extensions, low-frequency body, decay and headroom with all four slots active.
The driver tests also check that observation preserves guest memory and that
engine, gunfire, music and stop commands produce the expected sound snapshots.

```sh
python3 -m unittest discover -s tests -p 'test_audio*.py'
```
