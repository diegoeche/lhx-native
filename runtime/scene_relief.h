#pragma once
#include "scene_texture.h"
#include "scene_types.h"

/* Prepared reference elevation/mounds, independent of the original simulation.
 * Sampling follows the rendered grid's b-c triangle split; mounds are art only. */
int lhx_relief_load(LhxTerrainGrid *,const char *);
void lhx_relief_free(LhxTerrainGrid *);
int lhx_relief_ready(const LhxTerrain *);
double lhx_relief_height(const LhxTerrain *,double x,double z);
LhxSceneVertex lhx_relief_view(const LhxSceneView *,const LhxTerrain *,double x,double y,double z);
