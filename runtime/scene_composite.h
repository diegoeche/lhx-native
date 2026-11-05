#pragma once
#include "scene_types.h"
#include "lifted.h"

/* Read the original two-color window immediately after its horizon fill.
 * Coordinates remain in the original 320x200 screen. Camera/rect fields are
 * owned by the capture code; this function changes only view->horizon. */
void lhx_scene_read_horizon(CPU *cpu, LhxSceneView *view);

/* Overlay original VGA pixel art on an already rendered ARGB8888 scene.
 * Palette values are the original six-bit DAC channels. Index255 exposes the
 * native scene only inside the frame's captured windows; it is opaque outside.
 * NULL/empty frames render the complete original screen, including menus. */
void lhx_scene_composite(const LhxSceneFrame *frame, const uint8_t *vga,
                         const uint8_t palette[768], uint32_t *pixels,
                         unsigned width, unsigned height);

/* Original-size RGBA overlay for GPU composition. Only index255 inside captured
 * scene windows is transparent; pixel art and menus otherwise remain opaque. */
void lhx_scene_overlay(const LhxSceneFrame *,const uint8_t *vga,
                        const uint8_t palette[768],uint32_t pixels[64000]);
