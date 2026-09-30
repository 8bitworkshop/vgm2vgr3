/* test_fade.c -- host test for mixer fade (built with -DVGR3_MIXER -DVGR3_FADE).
 *
 * usage: test_fade music.vgr3 sfx.vgr3 low|sn|gb vol-reg ...
 *
 * Scenarios, each checked frame by frame against an independent
 * Vgr3Player plus a reimplementation of the fade counter:
 *   A: music fades to silence and the layer stops
 *   B: fade started, then cancelled: volumes come back to the music's
 *   C: an sfx overlays a fading music; the sfx's bytes are never faded,
 *      and on release the music's volumes come back attenuated
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
static uint8_t volMask[NB], maskS[NB], wrote[NB], pending[NB];
static Vgr3FadeOps ops;
static uint8_t *music, *sfx;

/* scenario: fade at frame `fadeAt` with `step`; optional cancel frame;
 * optional sfx window [sfxOn, sfxOff) */
static int run(const char *name, int fadeAt, int step, int cancelAt, int sfxOn, int sfxOff) {
    uint8_t hw[VGR3_MAX_REGS];
    int f, r, i, errors = 0, lvl = 0, left = 0, fading = 0, stopped = 0, sfxLive = 0;
    const uint8_t *ct;

    memset(hw, 0, sizeof(hw));
    memset(wrote, 0, sizeof(wrote));
    memset(pending, 0, sizeof(pending));
    vgr3MixerInit(&mx, music);
    vgr3MixerSetFadeOps(&mx, &ops);
    vgr3Init(&pm, music);
    memset(maskS, 0, sizeof(maskS));
    ct = sfx + VGR3_HEADER_SIZE;
    for (i = 0; i < sfx[7]; i++, ct += VGR3_CHAN_SIZE)
        for (r = ct[0]; r < ct[0] + ct[1]; r++) maskS[r >> 3] |= (uint8_t)(1 << (r & 7));

    for (f = 0; f < 400 && errors < 10; f++) {
        if (f == fadeAt) { vgr3MixerFade(&mx, 0, (uint8_t)step); fading = 1; lvl = 0; left = step; }
        if (f == cancelAt) { vgr3MixerFadeCancel(&mx, 0); fading = 0; }
        if (f == sfxOn) {
            vgr3MixerPlay(&mx, 1, sfx); vgr3Init(&ps, sfx);
            memset(wrote, 0, sizeof(wrote));
            sfxLive = 1;
        }
        if (f == sfxOff) {
            vgr3MixerStop(&mx, 1);
            for (r = 0; r < VGR3_MAX_REGS; r++)
                if (mx.seMask && bit(maskS, r) && bit(mx.seMask, r)) pending[r >> 3] |= (uint8_t)(1 << (r & 7));
            sfxLive = 0;
        }
        if (fading && !stopped) {
            /* mirror the tick that vgr3MixerFrame does at the top of the merge */
            if (--left == 0) {
                left = step;
                if (++lvl == 16) { stopped = 1; fading = 0; }
            }
        }
        vgr3MixerFrame(&mx);
        for (r = 0; r < VGR3_MAX_REGS; r++)
            if (bit(mx.outDirty, r)) hw[r] = mx.outRegs[r];
        memset(mx.outDirty, 0, sizeof(mx.outDirty));

        vgr3Frame(&pm);
        if (sfxLive) {
            vgr3Frame(&ps);
            for (i = 0; i < NB; i++) wrote[i] |= ps.dirty[i];
            if (reapEnded(&ps, wrote)) sfxLive = 0;
        }
        for (r = 0; r < VGR3_MAX_REGS; r++) {
            int want, mask = 0xFF, isVol = bit(volMask, r);
            if (bit(pm.dirty, r)) pending[r >> 3] &= (uint8_t)~(1 << (r & 7));
            if (sfxLive && bit(wrote, r)) {
                want = ps.regs[r];
            } else if (bit(pending, r)) {
                continue;
            } else {
                want = pm.regs[r];
                if (isVol && (fading || stopped)) want = ops.atten((uint8_t)(stopped ? 15 : lvl), (uint8_t)r, (uint8_t)want);
                if (stopped) {
                    if (!isVol) continue;       /* music is gone: nothing else to compare */
                    mask = 0x0F;
                }
            }
            if (((hw[r] ^ want) & mask) != 0) {
                fprintf(stderr, "%s: frame %d reg 0x%02X: hw %02X want %02X (level %d)\n", name, f, r, hw[r], want, lvl);
                errors++;
            }
        }
        memset(pm.dirty, 0, sizeof(pm.dirty));
        memset(ps.dirty, 0, sizeof(ps.dirty));
    }
    if (fadeAt >= 0 && step * 16 + fadeAt < 400 && cancelAt < 0 && !stopped) {
        fprintf(stderr, "%s: fade never finished\n", name);
        errors++;
    }
    printf("%s: %s\n", name, errors ? "FAIL" : "PASS");
    return errors;
}

int main(int argc, char **argv) {
    int i, errors = 0;
    if (argc < 5) { fprintf(stderr, "usage: %s music.vgr3 sfx.vgr3 low|sn|gb vol-reg ...\n", argv[0]); return 2; }
    music = slurp(argv[1]);
    sfx = slurp(argv[2]);
    ops.atten = !strcmp(argv[3], "sn") ? vgr3AttenSN : !strcmp(argv[3], "gb") ? vgr3AttenGB : vgr3AttenLow4;
    ops.volMask = volMask;
    for (i = 4; i < argc; i++) { int r = (int)strtol(argv[i], 0, 0); volMask[r >> 3] |= (uint8_t)(1 << (r & 7)); }

    errors += run("A fade out", 100, 3, -1, -1, -1);
    errors += run("B cancel", 100, 4, 130, -1, -1);
    errors += run("C overlay", 100, 5, -1, 110, 150);
    return errors != 0;
}
