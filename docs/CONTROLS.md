# Controller

The optional **Multi-USB bindings** editor supports several devices at once and
multiple independent bindings for every action. Older profiles keep the legacy
controller behavior until you explicitly apply a USB draft.


Supported SDL gamepads connect automatically, including the Logitech F310 in
DirectInput mode (reported as **Logitech Dual Action**). Start with `./run.sh`;
the terminal prints the connected controller's name. The D/X switch can stay
in its current position. Use the keyboard to type your pilot name.

| Control | Action |
| --- | --- |
| Left stick | Proportional cyclic: pitch and bank; release to center |
| Right stick left/right | Rudder, taking priority over bank while deflected |
| Right stick up/down | Increase/decrease lift in 10-unit steps |
| D-pad | Menu arrows |
| A | Confirm / next target |
| B | Toggle cockpit / chase view |
| RT | Fire; hold to repeat |
| X | Deploy chaff (C) |
| Y | Deploy flares (F) |
| LT | Original aircraft lift preset (key 3) |
| LB / RB | Lift -/+ in 10-unit steps; hold to repeat |
| Click left stick | Landing gear |
| Click right stick | Next weapon |
| Start | Pause / resume |
| Select / Back | Menu / Escape / back |

The sticks have a center deadzone. Disconnecting the controller or switching
away from the game releases its held keys and centers its flight inputs.
Keyboard controls remain available. `./run.sh --list-controllers` lists devices;
`./run.sh --no-controller` disables controller input. Headless test runs ignore
physical controllers.

Controller bindings share [`controls.ini`](../controls.ini) with audio and rendering options.
Use `./run.sh --configure-controls` to open the optional native controller setup
screen before starting the game. It needs no game artwork or sound. Normal
startup stays unchanged. For a separate profile, use
`./run.sh --configure-controls --controls /path/to/pilot.ini`; setup can create
a missing profile. An existing invalid profile is rejected before the screen opens.

The legacy screen lists **Action -> Current Binding**, starting from your current
INI or defaults. Flight axes, collective, combat, weapons, views and menu actions
are all listed. The range/count above the list shows more actions below; scroll
with the wheel or Page Up/Down. Select a row to see **all current bindings** below
it, including multi-input bindings. Physical button/axis numbers and hat directions
come from SDL's mapping. Without a connected device, SDL channel names are shown.

Click an action's **Current Binding** cell to bind it directly, or select the
label then press **Enter / Bind**. Fire, Original lift preset, Chaff and Flares accept a button,
hat direction or axis-based trigger. Other button actions require a button or hat;
Roll, Pitch, Rudder and Collective +/- require an axis. Operate the desired input
on the chosen controller. A fresh axis movement is captured immediately from the
position sampled when Bind starts; a stationary off-center lever is ignored.
Release buttons/triggers already held before capture, then press again. This is
binding, not range calibration or absolute throttle setup.

Rebinding replaces all previous inputs for the selected action. A confirmation
shows those inputs and every old action that will lose the chosen input. Choose
**Move / Replace** to commit the change to the draft, or **Keep Current Bindings**
to cancel it. Other inputs for displaced actions remain assigned. Analog-role
replacement clears the old axis assignments for that role, so both axes cannot
accidentally remain active. Opening or saving alone preserves multi-input bindings.

**Delete / Clear** unbinds the selected action. **I / Invert Axis** changes its
bound axis polarity; for a stick-bound trigger action, the moved side determines
the initial polarity. SDL trigger channels are positive-only, so inversion keeps
the normal press usable instead of saving a negative-only Fire binding. Buttons
cannot be inverted. **Cancel Bind** or Escape cancels listening; Escape or the
confirmation's cancel button cancels replacement. Editor Cancel and window close
remain available. Capture/replacement also cancel on focus loss or disconnect.

The live panel shows numbered axis values even before binding on native joysticks.
**M** cycles through all axis and button/hat pages and the recent event history.
Every input shows its assigned action or Unbound; unavailable devices show No Signal.
The event page shows recent button/hat down/up events and meaningful axis movement,
each with the physical input and its assigned action. Axis noise is coalesced and
kept separate from button history. Active actions stay highlighted in the list;
monitoring never changes your selection or scroll position. Only the chosen device
feeds monitoring and capture. Controller input never navigates, saves or starts.
Keyboard and mouse always work: Up/Down or Tab select, Enter activates, and the
bottom buttons retain independent navigation.

