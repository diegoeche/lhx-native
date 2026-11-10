# Rendering

The supplied profile uses the native **OpenGL GPU backend at 2560×1600**. It rasterizes the
captured 3D models at that resolution, with depth testing and polygon clipping.
The original cockpit, HUD, menus and special 2D effects retain their
pixel artwork. Flight dynamics, AI and mission logic still run through the
original compiled game code.

```sh
./run.sh --render-scale 6       # 1920×1200 world rendering
./run.sh --renderer original    # original 320×200 rendering
./run.sh --backend cpu --render-scale 3  # native CPU renderer at 960×600
```

`--render-scale` accepts 1–8 and multiplies the original 320×200 canvas; the
built-in default is 3; the supplied GPU profile selects 8. Resizing the window
scales this selected canvas. OpenGL 3.3 moves
projection, clipping, rasterization, depth, texture sampling and antialiasing
onto the graphics device. The simulation, scene extraction, and original-model triangulation/face lighting
remain native CPU code shared with the fallback. Replacement scenery stays in
cached GPU meshes; the GPU applies each object's fit, pose, lighting and shadow
projection. Ground markings sample cached elevation textures in the vertex shader.
Higher scales increase graphics cost. Resolution adds finer edges and small visible
details; it does not add polygons to the original models.

Headless checks retain original rendering unless a renderer or scale is
selected in the INI or on the command line. Screenshots use the selected canvas
resolution.

Native play also uses **extended visibility** by default: it reads the loaded
world directly and draws supported models at their highest available detail,
without the DOS distance cutoffs, small draw-list arena or 16-bit translation
range. It preserves camera visibility flags and reads existing object state;
it does not spawn objects or change their simulation. Custom 2D effects and
unsupported model callbacks keep their original drawing path.

Choose rendering options in the existing [`controls.ini`](../controls.ini) and
restart the game:

```ini
[rendering]
renderer = native
backend = gpu
render_scale = 8
visibility = extended
show_fps = false
antialiasing = true
sphere_depth = true
hide_ground_dots = true
smooth_distant_aircraft = true
atmosphere = true
photo_skies = true
sky_dir = bin/enhanced/sky-subtle
replacement_models = true
models_dir = bin/enhanced/models
lighting = true
runway_textures = true
terrain_textures = true
terrain_relief = true
shadows = true
military_paint = true
surface_detail = true
rotor_blades = true
runway_detail = true
terrain_dir = bin/enhanced/terrain
```

Set `backend = cpu` for the previous native CPU renderer. Profiles omitting this
key retain CPU rendering. `--backend cpu|gpu` overrides the INI. GPU initialization
failure prints a diagnostic and uses the CPU backend. `renderer = original`
always uses original rendering, independently of this setting. GPU startup logs
the actual OpenGL device; a driver such as llvmpipe is software emulation, not
hardware acceleration. `--headless --backend gpu` uses SDL's offscreen OpenGL
driver where available, with the same explicit fallback if it is unavailable.
Fallback retains the selected resolution; use `--render-scale 3` for the
previous 960×600 CPU profile if GPU support is unavailable.

Set `visibility = original` to disable extended visibility, or
`renderer = original` to use the original 320×200 renderer. `render_scale`
accepts 1–8; `show_fps` accepts `true` or `false`. These options also load with
`--no-controller` and `--headless`. Command-line rendering options override the
INI values. `--controls PATH` selects a different INI for both rendering and
controller bindings. Old profiles without `[rendering]` keep the previous
defaults, including original rendering for headless checks unless native
rendering or scale is explicitly selected. The supplied INI selects native
rendering explicitly.

The supplied INI enables the visual upgrades below. Profiles omitting them
keep the built-in defaults of `false`. Set `lighting = true` for a
fixed sun/sky light on solid world faces, or `runway_textures = true` for
generated concrete, slab joints, edge stripes, thresholds, centreline and
helipad markings. The light stays fixed as the camera turns. Original cockpit,
HUD and attitude-instrument artwork keep their previous rendering. Target
world views share enabled enhancements. Each upgrade works
independently, with either visibility mode; `renderer = original` uses the
original drawing path.

