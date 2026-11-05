# Controller

Supported SDL gamepads connect automatically, including the Logitech F310 in
DirectInput mode (reported as **Logitech Dual Action**). Start with `./run.sh`;
the terminal prints the connected controller's name. The D/X switch can stay
in its current position. Use the keyboard to type your pilot name.

| Control | Action |
| --- | --- |
| Left stick | Proportional cyclic: pitch and bank; release to center |
| Right stick left/right | Rudder, taking priority over bank while deflected |
| Right stick up/down | Increase/decrease collective in 10% steps |
| D-pad | Menu arrows |
| A | Confirm / next target |
| B | Escape / back |
| RT | Fire; hold to repeat |
| X | Deploy chaff (C) |
| Y | Deploy flares (F) |
| LT | 50% collective / hover preset |
| LB / RB | Collective -/+ in 10% steps; hold to repeat |
| Click left stick | Landing gear |
| Click right stick | Next weapon |
| Start | Pause / resume |
| Back | Toggle cockpit / chase view |

The sticks have a center deadzone. Disconnecting the controller or switching
away from the game releases its held keys and centers its flight inputs.
Keyboard controls remain available. `./run.sh --list-controllers` lists devices;
`./run.sh --no-controller` disables controller input. Headless test runs ignore
physical controllers.

Controller bindings share [`controls.ini`](../controls.ini) with rendering options.
Edit the file and restart the game; no rebuild is needed. For example, under `[buttons]`, set
`a = fire` and `x = confirm` to put firing on A and confirmation on X.
Use `none` to unbind an input. Available action names are listed in the file.
Keyboard bindings remain the original game's controls.

Under `[axes]`, assign `roll`, `pitch`, `rudder`, or `collective` to change stick
roles; prefix an action with `-` to invert it (`lefty = -pitch`). Trigger axes
can also use `fire`, `hover`, `chaff`, and `flares`. Countermeasures deploy once
per press, so holding a button does not drain their inventory. Hold-to-fire
sends repeated Space press/release pairs, which the original game accepts even
when it filters keyboard repeat.
Releasing the controller input stops the repeats.

The `[settings]` section controls deadzones, thresholds and repeat timing.
After an initial 350 ms delay, throttle repeats every 200 ms and fire every
100 ms by default. Opposite throttle inputs cancel while both are held.
Use `./run.sh --controls PATH` for another profile. Missing default files use
the built-in layout; an invalid profile prints its filename and line number
and stops before starting the game.