**S / Device / Settings** opens the secondary controls: device selection, startup
prompt, deadzone, thresholds and Restore Controller Defaults. Use Left/Right or
click a setting's left/right half to adjust it. The device line shows its SDL name
and USB vendor/product ID. For example, the T.Flight Hotas One may appear as
**Generic X-Box pad [044f:b68c]** in Xbox mode, or **Thrustmaster T.Flight
Hotas One [044f:b68d]** in PC mode. Device selection cycles through Automatic and
supported devices. The exact Linux PC-mode HOTAS preset is described below. Native SDL joysticks
with at least two axes and two buttons are eligible without an Xbox mapping.
This conservative capability filter excludes one-axis keyboard media interfaces,
including the observed Keychron pseudo-device; SDL type and Linux joystick tags
alone do not distinguish them. Automatic selection prefers mapped gamepads, then
eligible native joysticks. The native limits are 32 axes, 64 buttons and four hats;
devices exceeding these limits are rejected instead of truncated.
Devices can reconnect while setup is open;
monitoring resumes with a fresh connected status.

- **Save and Start** atomically saves the shared layout and device preference
  in the chosen INI, then uses those edits for the game.
- **Start without Saving** uses the edits for this session and leaves the file
  untouched.
- **Cancel**, Escape, or closing the window exits without starting or writing.

A save error stays visible in setup and leaves the original file intact. Saving
preserves unrelated audio/rendering options, comments and file permissions;
duplicate controller keys are all updated consistently. A temporary file in the
same directory is flushed, validated and renamed into place. Symlink paths are
rejected for saving; select the actual file instead. Restore Controller Defaults
resets bindings, deadzone, thresholds and repeat timings in the draft; Save or
Start without Saving applies that draft.

Mapped gamepad and native joystick layouts are kept separately in the same INI,
in the legacy model. Native bindings start unbound; F310 defaults
remain in `[buttons]` and `[axes]`. Native axes use `[joystick_axes]` keys such as
`axis3 = collective` or `axis7 = -fire`. Native digital inputs use
`[joystick_buttons]`, such as `button15 = gear` and `hat1_up = menu_up`.
Indices in these keys and the UI are **one-based**; the captured PC-mode HOTAS
throttle at zero-based SDL axis 2 is **Axis 3**. Its signed SDL range is
-32768 through 32767. Known-model names appear only for the exact Linux raw
layout below; other devices retain numbered controls.
Raw axes, including axes 5-8, retain signed polarity; only mapped SDL trigger
channels use positive-only trigger rules. Collective +/- remains relative
10-unit stepping in the legacy editor. The Multi-USB editor below adds direct lever tracking.

Selection also saves `[controller] input_mode = joystick` or `gamepad` so a
later SDL mapping database change cannot reinterpret saved raw indices.
`auto` is the default for older profiles. The automatic native choice is pinned
to its GUID and raw mode when setup opens; merely opening does not write the file.
Restore Defaults resets the current mode's bindings (raw defaults are unbound),
while retaining the other mode's layout.
Selecting a device saves its SDL GUID and stable serial when available. Runtime
waits for that device if it is missing, keeping the keyboard available; it does
not fall back to a different controller. Linux `/dev/input/event*` paths are
never saved. Identical units without unique serials (or with serials longer than
120 bytes) cannot be distinguished; choose Automatic to restore the original
first-supported behavior (mapped gamepads preferred). The observed HOTAS PC mode
has no serial, so identical units in that mode share this limitation.
Reconnecting a controller with the same GUID/serial
resumes it without restarting the game.

For an opt-in startup prompt, setup can save **Show setup on startup: Yes**,
which corresponds to `[controller] show_setup = true`. The built-in default is
false. Headless runs ignore that setting; explicitly combining `--headless`
with `--configure-controls` is an error, as is `--list-controllers` with setup.

Edit the file and restart the game; no rebuild is needed. For example, under `[buttons]`, set
`a = fire` and `x = confirm` to put firing on A and confirmation on X.
Use `none` to unbind an input. Available action names are listed in the file.
Keyboard bindings remain the original game's controls.

Under `[axes]`, assign `roll`, `pitch`, `rudder`, or `collective` to change stick
roles; prefix an action with `-` to invert it (`lefty = -pitch`).
The **Collective +/-** axis is relative: deflecting it increases/decreases
power in 10-unit steps; centering it holds the current power. The legacy screen keeps this relative behavior; use Multi-USB bindings for
absolute lever tracking and explicit calibration.
Trigger axes can also use `fire`, `hover`, `chaff`, and `flares`. Countermeasures deploy once
per press, so holding a button does not drain their inventory. Hold-to-fire
sends repeated Space press/release pairs, which the original game accepts even
when it filters keyboard repeat.
Releasing the controller input stops the repeats.
During TOW guidance, automatic fire repeat is suppressed until you release the
trigger. The assigned roll/pitch (cyclic) axes steer the missile; releasing and pressing fire again
detonates it. See [weapons and TOW guidance](WEAPONS.md) for loadout and aiming.

