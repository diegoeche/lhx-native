#pragma once
#include "lifted.h"
#include "scene_types.h"
#include <stddef.h>

typedef void *(*LhxSceneWorldAlloc)(void *context, size_t count, size_t size);
typedef void (*LhxSceneWorldFree)(void *context, void *pointer);
/* One transfers ownership of every mesh allocation; zero declines ownership. */
typedef int (*LhxSceneWorldEmit)(void *context, uint32_t object_id,
                               LhxSceneMesh *owned);
typedef struct {
    unsigned considered, inactive, groups, unsupported, frustum_rejected;
    unsigned emitted, vertices, primitives;
} LhxSceneWorldStats;

/* Rendering metadata from the same camera/object fields already used here.
 * Unknown or malformed optional metadata is left disabled. No guest writes. */
void lhx_scene_world_environment(CPU *, LhxSceneView *);
void lhx_scene_world_surface(CPU *, unsigned record, uint16_t object_segment,
                             unsigned object, LhxSceneMesh *);

/* Read all currently loaded cells/global objects. Never modifies guest memory,
 * simulation state, original draw lists, or spawning. No finite far plane.
 * Returns zero for malformed data/allocation/emit failure, one otherwise.
 * Unsupported custom models remain on the original capture path. */
int lhx_scene_world_capture(CPU *cpu, const LhxSceneView *view,
                            LhxSceneWorldAlloc allocate,
                            LhxSceneWorldFree release,
                            LhxSceneWorldEmit emit, void *context,
                            LhxSceneWorldStats *stats);
