#pragma once

#include "scene_types.h"
#include <stdio.h>

#define LHX_SCENE_METRICS_DEFAULT_SAMPLES 262144u
#define LHX_SCENE_METRICS_MAX_SAMPLES 1048576u

/* Optional, single-threaded diagnostics; record_frame is a no-op before init.
 * Init returns 1 on success, 0 on allocation failure or an excessive limit.
 * A zero limit selects DEFAULT_SAMPLES. Successful reinitialization clears all
 * measurements. Storage is bounded to 16 bytes per retained draw plus 8 bytes
 * per retained capture (6 MiB at the default limit).
 */
int lhx_scene_metrics_init(unsigned sample_limit);
int lhx_scene_metrics_enabled(void);

/* Record one actual host draw, not one guest frame. Supply separately measured
 * host elapsed times for scene rasterization and composition, in microseconds.
 * Empty/null scenes are allowed. Geometry is scanned once per frame pointer and
 * serial; the producer must change serial whenever its captured geometry changes.
 * Vertex distances use captured camera-space geometry in the engine's common
 * units. Object-origin maxima use mesh.object_distance, excluding background
 * meshes, synthetic object_id=0 meshes, and invalid/negative distances. Reports
 * keep common-unit labels; these are neither visibility limits nor miles.
 */
void lhx_scene_metrics_record_frame(const LhxSceneFrame *frame,
                                    uint64_t render_us, uint64_t composite_us);

/* World-geometry capture happens at its own rate, independently of host draws.
 * Count source records considered, meshes emitted, and unsupported records.
 * Its timing has a separate reservoir/count; it is excluded from draw combined.
 */
void lhx_scene_metrics_record_capture(uint64_t elapsed_us, unsigned considered,
                                      unsigned emitted, unsigned unsupported);

/* Exact original main-view scene-list counters supplied by the caller.
 * No guest CPU or unit conversion is performed by this module. arena_full is
 * normalized to 0/1; its mean is the fraction of lists that filled the arena.
 */
void lhx_scene_metrics_record_visibility(unsigned candidates, unsigned range_rejected,
                                         unsigned frustum_rejected, unsigned listed,
                                         int arena_full);

/* Reports include separate render/composite/combined timing mean, nearest-rank
 * p50/p95, and maximum. Means/maxima include every draw. Quantiles are exact up
 * to the sample limit, then use a deterministic unbiased reservoir; the report
 * explicitly states which method and how many samples were retained.
 * JSON and CSV contain aggregate per-draw geometry counts and camera distances.
 * Return 1 on success, 0 if disabled, -1 on allocation or output failure.
 * Reporting preserves the measurements and may be repeated.
 */
int lhx_scene_metrics_report_json(FILE *out);
int lhx_scene_metrics_report_csv(FILE *out);
void lhx_scene_metrics_reset(void);
