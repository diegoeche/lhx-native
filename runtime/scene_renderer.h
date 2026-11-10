#pragma once

#include "scene_types.h"
#include "scene_texture.h"
struct LhxProps;
struct LhxSky;

typedef struct {
    int lighting, runway_textures;
    int terrain_relief,shadows,military_paint,surface_detail,rotor_blades,runway_detail;
    const LhxTerrain *terrain; /* Optional satellite imagery/elevation, world coordinates. */
    int antialiasing; /* Native world only, before original pixel-art composition. */
    int sphere_depth; /* Depth-test finite spheres, including burning-object smoke. */
    int hide_ground_dots; /* Only the original ground-reference sphere grid. */
    int smooth_distant_aircraft; /* Render-only interpolation outside combat range. */
    int atmosphere; /* Distance haze and a continuous terrain/sky horizon. */
    const struct LhxProps *props; /* Optional static scenery replacements. */
    const struct LhxSky *sky;
    double time_seconds; /* Host render clock; no simulation writes. */
} LhxSceneRenderOptions;

/* Rasterize the original camera-space scene into an ARGB8888 framebuffer.
 * Palette entries are the original six-bit VGA RGB components. View rectangles
 * use exclusive x1/y1 bounds in the original 320x200 coordinate system.
 * Returns 1 after drawing a view, 0 for an empty frame, and -1 for invalid input,
 * an allocation failure, or geometry exceeding the limits below. The renderer
 * owns reusable scratch memory and is intended for one rendering thread.
 *
 * Bounds: 8192 pixels per dimension, 16,777,216 pixels total, 64 views, 65,536
 * meshes per view, 65,536 vertices and 4096 primitives per mesh, and 1024 indices
 * per primitive. These exceed the original engine's byte-sized polygon counts.
 */
int lhx_scene_render(const LhxSceneFrame *frame, const uint8_t palette[768],
                     uint32_t *pixels, unsigned width, unsigned height);
int lhx_scene_render_with_options(const LhxSceneFrame *, const uint8_t palette[768],
                                  uint32_t *, unsigned width, unsigned height,
                                  const LhxSceneRenderOptions *);
void lhx_scene_renderer_reset(void);
