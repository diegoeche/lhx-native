#pragma once
#include <stdint.h>

enum { LHX_SCENE_MOTION_CAPACITY = 128 };
/* Common world units (feet). Keep nearby geometry/aiming authoritative.
 * The supported helicopter visibility bound is 31*256=7936; the retained
 * weapon definition table has engagement ranges up to 45000. 49152 keeps
 * both inside the authoritative region. This is a visual policy, not a new
 * gameplay range. Evidence and the native/DOS distinction are in the local
 * aircraft-motion investigation report. */
#define LHX_SCENE_MOTION_NEAR_RANGE 49152.0
#define LHX_SCENE_MOTION_MAX_INTERVAL 4.0

typedef struct {
    uint64_t identity;
    double sample[3], from[3];
    double changed_at, duration, seen_at;
} LhxSceneMotionEntry;
typedef struct {
    LhxSceneMotionEntry entries[LHX_SCENE_MOTION_CAPACITY];
    double last_time;
    unsigned has_time;
} LhxSceneMotion;

/* Zero-initialize once; reset on mission/load, camera-family changes or when
 * the option is disabled. No allocation, guest memory or simulation calls. */
void lhx_scene_motion_reset(LhxSceneMotion *motion);

/* Return an additive WORLD-space visual offset, never an authored position.
 * Pass only aircraft; world[] is the current authored origin, range is its
 * camera distance, and time is render_options.time_seconds. The caller owns
 * enablement and applies the offset through the current camera basis to all
 * corresponding geometry/placement. Nearby/disabled/invalid/first calls
 * return exactly zero. Entering the nearby region discards any distant lag.
 *
 * identity must be nonzero and include model/lifecycle generation when known.
 * Explicit reset (or a new identity) handles reuse at the same location; large
 * jumps and observations absent for >8 seconds also discard history. A bare
 * address cannot distinguish every same-model, same-location respawn.
 *
 * Interpolate from the prior DISPLAYED origin on each changed sample over
 * the measured sample interval (1/60..4 seconds). No extrapolation/velocity.
 * The resulting distant image lags gameplay; each new interpolation lasts
 * at most four seconds and stops at the latest sample during a longer hold.
 * Original targeting, projectiles and collision must never use this offset. */
void lhx_scene_motion_offset(LhxSceneMotion *motion, int enabled,
                             uint64_t identity, const double world[3],
                             double range, double time_seconds,
                             double offset[3]);
