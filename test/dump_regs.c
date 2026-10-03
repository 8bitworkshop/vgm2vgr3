/* dump_regs.c -- host reference for test/bws_check.py.
 *
 * usage: dump_regs file.vgr3 frames
 *
 * Runs the decoder and prints the shadow register file (VGR3_MAX_REGS
 * bytes, hex) after each vgr3Frame() call, one line per frame.
 */
#include <stdio.h>
#include <stdlib.h>
#include "vgr3_play.h"

int main(int argc, char **argv) {
    static Vgr3Player p;
    FILE *f;
    long n;
    uint8_t *b;
    int frames, i, r;
    if (argc != 3) { fprintf(stderr, "usage: %s file.vgr3 frames\n", argv[0]); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n);
    if (fread(b, 1, (size_t)n, f) != (size_t)n) return 2;
    fclose(f);
    if (!vgr3Init(&p, b)) { fprintf(stderr, "%s: not playable\n", argv[1]); return 2; }
    frames = atoi(argv[2]);
    for (i = 0; i < frames; i++) {
        vgr3Frame(&p);
        for (r = 0; r < VGR3_MAX_REGS; r++) printf("%02x", p.regs[r]);
        printf("\n");
    }
    return 0;
}
