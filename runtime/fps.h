#pragma once
#include <stdint.h>

typedef struct {
  uint64_t start_us, start_frames;
  double rate;
  int initialized, ready;
} LhxFpsCounter;

/* Sample original game iterations, independently of host display repaints. */
void lhx_fps_update(LhxFpsCounter *, uint64_t now_us, uint64_t game_frames);
void lhx_fps_overlay(const LhxFpsCounter *, uint32_t *pixels,
                     unsigned width, unsigned height);
void lhx_fps_overlay_scaled(const LhxFpsCounter *, uint32_t *pixels,
                            unsigned width, unsigned height, unsigned scale);
