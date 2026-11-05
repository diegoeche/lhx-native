#pragma once
#include "scene_renderer.h"
#include "scene_props.h"

/* Shared bounded model preparation. Tessellation, authored coplanar details and
 * flat face lighting are shared with the CPU renderer. The consumer performs
 * projection, clipping, rasterization, sampling and depth on its own device. */
typedef struct {
    int (*view)(void *,const LhxSceneView *,const uint32_t colors[256],float daylight);
    int (*layer)(void *,int);
    int (*triangle)(void *,const LhxSceneVertex[3],const LhxSceneUV *,
                    const LhxTexture *,uint32_t,float,int,int,float);
    int (*line)(void *,LhxSceneVertex,LhxSceneVertex,uint32_t,int);
    int (*particle)(void *,LhxSceneVertex,float,uint32_t,int);
    int (*end_view)(void *);
    int (*material)(void *,int detail,int shadow); /* Optional procedural/ground-shadow state. */
    /* Immutable replacement art can remain on-device. shadow_view is NULL for
     * a solid instance, or a camera-space affine projection for its sun shadow. */
    int (*prop)(void *,const LhxPropModel *,const LhxSceneMesh *,const LhxPropFit *,
                float daylight,int lighting,const double shadow_view[3][4]);
    /* Already subdivided ground triangles; the device may drape them by
     * sampling immutable elevation grids instead of repeating CPU lookups. */
    int (*draped_triangle)(void *,const LhxSceneVertex[3],const LhxSceneUV *,
                          const LhxTexture *,uint32_t,float,int,int,float);
} LhxSceneGeometrySink;

int lhx_scene_emit_geometry(const LhxSceneFrame *,const uint8_t palette[768],
                            const LhxSceneRenderOptions *,
                            const LhxSceneGeometrySink *,void *);
