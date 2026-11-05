#pragma once
#include <stdint.h>

typedef struct {
  uint64_t next_us;
  unsigned rate, fraction;
  int started;
} LhxFrameClock;

/* Return this frame's deadline and schedule the following one. Late frames
 * restart the schedule rather than accumulating a burst of catch-up updates. */
uint64_t lhx_frame_clock_next(LhxFrameClock *, uint64_t now_us);

/* Keep the 60 Hz display schedule when drawing fits. After a slow draw leave
 * 2 ms for input/timer/simulation work instead of redrawing immediately. */
uint64_t lhx_present_deadline(uint64_t started_us,uint64_t finished_us);
