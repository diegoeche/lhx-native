/* Device-free cannon envelope regression. No original game data required. */
#include "audio_synth.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int16_t pcm[96000], other[96000];

static void save(const char *directory, const char *name)
{
    char path[4096];
    snprintf(path, sizeof path, "%s/%s.raw", directory, name);
    FILE *file = fopen(path, "wb");
    assert(file);
    assert(fwrite(pcm, sizeof *pcm, 96000, file) == 96000);
    fclose(file);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    LhxSound sound, second;
    LhxAudioSnapshot snapshot = {0};
    snapshot.voices[2] = (LhxAudioVoice){620, 15, 98, 1, 1};
    lhx_sound_init(&sound, 48000);
    lhx_sound_submit(&snapshot, &sound);
    lhx_sound_render(&sound, pcm, 48000, 2);
    save(argv[1], "gun-single");
    /* An unchanged active state must not sustain an endless noise bed. */
    for (unsigned i = 48000; i < 96000; ++i) assert(pcm[i] == 0);

    lhx_sound_init(&sound, 48000);
    for (unsigned shot = 0; shot < 10; ++shot) {
        /* The driver's repeat edge may be shorter than an audio block.
         * Submit it without rendering the inactive state, and keep the same
         * API serial throughout the burst. Every edge still needs a body. */
        snapshot.voices[2].active = 0;
        lhx_sound_submit(&snapshot, &sound);
        snapshot.voices[2].active = 1;
        lhx_sound_submit(&snapshot, &sound);
        lhx_sound_render(&sound, pcm + shot*9600, 4800, 2);
    }
    save(argv[1], "gun-burst");

    lhx_sound_init(&sound, 48000);
    lhx_sound_init(&second, 48000);
    lhx_sound_submit(&snapshot, &sound);
    lhx_sound_submit(&snapshot, &second);
    lhx_sound_render(&sound, pcm, 4800, 2);
    lhx_sound_render(&second, other, 4800, 2);
    /* Extending an active gun command is not another envelope onset. */
    ++snapshot.voices[2].serial;
    lhx_sound_submit(&snapshot, &second);
    lhx_sound_render(&sound, pcm, 4800, 2);
    lhx_sound_render(&second, other, 4800, 2);
    assert(!memcmp(pcm, other, 9600 * sizeof *pcm));

    snapshot.voices[2].active = 0;
    lhx_sound_submit(&snapshot, &sound);
    snapshot.voices[2].active = 1;
    lhx_sound_submit(&snapshot, &sound);
    lhx_sound_render(&sound, pcm, 200, 2);
    memset(&snapshot, 0, sizeof snapshot);
    lhx_sound_submit(&snapshot, &sound);
    lhx_sound_render(&sound, pcm, 48000, 2);
    save(argv[1], "gun-stop");
    for (unsigned i = 20000; i < 96000; ++i) assert(pcm[i] == 0);

    static const unsigned rates[] = {8000, 44100, 48000, 96000, 192000};
    for (unsigned rate = 0; rate < sizeof rates / sizeof *rates; ++rate) {
        for (unsigned channels = 1; channels <= 4; ++channels) {
            for (unsigned mixed = 0; mixed < 2; ++mixed) {
                lhx_sound_init(&sound, rates[rate]);
                for (unsigned block = 0; block < 80; ++block) {
                    for (unsigned i = 0; i < LHX_AUDIO_VOICES; ++i)
                        snapshot.voices[i] = (LhxAudioVoice){
                            620 + 220*block, mixed && i ? i-1 : 15, 255,
                            block%5 != 4, block/5
                        };
                    lhx_sound_submit(&snapshot, &sound);
                    lhx_sound_render(&sound, pcm, 1024, channels);
                    for (unsigned i = 0; i < 1024*channels; ++i)
                        assert(abs(pcm[i]) < 20900); /* no final hard clamp */
                }
            }
        }
    }
    puts("gun repeat edges, command extension, decay, stop and mix headroom passed");
}
