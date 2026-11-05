#pragma once
#include "scene_renderer.h"
#include <SDL.h>
#include <stdio.h>

typedef struct LhxGpu LhxGpu;
/* Context/window lifecycle belongs to the caller; destroy GPU before window. */
void lhx_gpu_attributes(void);
LhxGpu *lhx_gpu_create(SDL_Window *,unsigned width,unsigned height,int antialiasing,int timings);
void lhx_gpu_destroy(LhxGpu *);
/* No framebuffer readback during ordinary drawing/presentation. */
int lhx_gpu_draw(LhxGpu *,const LhxSceneFrame *,const uint8_t palette[768],
                 const uint32_t overlay[64000],const LhxSceneRenderOptions *);
void lhx_gpu_present(LhxGpu *);
int lhx_gpu_read_pixels(LhxGpu *,uint32_t *pixels);
/* GPU timer queries are asynchronous during play; reporting waits at exit only. */
void lhx_gpu_report(LhxGpu *,FILE *);