`hide_ground_dots = true` removes the original grid of ground-reference dots
from the native CPU and GPU views. Stars, smoke and distant scenery keep their
normal drawing. Set it to `false`, or omit it, to retain the original dots.

`smooth_distant_aircraft = true` smooths the original simulation's sparse updates
of distant aircraft. It affects the main native CPU/GPU view beyond 49,152 feet.
Nearby aircraft, targeting, missiles and collision
keep their original positions. The distant image catches up between received
positions; each catch-up lasts at most four seconds. The image can lag gameplay
by seconds and returns immediately to its authored position on entering the
near region. Aircraft rotation retains its original update cadence.
Set this option to `false`, or omit it, for the original stepped presentation.
Terrain relief also preserves the altitude of airborne point LODs; only ground
markings and the ground-reference grid are draped onto the terrain.

`antialiasing = true` smooths native scene edges before composing the original
cockpit, HUD and effect pixels. Lines and point markers use fractional coverage
at the output resolution, and finite spheres smaller than a pixel fade with
their projected area. This reduces heavy distant specks and abrupt pixel pops
without rendering the whole scene at a higher resolution. It is spatial
smoothing; it cannot recover every detail smaller than a pixel or eliminate
all motion shimmer. Set it to `false` for the previous native drawing path.
Profiles omitting this key retain the built-in `false` default. On the GPU this
option selects up to four multisamples for world geometry, with analytic
coverage for point markers and spherical particles. The cockpit/HUD overlay is
composited afterward with nearest-neighbor sampling so its pixel art stays sharp.
Gun tracers retain a stroke of one original 320×200 pixel as the canvas grows,
so AA does not shrink them to a faint output-pixel hairline at high render scales.
Their captured endpoints, palette colour and depth remain in use. This applies
only to the `bullet` model's lines; scenery details retain their thin AA path.
The title logo retains its existing original-pixel stroke compensation.

`sphere_depth = true` gives finite round particles, including smoke from burning
objects, depth testing against solid scenery. Previously these shared the
background pass with point markers, allowing distant buildings to cover nearer
smoke. Stars and point markers retain their background behavior. This option
does not change object positions or the original 2D explosion artwork. Omitted
or `false` retains the previous layering; the supplied profile enables it.

`atmosphere = true` blends distant terrain and geometry into day/night haze and
uses a continuous camera-aligned horizon. Satellite imagery has finite bounds;
without haze, the original ground colour reappears beyond its far edge. The
enabled option replaces that exposed strip with haze. It does not remove the
original hills or create terrain relief. Set it to `false` for the previous
horizon and unmodified distance colours.

`photo_skies = true` selects a day/night equirectangular panorama by the game's
sky brightness, oriented with the camera. The existing option accepts both
pixel-art and photographic packs. Import the reference photo panoramas once:

```sh
python3 tools/import_skies.py \
  --source /path/to/reference/assets/sky
```

That command retains the existing JPG/Poly Haven CC0 defaults and writes to
ignored `bin/enhanced/sky/`. For another pack, place `day.png`, `night.png` and
an accurate `SOURCE.txt` in its source directory, then import with explicit
licence metadata, for example:

```sh
python3 tools/import_skies.py \
  --source /path/to/sky-art \
  --output bin/enhanced/sky-custom --source-format png --width 1024 \
  --licence 'Your artwork licence; see SOURCE.txt'
```

PNG imports require `--licence`; the importer makes no automatic CC0 claim for
them. It copies `SOURCE.txt` and records input/output hashes and the supplied
licence in `SOURCE.json`. Source files stay unchanged, including when stored
in the output pack's `source/` child directory. Conversion only decodes and
resizes the supplied art; it does not create clouds or change its style.
`--nearest` preserves the source pixel colours and block edges while resizing;
omitting it retains ImageMagick's default filter used by existing photo imports.

