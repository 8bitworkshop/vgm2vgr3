/* dump_mixer.c -- host reference for test/bws_check.py's mixer mode.
 *
 * usage: dump_mixer music.vgr3 sfx.vgr3 frames
 *
 * Mirrors nes/mixdemo.c: same build settings, same fade ops, same schedule
 * (counted in vgr3MixerFrame calls: 20 SFX plays on layer 1, 90 it stops,
 * 120 the music fades at 1 frame per step). Prints the merged output
 * (outRegs, VGR3_MAX_REGS bytes in hex) after each vgr3MixerFrame call. Keep
 * the settings and schedule in sync with nes/mixdemo.c.
 */
#define VGR3_MIXER
#define VGR3_SINGLE_MIXER
#define VGR3_FADE
#define VGR3_MAX_LAYERS 3
#define VGR3_MAX_CHANS  16
#define VGR3_MAX_REGS   0x18
#include <stdio.h>
#include <stdlib.h>
#include "vgr3_format.h"
#include "vgr3_play.h"
#include "vgr3_play.c"

static const uint8_t nesVolMask[3] = { 0x11, 0x10, 0x00 };
static const Vgr3FadeOps nesFadeOps = { nesVolMask, vgr3AttenLow4 };

static uint8_t *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    long n;
    uint8_t *b;
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) { perror(path); exit(2); }
    fclose(f);
    return b;
}

int main(int argc, char **argv) {
    uint8_t *music, *sfx;
    int frames, n, r;
    if (argc != 4) { fprintf(stderr, "usage: %s music.vgr3 sfx.vgr3 frames\n", argv[0]); return 2; }
    music = slurp(argv[1]);
    sfx = slurp(argv[2]);
    frames = atoi(argv[3]);
    if (!vgr3MixerInit(music)) { fprintf(stderr, "bad music file\n"); return 2; }
    vgr3MixerSetFadeOps(&nesFadeOps);
    for (n = 1; n <= frames; n++) {
        vgr3MixerFrame();
        for (r = 0; r < VGR3_MAX_REGS; r++) printf("%02x", g_mixer.outRegs[r]);
        printf("\n");
        if (n == 20) vgr3MixerPlay(1, sfx);
        if (n == 90) vgr3MixerStop(1);
        if (n == 120) vgr3MixerFade(0, 1);
    }
    return 0;
}
