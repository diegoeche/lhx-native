#pragma once
#include "scene_texture.h"
#include <stdio.h>

/* Track actual reads of the known EALIB theatre entries. No guest writes and
 * no resource I/O when disabled. Read-only archive inspection restores position. */
void lhx_terrain_configure(int enabled,const char *directory);
void lhx_terrain_relief_configure(int enabled);
void lhx_terrain_file_open(unsigned handle,FILE *);
void lhx_terrain_file_close(unsigned handle);
void lhx_terrain_file_read(unsigned handle,long position,unsigned bytes);
const LhxTerrain *lhx_terrain_current(void);
void lhx_terrain_reset(void);
