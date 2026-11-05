# Development and validation

```sh
ctest --test-dir build/cmake --output-on-failure
```

Tests cover extraction, EXEPACK reconstruction, all five EALIB archives,
carry arithmetic, repeated string copying, the generated projection routine,
the model optimizer's arithmetic, cockpit needle drawing in every direction,
embedded mission callbacks, frame pacing, and the sound driver's calls and
timer chaining.
Native scene checks cover geometry clipping and depth, scene capture and
camera changes, horizon recovery, and compositing the original cockpit/HUD.
Visibility tests exercise loaded-world traversal, floating-point placement,
vehicle variants and the measurement reports. Paired native replays compare
extended visibility with the original cutoffs and verify unchanged aircraft
state alongside the added distant geometry.
Visual-enhancement checks also cover reference elevation interpolation, ground
placement and depth occlusion, shadow overlap, service paint, attached surface
patterns, rotor animation and runway headings on CPU and GPU.
Visual-enhancement checks cover perspective-correct texture mapping and near
clipping, mip filtering, world-fixed lighting through camera turns, resource
bounds and theatre switching. A combined native flight replay checks unchanged
complete game memory and original cockpit/HUD pixels with the upgrades on.
End-to-end tests boot the actual native
executable, check the original title, and type a pilot name to reach the
mission-selection screen.
The flight test runs Free Flight twice and checks the original aircraft's
power setting and world position, proving ascent only after collective input.

For bounded automated runs:

```sh
./build/cmake/lhx-native --headless --seconds 20 \
  --screenshot build/frame.ppm --keys '5:enter,10:enter'
```

`--keys` uses timed key presses; `--headless` uses a deterministic virtual clock
with the same flight frame deadlines and no wall-clock sleeps.
These times do not measure original PC instruction cycles. Screenshots are PPM
files. Untranslated control-flow targets fail explicitly and are logged to
`build/misses.txt`.

## Supported input and implementation

The current native substitutions are specific to the supplied Hit Squad CD
release. Its packed `LHX.EXE` SHA-256 is
`d0afb93033121ba2403cf4f82afa6865e4b889e961733e791aa5929b94e7d325`.
The unpacked load-image SHA-256 is
`4d30628da15b38eb93cad06420d255574db7283cfec4292f9bfbc8b9d4369f9e`.
The generator rejects other images before applying fixed-address substitutions.

The EXEPACK unpacker never executes DOS code. Recursive instruction discovery
and the PCRecomp lifter produce C functions; indirect branches select from
already compiled functions. LHX's generated interrupt trampoline and projection
routine have native semantic equivalents. Known self-modified operands are
read from the game's memory state. The graphics routines' authored changes
between forward and backward pixel steps select compiled native operations.
The PC speaker driver is translated
separately and identified after the game loads it at its chosen address.
For model-specific transforms, observers retain the original compiler's
coefficient graphs before it converts them into machine code. Native math
evaluates those graphs directly, preserving predecessor reuse and rounding;
the runtime does not decode the generated instructions.
The embedded callbacks from all 22 supplied mission scenarios are also compiled
at build time. Aircraft `.FMD` files are parameter data read by the original
compiled flight code.

Static recovery cannot prove that every indirect branch or code-patching path
has been found. Unsupported instructions and missing native targets stop with
diagnostics instead of falling back to interpretation.

Game files and generated C stay out of version control.