The `[settings]` section controls deadzones, thresholds and repeat timing.
After an initial 350 ms delay, throttle repeats every 200 ms and fire every
100 ms by default. Opposite throttle inputs cancel while both are held.
Use `./run.sh --controls PATH` for another profile. Missing default files use
the built-in layout; an invalid profile prints its filename and line number
and stops before starting the game.


## HOTAS One with TFRP pedals

On Linux, connect the PC-mode **T.Flight Hotas One [044f:b68d]** with its
eight axes, 15 buttons and one hat, then run `./run.sh --configure-controls`.
Press **H** or click **HOTAS One + pedals** in the initial header. The same
entry is available in the Multi-USB editor. This explicitly enables the actual
HOTAS identity and prepares a draft with four flight bindings:

| Physical control | One-based axis | Preset action |
| --- | --- | --- |
| Stick left/right | 1 | Roll |
| Stick forward/back | 2 | Pitch |
| Throttle lever | 3 | Direct throttle / lift 0–100% |
| Pedal left/right slide | 8 | Rudder / yaw |

The live monitors, events and binding details also name **Axis 4 Right toe
brake**, **Axis 5 Left toe brake**, **Axis 6 Stick twist** and **Axis 7 Throttle
rocker**. These four controls receive no new bindings. Labels require the exact
VID/PID, Linux, native mode and capabilities; mapped layouts, missing devices
and other models keep generic numbers. The physical names come from the
[exact-model Thrustmaster manual](https://ts.thrustmaster.com/download/accessories/Manuals/TFHOne/T-Flight_HO_Manual.pdf)
(English page 7/12), matched to Linux raw SDL axis ordering. Windows/report
ordering differs.

The TFRP connects through the HOTAS's RJ12 port. Its three axes are already
reserved in the eight-axis USB device, even before pedals are plugged in.
For independent pedals, select **5/8 mode: GUIDE white LED off**; the GUIDE
button toggles this hardware mode (manual English page 8/12). Setup displays a
reminder and never changes the hardware mode. Eight visible axes alone do not
establish the current mode or pedal health.

In **4/6 coupled mode**, pedals and twist share **Axis 6 / RZ**. A user-confirmed
pedal sweep exercised its full range while Axis 8 stayed centered. If this occurs,
select **Rudder → E: Edit → N: Change physical control** and move the actual pedals
to bind the measured channel, or select independent **5/8 mode** and verify Axis 8
responds. Independent Axis 8 pedal response and pedal orientation remain untested.

The preset fills **unbound actions and unused controls only**. Existing bindings
on any device and all saved calibration take precedence. A legacy selected
device's effective bindings are imported, including its relative lift behavior
and custom twist rudder if present; those assignments are retained. The summary
reports each added, preserved or conflicting flight assignment. **B / PgDn**
shows the individual button results. Reapplying the preset creates no duplicate
bindings or device records. Identical units require an explicit occurrence
selection / reassignment in Devices. A mapped HOTAS profile must be reviewed in
Devices before applying a native preset.

Numbered button defaults are **1 Fire, 2 Confirm/target, 3 Next weapon,
4 Previous weapon, 5 Chaff, 6 Flares, 7 Gear, 8 Toggle view, 9 Pause,
10 Back, 11 Cockpit, 12 Chase**. Hat directions navigate menus. These are host
defaults, not physical button-name claims. Buttons **13–15** receive no new
assignments. Custom button/hat bindings are preserved as above.

The defaults use the existing roll/pitch convention and increasing lift when
the throttle moves forward (decreasing raw Axis 3). No calibration is measured
or saved automatically: uncalibrated axes use the supported full SDL range.
An existing lever calibration keeps its chosen 0% / 100% ends. On the action
screen, **E: Edit** then **I: Invert**, or **C: Calibrate**, adjusts orientation
and endpoints. **M** cycles through named live axes and events before saving.

Review the draft, then use **Save and Start / F5** or **Session / F6**, including
directly from the preset summary. Cancel leaves both the active profile and INI
untouched; Session leaves the INI untouched. The keyboard remains usable.
Generic defaults and normal startup do not change until you choose the preset.

## Independent USB bindings

Open `./run.sh --configure-controls` and click **F2: Multi-USB bindings**.
Import the selected device's effective legacy layout, or begin with an empty
USB draft. Import copies its actual bindings and reference normalization; it
neither guesses hardware roles nor measures physical calibration. Original
keyboard presets keep their original behavior when imported. The old INI
sections are retained. **Save and Start** writes the new opt-in profile;
**Session** applies it only in memory; **Cancel** leaves the file unchanged.
Normal startup with an older INI is unchanged.

Use **D: Devices** to enable each controller explicitly. Single-axis pedals and
button boxes with zero axes are supported. Detected candidates include keyboard
media interfaces; these remain disabled unless deliberately enabled. Already
configured candidates reuse their profile and bindings. **G: Native / mapped**
selects interpretation before binding; an existing device's bindings must be
removed before changing that interpretation. Mapped SDL gamepads retain their
mapping database interpretation. Native axes/buttons/hats use their actual SDL
indices. The exact-model preset above is optional; other devices have no guessed
physical roles.

Select an action, **A: Add binding**, then press or move a control on any enabled
connected device. Review its behavior and live result before confirming.
Binding labels include device identity and the physical control. **E** edits
only the selected binding; **N: Change physical control** recaptures its input;
**Delete** removes only that binding. Click the binding list, use **[ / ]**, or
wheel over it to select additional bindings. The action list scrolls separately.
A collision with the same physical input requires **Allow intentional reuse** or
**Move only this control**. Move removes bindings for that control, preserving
the action's other independent controls. Escape, focus loss and disconnection
cancel pending capture/edit/calibration. Held inputs must release before capture;
stale events and disabled devices cannot bind. If several axes change together
(as when a device first publishes all positions), capture resets its baselines
and asks you to move only the intended control again. It never chooses by axis
event order. Controllers never save or navigate.
The footer accepts Space/Enter only after explicit Tab focus; F5/F6 and mouse
buttons also select Save/Session.

| Action | Useful behaviors |
| --- | --- |
| Throttle / lift 0–100% | Lever position sets lift; centered axis increases/decreases; hold to increase; hold to decrease; set a chosen percentage |
| Roll, pitch, rudder | Proportional axis; hold positive direction; hold negative direction |
| Other actions | Button/hat held activation, or positive/negative axis threshold |
| Maximum lift 100%, Set lift 50% | Explicit nominal percentage presets; imported presets may retain original keyboard semantics |

The new 50% action requests half the aircraft's nominal maximum. The original
key **3** uses an aircraft-specific preset table and remains unchanged; it does
not guarantee automatic hover. Relative lift inputs repeat the original 10-unit
adjustments. Opposite increase/decrease requests cancel. Fire-repeat suppression
and analog TOW guidance retain the original pre-physics/save ordering.

Digital held actions combine across devices: releasing or disconnecting one
source keeps another source held. Flight axes use deterministic activity order,
without adding competing values. A centered flight axis needs at least 1024
calibrated units of movement to take over; a fresh digital directional press can
also take over. A stationary held direction does not continually steal control
from a newer axis movement. Direct lift requests use a smaller 256-unit movement
gate (on a 0..32767 calibrated range), with exact endpoint capture; this resolves
1% adjustments while rejecting small jitter. A stationary lever never rewrites
power after keyboard/button adjustments. Focus restore and reconnect seed the
current positions without issuing a power command. If a newly opened multi-axis
native device starts with every axis at -32768, its first multi-axis position
report is treated as initialization and reseeds the bindings without a power
request. Subsequent isolated or simultaneous motion works normally. This also
applies after reconnect; no device roles are guessed. Move the lever to reclaim it.
The request passes through the original input suppression, active upper/lower
bounds and increase availability guard. Original flight dynamics remain in use.

The live panel shows recent input events and pages of the enabled device's
actual axes/buttons/hats, including before the first binding. Choose an enabled
profile in Devices and return to Actions to monitor it; selecting a binding
selects its device for the panel. Slow movement accumulates into visible events.
Missing devices show disconnected; the other enabled devices continue working.

## Calibration and identity

Select an axis binding and **C: Calibrate**, or calibrate from its behavior
preview. **Centered** calibration requires minimum, explicit neutral/zero and
maximum. **Lever** calibration asks for your desired **0% end** and **100% end**;
reversed raw travel automatically sets inversion. It does not force a center.
The screen shows raw input, calibrated value and resulting action. Inversion is
saved per device/axis, with optional additional binding polarity. Centered
calibration also has an adjustable deadzone. Lever normalization uses endpoints
for all behaviors, including threshold activation.

Hold a position steady, then explicitly click/capture its labelled point.
Each point requires at least 12 recent samples spanning 0.2 seconds, with raw
spread at most 256. Movement invalidates old samples; stale buffers cannot capture
a new endpoint. A valid span is at least 4096 raw units. Centered sides must each
span at least 2048 and exceed the deadzone. Incomplete, noisy, zero-span and
invalid calibration never replaces the prior draft settings. Cancel retains
prior calibration. Accepted calibration stays in the setup draft until
Save/Session. There is no automatic position learning. Uncalibrated native axes
use the SDL signed reference range; mapped triggers use SDL's known 0..32767
range. Calibration of other physical roles is user-selected, not inferred.

Saved identity uses SDL **GUID plus serial** when a serial is present. Device
paths and USB enumeration indices are never saved. Unique devices restore their
bindings after USB reorder/replug. Identical units without distinct serials are
ambiguous and stop supplying input. Select the affected profile in Devices,
choose **R: Reassign**, select its matching detected occurrence, then Enter.
This is explicitly a **session occurrence**. The occurrence ID is not saved.
Profiles marked session-only require reassignment on restart/replug, even if
only one matching unit is currently connected. A single occurrence cannot feed
two profiles. Devices with serials longer than 120 bytes are rejected for durable
identity rather than truncating the serial. Real HOTAS sweeps have exercised the
full Axis 3 throttle range and the coupled pedal channel described above.
Physical calibration remains user-driven.

## USB INI schema

The editor writes bounded sections to the existing INI. There are at most 16
configured device records, 128 bindings, 32 axes, 64 physical buttons and four
hats per native device. Enumeration supports at most 64 candidates and reports
an explicit error above that limit. Limits cause errors, never silent truncation.
Names are optional display labels encoded as UTF-8 hex (up to 96 bytes); long
hardware names receive an explicit ellipsis. Identity GUIDs and serials are
never truncated. Fields below are required except calibration sections/axes.
Empty USB profiles (`devices=0`, `bindings=0`) are valid.

```ini
[usb_controls]
version = 2
devices = 1
bindings = 1
binding1 = device1,axis3,throttle,direct,1,50,16000,v2

[usb_device1]
guid = 00112233445566778899aabbccddeeff
serial = none
name = none
enabled = 1
mapped = 0
session_only = 0

[usb_calibration1]
axis3 = lever,-32768,0,32767,0,0
```

This is a schema example with a placeholder GUID, not a hardware profile.
`bindingN` has eight comma-separated fields: profile device, input, action,
behavior, direction (`1`/`-1`), chosen percentage (`0..100`), threshold
(`0..32766`) and literal `v2`. Inputs are one-based `axis1..32` or `button1..80`.
Native button slots 65..80 represent hat 1 up/right/down/left, then hats 2..4;
mapped device inputs use SDL logical controller indices. Percentage is used by
`preset`; threshold applies to axis digital/relative behaviors. The editor hides
irrelevant fields. Actions are `roll`, `pitch`, `rudder`, `throttle`,
`increase_lift`, `decrease_lift`, `maximum_lift`, `half_lift`, `gear`, `fire`,
`previous_weapon`, `next_weapon`, `chaff`, `flares`, `cockpit`, `chase`,
`toggle_view`, `pause`, `confirm`, `back`, `menu_up/down/left/right`.
Behaviors are `analog`, `direct`, `relative`, `increase`, `decrease`, `preset`,
`positive`, `negative`, `hold`, and imported `legacy_preset` for original
keyboard presets. Only meaningful action/input combinations are accepted.

`usb_calibrationN` belongs to profile device N. Each `axisN` stores type
(`centered`/`lever`), raw minimum, neutral, raw maximum, inversion (`0`/`1`),
deadzone (`0..30000`). All coordinates are bounded to -32768..32767 before
span arithmetic. Lever neutral/deadzone fields are retained but unused.
`session_only=1` records the need for explicit occurrence assignment, never an
occurrence or enumeration index. Unknown, duplicate, missing and out-of-count
USB fields fail atomically, leaving the caller's prior configuration intact.
Older duplicate legacy sections retain their established last-value behavior.

When version 2 is applied, only USB bindings execute; legacy mappings are retained
as a fallback reference without double execution. V2 saves preserve legacy,
audio/rendering, comments, CRLF style and permissions. They replace retired USB
binding keys rather than allowing stale bindings to execute. Save uses the same
validated temporary-file and file-race protection as the legacy writer. Session
and Cancel leave original bytes unchanged; save errors stay visible in setup.
