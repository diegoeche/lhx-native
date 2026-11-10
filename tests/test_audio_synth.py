"""Check procedural audio without original game files or an audio device."""
from array import array
import math
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def power(samples, frequency, rate=48000):
    coefficient = 2 * math.cos(2 * math.pi * frequency / rate)
    a = b = 0.0
    for sample in samples:
        current = sample + coefficient * a - b
        b, a = a, current
    return (a*a + b*b - coefficient*a*b) / len(samples)**2


@unittest.skipUnless(shutil.which("cc"), "C compiler required")
class AudioSynth(unittest.TestCase):
    def test_gun_repeat_edges_body_and_decay(self):
        with tempfile.TemporaryDirectory(prefix="lhx-gun-synth-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            binary = work / "gun"
            result = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "runtime"),
                "-I", str(ROOT / "third_party/pcrecomp/runtime"),
                str(ROOT / "tests/audio_synth_gun.c"),
                str(ROOT / "runtime/audio_synth.c"), "-lm", "-o", str(binary),
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), str(work)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            def load(name):
                samples = array("h")
                samples.frombytes((work / f"{name}.raw").read_bytes())
                return samples[::2]

            def rms(samples):
                return math.sqrt(sum(x*x for x in samples) / len(samples))

            single = load("gun-single")
            # A substantial low body followed by a decaying tail, not a
            # sustained high tone or noise bed. Broad bands avoid tuning the
            # test to a particular oscillator frequency or random sample.
            body = sum(power(single[:4800], hz) for hz in range(60, 241, 20))
            treble = sum(power(single[:4800], hz) for hz in range(1000, 2801, 200))
            self.assertGreater(body, 5 * treble)
            self.assertGreater(rms(single[:960]), 3 * rms(single[3840:4800]))
            burst = load("gun-burst")
            for shot in range(10):
                start = shot * 4800
                self.assertGreater(rms(burst[start:start+960]), 1000)
                self.assertGreater(rms(burst[start:start+960]),
                                   3 * rms(burst[start+3840:start+4800]))
            stop = load("gun-stop")
            self.assertLess(rms(stop[2400:4800]), rms(stop[:2400]) / 10)

    def test_polyphony_stereo_envelopes_and_limits(self):
        with tempfile.TemporaryDirectory(prefix="lhx-audio-synth-") as directory:
            work = Path(directory)
            (work / "recomp_all.h").write_text("")
            source = work / "check.c"
            source.write_text(r'''
#include "audio_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static int16_t pcm[96000], other[96000];
static void save(const char *directory,const char *name) {
    char path[4096];snprintf(path,sizeof path,"%s/%s.raw",directory,name);
    FILE *file=fopen(path,"wb");assert(file);
    assert(fwrite(pcm,sizeof *pcm,96000,file)==96000);fclose(file);
}
static void tone(LhxSound *sound,LhxAudioSnapshot *snapshot) {
    lhx_sound_init(sound,48000);lhx_sound_submit(snapshot,sound);
    lhx_sound_render(sound,pcm,4800,2); /* settle the attack/pitch */
    lhx_sound_render(sound,pcm,48000,2);
}
int main(int argc,char **argv) {
    assert(argc==2);
    LhxSound sound, second;LhxAudioSnapshot snapshot={0};
    lhx_sound_init(&sound,48000);
    assert(atomic_is_lock_free(&sound.targets[0]));
    lhx_sound_render(&sound,pcm,48000,2);
    for(unsigned i=0;i<96000;i++)assert(pcm[i]==0);
    snapshot.voices[0]=(LhxAudioVoice){1193,3,0,1,1};
    tone(&sound,&snapshot);save(argv[1],"tone");
    snapshot.voices[0].active=0;
    lhx_sound_submit(&snapshot,&sound);
    lhx_sound_render(&sound,pcm,48000,2);save(argv[1],"note-release");
    snapshot.voices[1]=(LhxAudioVoice){1193,3,0,1,1};
    snapshot.voices[2]=(LhxAudioVoice){1999,4,0,1,1};
    tone(&sound,&snapshot);save(argv[1],"mix");
    memset(&snapshot,0,sizeof snapshot);
    snapshot.voices[3]=(LhxAudioVoice){65,0,0,1,1};
    tone(&sound,&snapshot);save(argv[1],"slow");
    snapshot.voices[3].divisor=5;
    tone(&sound,&snapshot);save(argv[1],"fast");
    snapshot.voices[2]=(LhxAudioVoice){400,15,255,1,1};
    lhx_sound_init(&sound,48000);lhx_sound_init(&second,48000);
    lhx_sound_submit(&snapshot,&sound);lhx_sound_submit(&snapshot,&second);
    lhx_sound_render(&sound,pcm,1024,2);
    lhx_sound_render(&second,other,300,2);
    lhx_sound_render(&second,other+600,724,2);
    assert(!memcmp(pcm,other,2048*sizeof *pcm));
    memset(&snapshot,0,sizeof snapshot);lhx_sound_submit(&snapshot,&sound);
    lhx_sound_render(&sound,pcm,48000,2);save(argv[1],"release");
    for(unsigned i=20000;i<96000;i++)assert(pcm[i]==0);
    static const unsigned rates[]={0,8000,44100,48000,96000,192000};
    for(unsigned rate=0;rate<sizeof rates/sizeof *rates;rate++) {
        lhx_sound_init(&sound,rates[rate]);
        for(unsigned j=0;j<4;j++)snapshot.voices[j]=(LhxAudioVoice){j%2?1:65535,j?15:0,255,1,~0u};
        lhx_sound_submit(&snapshot,&sound);
        for(unsigned channels=1;channels<=4;channels++) {
            int16_t guarded[514];guarded[0]=123;guarded[513]=456;
            lhx_sound_render(&sound,guarded+1,128,channels);
            assert(guarded[0]==123 && guarded[513]==456);
            for(unsigned i=0;i<128*channels;i++)assert(abs(guarded[i+1])<=20900);
        }
        int16_t untouched=17;lhx_sound_render(&sound,&untouched,100,0);assert(untouched==17);
    }
    puts("polyphony, stereo, silence, chunk continuity, envelopes and limits passed");
}
''')
            binary = work / "check"
            result = subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-I", str(work), "-I", str(ROOT / "runtime"),
                "-I", str(ROOT / "third_party/pcrecomp/runtime"),
                str(source), str(ROOT / "runtime/audio_synth.c"), "-lm", "-o", str(binary)
            ], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            result = subprocess.run([str(binary), str(work)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

            def load(name):
                samples = array("h")
                samples.frombytes((work / f"{name}.raw").read_bytes())
                return samples

            mono = load("tone")
            self.assertEqual(mono[::2], mono[1::2])
            self.assertGreater(power(mono[::2], 1193182 / 1193), 10000)
            release = load("note-release")[::2]
            self.assertGreater(power(release, 1193182 / 1193), 20 * power(release, 100))
            mixed = load("mix")
            left, right = mixed[::2], mixed[1::2]
            self.assertNotEqual(left, right)
            self.assertGreater(power(left, 1193182 / 1193), 10000)
            self.assertGreater(power(left, 1193182 / 1999), 10000)
            self.assertGreater(power(left, 1193182 / 1193), power(right, 1193182 / 1193))
            self.assertGreater(power(right, 1193182 / 1999), power(left, 1193182 / 1999))
            slow, fast = load("slow")[::2], load("fast")[::2]
            self.assertGreater(power(slow, 6), 10 * power(fast, 6))
            self.assertGreater(power(fast, 20), 10 * power(slow, 20))


if __name__ == "__main__":
    unittest.main()