`sky_dir` selects the prepared pack. The supplied profile selects
`bin/enhanced/sky-subtle`: the approved soft daytime panorama, paired with
the unchanged original photo night sky. The day image is 2048×1024 with
smooth filtering. Its source, prompt and mixed provenance are recorded
inside the pack. Use `sky_dir = bin/enhanced/sky` for the preserved
photographic pack.
Set `photo_skies = false` for the original flat sky. Missing or invalid files
also retain the original sky. Imported files use native `LHXSKY1` RGB at 2:1;
`--width` accepts 512, 1024, 2048 or 4096, with half that height. The panorama
wraps horizontally; matching left/right edges avoid a visible join. Above the
horizon the renderer samples the upper half, with the horizon at the middle
row, so keep that band clear. Existing filtering, slight dimming and optional
haze also apply to pixel art.

The sky and atmosphere affect the main scene and target display; the original
cockpit, HUD and attitude instrument keep their artwork. Both options work
independently and default to `false` when omitted.

`replacement_models = true` uses the reference Kenney static scenery at the
game's object positions and headings, fitted to its model bounds. This includes
houses, hangars, factories, storage tanks, tents, huts, palms and other trees. Vehicles,
soldiers, towers, cranes and destroyed models retain the game's geometry.
Prepare this optional art once:

```sh
python3 tools/import_scenery.py \
  --source /path/to/reference/assets/models/kenney
```

The importer reads the source checkout without changing it. It handles the
static glTF subset used by these models, writes bounded native meshes/materials
and source hashes to ignored `bin/enhanced/models/`, and retains CC0 attribution.
`models_dir` selects another prepared directory. Missing/invalid classes retain
original models. The option defaults to `false` when omitted and works with both
native backends. Importing skies/scenery requires ImageMagick; native play does
not need a glTF library or image conversion tools.

The reference has no separate animated-water renderer. Sea, coast and river
appearance comes from its satellite imagery; the native `terrain_textures`
option uses those same images. Visual elevation is available separately through `terrain_relief`.

For satellite ground, first import the images from the earlier GPU checkout:

```sh
python3 tools/import_terrain.py \
  --source /path/to/reference/assets/terrain
```

This optional preparation needs ImageMagick (`magick`). It reads the source
checkout without changing it and writes bounded native textures plus an
attribution/hash manifest to ignored `bin/enhanced/terrain/`. Normal builds and
play do not require ImageMagick or an additional rendering library.
Set `terrain_textures = true` after importing. The original city model's green
ground parcels (material 125) and the approved flat patches `square1` (10),
`square2` (6) and `square3` (3) also sample the satellite imagery in world
coordinates on both native backends, preserving their geometry and relief.
Other square materials, city paving (47), lines (39), roads, trails, rivers,
water and other scenery retain their materials. Parcels outside the imagery's
coverage, disabled terrain textures, or missing terrain art keep the original
flat color.

Gulf/Libya, Asia/Vietnam and Europe imagery switches with the selected mission
theatre. Missing or invalid
files use original ground and print a diagnostic. `terrain_dir` may select a
different prepared directory; relative paths are resolved from the working
directory (`run.sh` starts in the project directory).

With `terrain_relief = false`, satellite imagery covers the original flat ground.
To enable the reference's visual elevation and smooth hill replacements, prepare
its height data once as well:

```sh
python3 tools/import_relief.py \
  --source /path/to/reference/assets/terrain
```

Enable both `terrain_textures` and `terrain_relief`. The importer retains the
reference's near/far grids, their triangle sampling, overlap and bell-shaped hill
mounds. Immutable indexed terrain stays cached on the GPU, which transforms it
with the camera. Scenery rises by the terrain height beneath it relative to the
height beneath the camera. Ground markings and runways are subdivided to follow
the surface, and terrain depth hides distant scenery. Missing or invalid relief
keeps flat imagery and the original hill models. These changes affect appearance;
flight altitude, world coordinates and collision behavior remain the original
simulation. This can produce differences between visible slopes and collision
surfaces. Prepared data and source hashes remain ignored local assets.

