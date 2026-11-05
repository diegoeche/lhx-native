#pragma once
#include "scene_texture.h"
typedef struct LhxSky { LhxTexture day,night; } LhxSky;
LhxSky *lhx_sky_load(const char *directory);
void lhx_sky_free(LhxSky *);
