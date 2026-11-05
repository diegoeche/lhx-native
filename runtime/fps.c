#include "fps.h"
#include <stdio.h>
#include <string.h>

void lhx_fps_update(LhxFpsCounter *counter, uint64_t now_us, uint64_t frames) {
  if (!counter->initialized || now_us < counter->start_us ||
      frames < counter->start_frames) {
    *counter = (LhxFpsCounter){
      .start_us = now_us, .start_frames = frames, .initialized = 1
    };
    return;
  }
  uint64_t elapsed = now_us - counter->start_us;
  if (elapsed >= 1000000) {
    counter->rate = (double)(frames - counter->start_frames) * 1000000.0 / elapsed;
    counter->ready = 1;
    counter->start_us = now_us;
    counter->start_frames = frames;
  }
}

/* Small host-side font; no game palette or framebuffer bytes are changed. */
static const uint8_t glyphs[][7] = {
  {14,17,17,17,17,17,14}, {4,12,4,4,4,4,14},
  {14,17,1,2,4,8,31}, {30,1,1,14,1,1,30},
  {2,6,10,18,31,2,2}, {31,16,16,30,1,1,30},
  {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
  {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14},
  {31,16,16,30,16,16,16}, /* F */
  {30,17,17,30,16,16,16}, /* P */
  {15,16,16,14,1,1,30},   /* S */
  {0,0,0,0,0,12,12},     /* . */
  {0,0,0,31,0,0,0},      /* - */
  {0,0,0,0,0,0,0}        /* space */
};

static const uint8_t *glyph(char c) {
  if (c >= '0' && c <= '9') return glyphs[c - '0'];
  switch (c) {
  case 'F': return glyphs[10];
  case 'P': return glyphs[11];
  case 'S': return glyphs[12];
  case '.': return glyphs[13];
  case '-': return glyphs[14];
  default: return glyphs[15];
  }
}

void lhx_fps_overlay(const LhxFpsCounter *counter, uint32_t *pixels,
                     unsigned width, unsigned height) {
  lhx_fps_overlay_scaled(counter, pixels, width, height, 1);
}

void lhx_fps_overlay_scaled(const LhxFpsCounter *counter, uint32_t *pixels,
                            unsigned width, unsigned height, unsigned scale) {
  if (!scale || scale > 8)
    return;
  char label[32];
  if (counter->ready && counter->rate > 0)
    snprintf(label, sizeof(label), "FPS %.1f", counter->rate);
  else
    snprintf(label, sizeof(label), "FPS --");
  unsigned length = (unsigned)strlen(label), box_width = (length * 6 + 5) * scale;
  if (width < box_width + 6 * scale || height < 25 * scale)
    return;
  unsigned left = width - box_width - 3 * scale, top = 10 * scale;
  for (unsigned y = 0; y < 13 * scale; ++y)
    for (unsigned x = 0; x < box_width; ++x)
      pixels[(top + y) * width + left + x] = 0xff000000;
  for (unsigned i = 0; i < length; ++i) {
    const uint8_t *rows = glyph(label[i]);
    for (unsigned y = 0; y < 7; ++y)
      for (unsigned x = 0; x < 5; ++x)
        if (rows[y] & (1u << (4 - x)))
          for (unsigned dy = 0; dy < scale; ++dy)
            for (unsigned dx = 0; dx < scale; ++dx)
              pixels[(top + (3 + y) * scale + dy) * width +
                     left + (3 + i * 6 + x) * scale + dx] = 0xffeeeeee;
  }
}
