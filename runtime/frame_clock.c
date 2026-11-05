#include "frame_clock.h"

uint64_t lhx_frame_clock_next(LhxFrameClock *clock, uint64_t now_us) {
  uint64_t deadline = clock->started && clock->next_us > now_us
                          ? clock->next_us : now_us;
  unsigned period = 1000000 + clock->fraction;
  clock->next_us = deadline + period / clock->rate;
  clock->fraction = period % clock->rate;
  clock->started = 1;
  return deadline;
}

uint64_t lhx_present_deadline(uint64_t started_us,uint64_t finished_us) {
  uint64_t normal=started_us>UINT64_MAX-16667?UINT64_MAX:started_us+16667;
  uint64_t services=finished_us>UINT64_MAX-2000?UINT64_MAX:finished_us+2000;
  return normal>services?normal:services;
}