`shadows = true` adds sun shadows for grounded scenery, vehicles and soldiers,
plus soft contact shade under replacement scenery. Each sun-shadow pixel darkens
once even where faces overlap; airborne models are excluded, and aircraft keep
their original shadow artwork. Shadows follow the visual terrain when relief is
available and also work on flat ground.

`military_paint = true` tones aircraft/vehicle palette colours toward the
reference's olive service paint. `surface_detail = true` adds model-space panel
lines and fine wear on aircraft, and two-tone camouflage on ground vehicles.
Patterns stay attached as models move; panel lines fade when too small to resolve.
`rotor_blades = true` recognizes moving rotor sectors and draws four turning
blades over a fainter blur, with a faster tail rotor and softer canopy glass.
The animation uses the render clock and does not change the flight model.

`runway_detail = true`, together with `runway_textures`, adds heading numbers
at both ends, touchdown/aiming marks, weathering and tyre wear. Numbers follow
the runway's world bearing; the generated native lettering differs from the
reference's browser font. Runways drape over the visual terrain when enabled.
All these flags default to `false` when omitted and support both native backends.
Generated runways and terrain follow the game's sky brightness. Mip filtering
reduces distant texture shimmer.

```sh
./run.sh --visibility original  # compare original visibility at native resolution
./run.sh --render-stats build/render-stats.json
```

The GPU path keeps world rendering and original-art composition on the graphics
device; ordinary display frames do not read the framebuffer back to the CPU.
Screenshots perform one explicit readback. Window resizing preserves the selected
canvas aspect ratio with black borders where necessary.

The optional JSON report is written on exit. It records mesh/vertex counts,
model-origin distances, original culling counters, and separate host CPU
timings for native capture, scene drawing and composition (mean, median, 95th
percentile and maximum). It excludes simulation and SDL upload/display time.
Measurements use the real monotonic clock even in a headless replay. For the GPU
backend these host stage times measure CPU preparation/submission, not device
execution. The separate `gpu` object reports the actual device, software-driver
status, sample count, framebuffer readbacks, and asynchronous OpenGL timestamp
durations for device scene/overlay drawing. Presentation is excluded from those
device timings. Query collection does not block play; reporting drains pending
queries at exit. GPU timing queries are enabled only with `--render-stats`.
Timing collection and geometry scans are disabled unless requested.
The GPU report also counts cached scenery instances, vertices uploaded once,
vertices rendered from those meshes, streamed vertices, and vertices draped on
the GPU. The `services` object records real-clock polling gaps, slow display
calls, observed timer lateness and audio callback gaps. Callback cadence does
not by itself prove audible playback quality. These diagnostics help distinguish
render stalls from audio-device scheduling; real-clock service gaps are zero in
virtual headless replays.

Display deadlines leave a short service interval after an over-budget draw.
This prevents an expensive scene from making every subsequent timer/input poll
redraw immediately. Original simulation timing remains controlled by its frame
clock and IRQ path.

For a saved `--dump-memory build/state.ram` snapshot,
`python3 tools/measure_visibility.py build/state.ram --output build/visibility.json`
inventories loaded object records and
their model-specific distance thresholds in feet and miles. These are nominal
distance checks, distinct from actual camera visibility or simulation activity.

Basic controls from the supplied manual:

- Enter confirms selections; arrows move through menus.
- Top-row **5** sets maximum throttle/collective for takeoff; **3** selects hover.
- Keypad **5** or **/** centers the flight stick.
- **G** retracts the gear on the LHX and Osprey.
- **F1** selects the cockpit; **F5** selects the chase view.
