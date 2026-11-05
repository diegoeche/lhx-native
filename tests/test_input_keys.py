"""Verify shared keyboard/controller keys emit valid IRQ scancode edges."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class InputKeys(unittest.TestCase):
    def test_independent_sources_and_focus_release(self):
        runtime = (ROOT / "runtime/main.c").read_text()
        functions = runtime[runtime.index("static void queue_key("):
                            runtime.index("static void draw_frame(")]
        source = r'''
#include <assert.h>
#include <stdint.h>
static uint8_t keyq[256], input_sources[256];
static unsigned key_tail;
'''
        source += functions
        source += r'''
int main(void) {
    /* Releasing either source must not release the other's held fire key. */
    input_key(1,0x39,0); assert(key_tail==1 && keyq[0]==0x39);
    controller_key(0x39,0); assert(key_tail==1);
    controller_key(0x39,1); assert(key_tail==1);
    input_key(1,0x39,1); assert(key_tail==2 && keyq[1]==0xb9);
    controller_key(0x39,0); assert(key_tail==3 && keyq[2]==0x39);
    input_key(1,0x39,0); assert(key_tail==3);
    release_physical_keys(); assert(key_tail==3);
    controller_key(0x39,1); assert(key_tail==4 && keyq[3]==0xb9);
    controller_key(0x39,1); assert(key_tail==4);
    /* Extended arrows keep their E0 prefix, including keyboard repeats. */
    input_key(1,0xe048,0); input_key(1,0xe048,0);
    assert(key_tail==8 && keyq[4]==0xe0 && keyq[5]==0x48);
    assert(keyq[6]==0xe0 && keyq[7]==0x48);
    input_key(4,0xe048,0); release_physical_keys(); assert(key_tail==8);
    input_key(4,0xe048,1);
    assert(key_tail==10 && keyq[8]==0xe0 && keyq[9]==0xc8);
    /* Focus loss clears only physical keys; controller and script retain
       their independent ownership until each source releases them. */
    input_key(1,0x1d,0); input_key(1,0x19,0);
    controller_key(0x1d,0); controller_key(0x19,0);
    release_physical_keys(); assert(key_tail==12);
    controller_key(0x19,1); controller_key(0x1d,1);
    assert(key_tail==14 && keyq[12]==0x99 && keyq[13]==0x9d);
    for(unsigned i=0;i<256;i++) assert(!input_sources[i]);
}
'''
        with tempfile.TemporaryDirectory(prefix="lhx-input-keys-") as directory:
            tmp = Path(directory)
            src, exe = tmp / "input.c", tmp / "input"
            src.write_text(source)
            build = subprocess.run(["cc", "-std=c11", "-O2", str(src), "-o", str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, run.stderr)


if __name__ == "__main__":
    unittest.main()
