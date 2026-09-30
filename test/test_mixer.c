/* test_mixer.c -- host test for the VGR3 mixer (built with -DVGR3_MIXER).
 *
 * usage: test_mixer music.vgr3 sfx.vgr3
 *
 * Plays `music` as layer 0 and `sfx` as layer 1 with a schedule of
 * start / replace / stop events, and checks the merged output, as
 * applied to a simulated chip, against two independent Vgr3Players:
 *   - bytes the sfx has not written always equal the music's shadow (an
 *     overlay never disturbs voices, or bytes, it hasn't touched);
 *   - bytes the sfx has written equal the sfx's shadow while it plays;
 *   - after the sfx stops, held bytes are back to the music's shadow,
 *     including trigger bytes (only bytes in the optional mx.seMask are left
 *     alone until the music next writes them; NULL by default).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vgr3_play.h"

#define NB (VGR3_MAX_REGS / 8)

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

static int bit(const uint8_t *a, int r) { return a[r >> 3] >> (r & 7) & 1; }

static void windowMask(const uint8_t *file, uint8_t *mask) {
    const uint8_t *ct = file + VGR3_HEADER_SIZE;
    int n = file[7], i, r;
    memset(mask, 0, NB);
    for (i = 0; i < n; i++, ct += VGR3_CHAN_SIZE)
        for (r = ct[0]; r < ct[0] + ct[1]; r++) mask[r >> 3] |= (uint8_t)(1 << (r & 7));
}

/* Reference model of END: a parked sfx channel gives its window back to the
 * music; when every channel has parked the overlay is over. Returns 1 then. */
static int reapEnded(Vgr3Player *ps, uint8_t *wrote) {
    int k, r, all = 1;
    for (k = 0; k < ps->numChans; k++) {
        Vgr3Chan *c = &ps->chans[k];
        if (c->ended == 1) {
            for (r = c->base; r < c->base + c->width; r++) wrote[r >> 3] &= (uint8_t)~(1 << (r & 7));
            c->ended = 2;
        }
        if (c->ended != 2) all = 0;
    }
    return all;
}

static Vgr3Mixer mx;
static Vgr3Player pm, ps;

int main(int argc, char **argv) {
    uint8_t *music, *sfx;
    uint8_t maskS[NB], hw[VGR3_MAX_REGS], wrote[NB], pending[NB];
    int sfxOn = 0, f, r, errors = 0, i;
    static const struct { int frame, op; } ev[] = {   /* op 1 = play, 0 = stop */
        {200, 1}, {500, 0}, {800, 1}, {900, 1}, {1100, 0}
    };
    int nev = (int)(sizeof(ev) / sizeof(ev[0])), e = 0;

    if (argc != 3) { fprintf(stderr, "usage: %s music.vgr3 sfx.vgr3 \n", argv[0]); return 2; }
    music = slurp(argv[1]);
    sfx = slurp(argv[2]);

    if (!vgr3MixerInit(&mx, music) || !vgr3Init(&pm, music)) { fprintf(stderr, "bad music file\n"); return 2; }
    memset(hw, 0, sizeof(hw));
    memset(wrote, 0, sizeof(wrote));
    memset(pending, 0, sizeof(pending));
    windowMask(sfx, maskS);

    for (f = 0; f < 1500 && errors < 10; f++) {
        while (e < nev && ev[e].frame == f) {
            if (sfxOn) {
                /* stop/replace: bytes leave the sfx; SE ones wait for the music */
                for (r = 0; r < VGR3_MAX_REGS; r++)
                    if (mx.seMask && bit(maskS, r) && bit(mx.seMask, r)) pending[r >> 3] |= (uint8_t)(1 << (r & 7));
                sfxOn = 0;
            }
            if (ev[e].op) {
                if (!vgr3MixerPlay(&mx, 1, sfx) || !vgr3Init(&ps, sfx)) { fprintf(stderr, "bad sfx file\n"); return 2; }
                memset(wrote, 0, sizeof(wrote));
                sfxOn = 1;
            } else {
                vgr3MixerStop(&mx, 1);
            }
            e++;
        }
        vgr3MixerFrame(&mx);
        for (r = 0; r < VGR3_MAX_REGS; r++)
            if (bit(mx.outDirty, r)) hw[r] = mx.outRegs[r];
        memset(mx.outDirty, 0, sizeof(mx.outDirty));

        vgr3Frame(&pm);
        if (sfxOn) {
            vgr3Frame(&ps);
            for (i = 0; i < NB; i++) wrote[i] |= ps.dirty[i];
            if (reapEnded(&ps, wrote)) sfxOn = 0;
        }
        for (r = 0; r < VGR3_MAX_REGS; r++) {
            int want = -1;
            if (bit(pm.dirty, r)) pending[r >> 3] &= (uint8_t)~(1 << (r & 7));
            if (sfxOn && bit(wrote, r)) {
                want = ps.regs[r];      /* the sfx owns a byte once it has written it */
            } else if (!bit(pending, r)) {
                want = pm.regs[r];
            }
            if (want >= 0 && hw[r] != want) {
                fprintf(stderr, "frame %d reg 0x%02X: hw %02X want %02X (%s)\n", f, r, hw[r], want,
                        sfxOn && bit(wrote, r) ? "sfx" : "music");
                errors++;
            }
        }
        memset(pm.dirty, 0, sizeof(pm.dirty));
        memset(ps.dirty, 0, sizeof(ps.dirty));
    }
    printf("%s: %s\n", argv[2], errors ? "FAIL" : "PASS");
    return errors != 0;
}
