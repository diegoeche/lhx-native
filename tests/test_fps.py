"""Measure game frames independently of display refreshes and draw the badge."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class FpsCounter(unittest.TestCase):
    def test_actual_game_rate_elapsed_time_and_overlay_bounds(self):
        source = r'''
#include <assert.h>
#include <math.h>
#include <string.h>
#include "fps.h"
static uint32_t pixels[320*200], before[320*200];
int main(void) {
    LhxFpsCounter counter={0};
    lhx_fps_update(&counter,0,0);
    assert(!counter.ready);
    /* A 120 Hz display repainting a 30 FPS game must report 30, not 120. */
    for(unsigned repaint=1;repaint<=120;repaint++)
        lhx_fps_update(&counter,(uint64_t)repaint*1000000/120,repaint/4);
    assert(counter.ready && fabs(counter.rate-30)<0.000001);
    /* Repainting or taking extra screenshots doesn't invent game frames. */
    for(unsigned i=0;i<20;i++)lhx_fps_update(&counter,1000000,30);
    assert(fabs(counter.rate-30)<0.000001);
    /* A delayed sample uses the actual elapsed time, not a fixed one second. */
    lhx_fps_update(&counter,3000000,150);
    assert(fabs(counter.rate-60)<0.000001);
    for(unsigned i=0;i<320*200;i++)pixels[i]=0xff123456;
    lhx_fps_overlay(&counter,pixels,320,200);
    unsigned changed=0,letters=0;
    for(unsigned y=0;y<200;y++)for(unsigned x=0;x<320;x++) {
        if(pixels[y*320+x]!=0xff123456) {
            changed++;
            assert(x>=264 && x<317 && y>=10 && y<23);
        }
        letters+=pixels[y*320+x]==0xffeeeeee;
    }
    assert(changed==53*13 && letters>50);
    memcpy(before,pixels,sizeof(pixels));
    lhx_fps_overlay(&counter,pixels,10,10); /* Small surfaces are untouched. */
    assert(!memcmp(before,pixels,sizeof(pixels)));
    lhx_fps_update(&counter,4000000,150); /* Static/pause interval. */
    assert(counter.ready && counter.rate==0);
    lhx_fps_update(&counter,5000000,0); /* Counter restart. */
    assert(!counter.ready && counter.rate==0);
    lhx_fps_update(&counter,0,0); /* Clock restart. */
    assert(!counter.ready);
}
'''
        with tempfile.TemporaryDirectory(prefix="lhx-fps-") as directory:
            tmp = Path(directory)
            src, exe = tmp / "fps.c", tmp / "fps"
            src.write_text(source)
            built = subprocess.run([
                "cc", "-std=c11", "-O2", "-I", str(ROOT / "runtime"),
                str(src), str(ROOT / "runtime/fps.c"), "-o", str(exe)],
                capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
