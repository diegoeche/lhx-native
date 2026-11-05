#pragma once
#include <stdint.h>

typedef struct { unsigned width, height; uint32_t *pixels; } LhxTextureLevel;
typedef struct LhxTexture {
    unsigned count;
    LhxTextureLevel levels[13];
    uint64_t revision; /* Changes when texture storage/content is regenerated. */
} LhxTexture;
typedef struct { float x,y,z,u,v,shade; } LhxTerrainVertex;
typedef struct {
    LhxTexture texture; double bounds[4];
    unsigned source_n,step,n,vertex_count,index_count;
    float *heights;
    LhxTerrainVertex *vertices;
    uint32_t *indices;
    uint64_t relief_revision;
} LhxTerrainGrid;
typedef struct { LhxTerrainGrid near, far; } LhxTerrain;

/* Bounded RGB texture resources. All sampling clamps to the image edges.
 * footprint is the largest projected pixel footprint in base-level texels. */
void lhx_texture_free(LhxTexture *);
int lhx_texture_mipmaps(LhxTexture *);
uint32_t lhx_texture_sample(const LhxTexture *, double u, double v, double footprint);
int lhx_texture_runway(LhxTexture *);
int lhx_texture_runway_detail(LhxTexture *,double length,double width,int bearing);
int lhx_texture_load(LhxTerrainGrid *, const char *path);
