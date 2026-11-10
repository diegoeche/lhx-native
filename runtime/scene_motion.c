/* Optional presentation of the original simulation's sparse distant updates.
 * Keep history in world space so moving cameras do not become object motion. */
#include "scene_motion.h"
#include <math.h>
#include <string.h>

void lhx_scene_motion_reset(LhxSceneMotion *motion)
{
    if (motion) memset(motion,0,sizeof(*motion));
}

static void begin(LhxSceneMotionEntry *entry, uint64_t identity,
                   const double world[3], double now)
{
    *entry=(LhxSceneMotionEntry){.identity=identity,.changed_at=now,.seen_at=now};
    memcpy(entry->sample,world,sizeof(entry->sample));
    memcpy(entry->from,world,sizeof(entry->from));
}

static void display(const LhxSceneMotionEntry *entry, double now, double out[3])
{
    double alpha=entry->duration>0 ? (now-entry->changed_at)/entry->duration : 1;
    alpha=fmax(0,fmin(1,alpha));
    for (unsigned k=0;k<3;k++)
        out[k]=entry->from[k]+alpha*(entry->sample[k]-entry->from[k]);
}

static double distance(const double a[3], const double b[3])
{
    return hypot(hypot(a[0]-b[0],a[1]-b[1]),a[2]-b[2]);
}

void lhx_scene_motion_offset(LhxSceneMotion *motion, int enabled,
                             uint64_t identity, const double world[3],
                             double range, double now, double offset[3])
{
    if (!offset) return;
    offset[0]=offset[1]=offset[2]=0;
    if (!motion) return;
    if (!enabled || !isfinite(now) || now<0) {
        lhx_scene_motion_reset(motion);
        return;
    }
    if (motion->has_time && now<motion->last_time) lhx_scene_motion_reset(motion);
    motion->last_time=now;
    motion->has_time=1;
    LhxSceneMotionEntry *entry=NULL,*slot=motion->entries;
    for (unsigned i=0;i<LHX_SCENE_MOTION_CAPACITY;i++) {
        LhxSceneMotionEntry *candidate=motion->entries+i;
        if (identity && candidate->identity==identity) entry=candidate;
        if (slot->identity && (!candidate->identity || candidate->seen_at<slot->seen_at))
            slot=candidate;
    }
    if (!identity || !world || !isfinite(range) || range<=LHX_SCENE_MOTION_NEAR_RANGE ||
        !isfinite(world[0]) || !isfinite(world[1]) || !isfinite(world[2])) {
        if (entry) memset(entry,0,sizeof(*entry));
        return;
    }
    if (!entry || now-entry->seen_at>8.0) {
        begin(entry?entry:slot,identity,world,now);
        return;
    }
    entry->seen_at=now;
    if (world[0]!=entry->sample[0] || world[1]!=entry->sample[1] ||
        world[2]!=entry->sample[2]) {
        double previous[3];
        display(entry,now,previous);
        /* Treat large discontinuities as reuse/teleport, not a flight path.
         * Also bound accumulated visual separation when updates accelerate. */
        if (now<=entry->changed_at || distance(world,entry->sample)>8192 ||
            distance(world,previous)>8192) {
            begin(entry,identity,world,now);
            return;
        }
        entry->duration=fmax(1.0/60,fmin(LHX_SCENE_MOTION_MAX_INTERVAL,
                                        now-entry->changed_at));
        entry->changed_at=now;
        memcpy(entry->from,previous,sizeof(entry->from));
        memcpy(entry->sample,world,sizeof(entry->sample));
    }
    double shown[3];
    display(entry,now,shown);
    for (unsigned k=0;k<3;k++) offset[k]=shown[k]-world[k];
}
