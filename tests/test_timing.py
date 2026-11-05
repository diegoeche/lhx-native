"""Check native frame deadlines and timer/input servicing while waiting."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class FrameTiming(unittest.TestCase):
    def test_deadlines_and_runtime_wait(self):
        runtime = (ROOT / "runtime/main.c").read_text()
        pace = runtime[runtime.index("void lhx_frame_pace("):
                       runtime.index("\nstatic void init_machine(")]
        source = r'''
#include <assert.h>
#include <stdint.h>
#include "frame_clock.h"
typedef int CPU;
static LhxFrameClock flight_clock;
static uint64_t flight_frames, first_flight_us, last_flight_us;
static uint64_t virtual_us, host_us, timer_us, last_poll_us;
static unsigned delay_calls, polls;
static int headless;
static uint64_t now_us(void) { return headless ? virtual_us : host_us; }
static void SDL_Delay(unsigned ms) {
    assert(ms >= 1 && ms <= 2);
    host_us += ms * 1000;
    delay_calls++;
}
static void poll_services(CPU *c) {
    (void)c;
    if (!headless && polls) assert(now_us()-last_poll_us <= 2100);
    timer_us = last_poll_us = now_us();
    polls++;
}
'''
        source += pace
        source += r'''
static void present_deadlines(void) {
    assert(lhx_present_deadline(0,1000)==16667);
    assert(lhx_present_deadline(0,16000)==18000);
    assert(lhx_present_deadline(0,50000)==52000);
    /* A 50 ms draw previously left the next display already overdue. Several
       timer/input polls must now run before another draw can be selected. */
    uint64_t due=lhx_present_deadline(100000,150000);
    unsigned services=0;for(uint64_t t=150000;t<due;t+=250)services++;
    assert(services==8);
    assert(lhx_present_deadline(UINT64_C(6000000000),UINT64_C(6000050000))==UINT64_C(6000052000));
    assert(lhx_present_deadline(UINT64_MAX-100,UINT64_MAX)==UINT64_MAX);
}
static void deadlines(void) {
    /* Fractional frame periods must not accumulate integer-rounding drift,
       including after the clock has passed 32-bit microsecond range. */
    const unsigned rates[] = {10, 30, 60, 75, 120};
    const uint64_t epoch = UINT64_C(6000000000);
    for(unsigned r=0; r<sizeof(rates)/sizeof(*rates); r++) {
        LhxFrameClock clock={.rate=rates[r]};
        for(unsigned frame=0; frame<=rates[r]*3600; frame++) {
            uint64_t at=lhx_frame_clock_next(&clock,epoch);
            assert(at == epoch+(uint64_t)frame*1000000/rates[r]);
        }
        /* A stall starts a new schedule; no rapid catch-up frames follow. */
        uint64_t late=clock.next_us+2000000;
        assert(lhx_frame_clock_next(&clock,late)==late);
        assert(lhx_frame_clock_next(&clock,late)>late);
    }
}
static void runtime_wait(int virtual_clock, unsigned rate) {
    headless=virtual_clock;
    flight_clock=(LhxFrameClock){.rate=rate};
    flight_frames=first_flight_us=last_flight_us=0;
    virtual_us=host_us=timer_us=last_poll_us=0;
    delay_calls=polls=0;
    CPU c=0;
    for(unsigned frame=0; frame<=rate; frame++) {
        lhx_frame_pace(&c);
        /* The original game must see fresh IRQ/input state when it resumes. */
        assert(timer_us==now_us());
        if(headless) virtual_us+=100; else host_us+=100;
    }
    assert(flight_frames==rate+1);
    assert(last_flight_us-first_flight_us>=1000000);
    assert(last_flight_us-first_flight_us<1002000);
    assert(polls>=rate+1);
    if(headless) assert(delay_calls==0); else assert(delay_calls>0);
}
int main(void) {
    deadlines();present_deadlines();
    runtime_wait(0,30); runtime_wait(0,60); runtime_wait(0,120);
    runtime_wait(1,30); runtime_wait(1,60); runtime_wait(1,120);
}
'''
        with tempfile.TemporaryDirectory(prefix="lhx-timing-") as directory:
            tmp = Path(directory)
            src, exe = tmp / "timing.c", tmp / "timing"
            src.write_text(source)
            compiled = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(ROOT / "runtime"),
                str(src), str(ROOT / "runtime/frame_clock.c"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
