#pragma once
#include "scene_types.h"
#include "scene_texture.h"

typedef struct { LhxSceneVertex position;LhxSceneUV uv; } LhxPropVertex;
typedef struct { unsigned count;int one_sided;LhxPropVertex *vertices;LhxTexture texture; } LhxPropGroup;
typedef struct { unsigned count;LhxPropGroup *groups;float minimum[3],maximum[3]; } LhxPropModel;
typedef struct LhxProps LhxProps;
typedef struct { float scale[3],offset[3],center[3];int turn; } LhxPropFit;

/* Optional prepared static art only; failed/missing classes keep original models.
 * Models are immutable until free, and the aggregate allocation is bounded. */
LhxProps *lhx_props_load(const char *directory);
void lhx_props_free(LhxProps *);
const LhxPropModel *lhx_props_select(const LhxProps *,const LhxSceneMesh *,LhxPropFit *);
LhxSceneVertex lhx_props_vertex(const LhxSceneMesh *,const LhxPropFit *,LhxSceneVertex);
