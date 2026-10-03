#!/usr/bin/env python3
"""bws_check.py -- run a player under the 8bitworkshop CLI emulator and check
its shadow registers against the host decoder, frame by frame.

usage: test/bws_check.py [-n FRAMES] [--bws DIR] PLATFORM [song.vgm]

Stages players/vgr3_<name>.c, the decoder and the encoded song in a scratch
dir (the repo is not touched), with `static` removed from g_player so the
symbol is visible, builds and runs it with 8bws, and after every emulated
frame reads g_player.regs. The same song is run through test/dump_regs on the
host. The regs are sampled each time vgr3Frame() returns (break + out), so target
call k is compared with host frame k; a start-up call skipped or added on the
target would show as a skew, which is searched for (0..MAXSKEW) and reported.

Mixer mode (platform "nes-mixer"): stages nes/mixdemo.c, which plays music on
layer 0 and overlays an SFX (play at call 20, stop at 90, music fade at 120),
and compares g_mixer.outRegs -- the merged, faded output the glue flushes --
against test/dump_mixer.c running the same schedule on the host.

Needs the sandbox to allow reading/writing ~/PuzzlingPlans/8bitworkshop.
"""
import argparse, os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
NREGS = 0x30            # VGR3_MAX_REGS default; the glue builds use it
MAXSKEW = 16

# platform -> (8bws platform id, glue, default song, pointer size, embedded name)
PLAYERS = {
    "coleco": ("coleco", "vgr3_coleco.c", "samples/sn76489/nightmarket.vgm", 2, "nightmarket.vgr3"),
    "gb":     ("gb", "vgr3_gb.c", "samples/gb/ninjag.vgm", 2, "ninjag.vgr3"),
    "msx":    ("msx-libcv", "vgr3_msx.c", "samples/ay8910/map.vgm", 2, "map.vgr3"),
    "sid":    ("c64", "vgr3_sid.c", "samples/sid/ringout-sid.vgm", 2, "ringout-sid.vgr3"),
    "pokey":  ("atari8-800", "vgr3_pokey.c", "samples/pokey/commando02.vgm", 2, "commando02.vgr3"),
    "pokey5200": ("atari8-5200", "vgr3_pokey.c", "samples/pokey/commando02.vgm", 2, "commando02.vgr3"),
}

# Mixer targets: 8bws platform, glue (relative to ROOT), (music vgm, embed name),
# (sfx vgm, embed name), pointer size, mixer build settings.
MIXERS = {
    "nes-mixer": dict(plat="nes", glue="nes/mixdemo.c", ptr=2,
                      music=("samples/nes/eiffel-nes.vgm", "eiffel.vgr3", True),
                      sfx=("samples/nes/famitune-nes.vgm", "sfx.vgr3", False),
                      regs=0x18, layers=3, chans=16),
}

def outregs_offset(m):
    """Offset of Vgr3Mixer.outRegs on a target with no struct padding (cc65,
    sdcc): layer[] (data, dict, regs, dirty, mask), chans[] (pc, base, width,
    k, wait, wmask, sp, ctx, ended, stack[VGR3_MAX_DEPTH]), active, chanTop."""
    ptr, r = m["ptr"], m["regs"]
    layer = 2 * ptr + r + r // 8 + r // 8
    chan = ptr + 5 + ptr + ptr + 1 + 4 * (ptr + 1)
    return m["layers"] * layer + m["chans"] * chan + 2

def sh(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def emulate(a, plat, glue, frame_sym, state_sym, nbytes, n):
    """Runs the glue under 8bws; returns n memory dumps (lists of hex byte
    strings) of `state_sym`, one each time `frame_sym` returns."""
    script = "; ".join("break %s 300; out; mem %s %d" % (frame_sym, state_sym, nbytes) for _ in range(n))
    r = sh(["node", "gen/tools/8bws.js", "run", "-p", plat, glue, "-e", script], cwd=a.bws)
    out = re.sub(r"\x1b\[[0-9;]*m", "", r.stdout + r.stderr)
    if "FAIL" in out and "mem" in out:
        sys.exit("8bws failed:\n" + out[-1500:])
    # each mem dump: a header line, then "ADDR: hh hh ... ascii" lines
    dumps, cur = [], None
    for line in out.splitlines():
        if re.match(r"\[\d+:\d+\] mem ", line):
            cur = []; dumps.append(cur)
        elif cur is not None:
            m = re.match(r"^[0-9A-Fa-f]{4}: (.*)", line)
            if m:   # hex bytes (a 2-space gutter mid-line), then 3+ spaces, then ascii
                cur.extend(re.split(r"\s{3,}", m.group(1))[0].split())
    if len(dumps) != n: sys.exit("expected %d dumps, got %d\n%s" % (n, len(dumps), out[-1500:]))
    return dumps

def compare(a, tgt, host):
    best = None
    for skew in range(MAXSKEW + 1):
        bad = [i for i in range(a.frames) if tgt[i + skew] != host[i]]
        if best is None or len(bad) < len(best[1]): best = (skew, bad)
    skew, bad = best
    print("%s: %d frames compared, target frame %d = host frame 0 (skew), %d mismatches"
          % (a.platform, a.frames, skew, len(bad)))
    for i in bad[:5]:
        print("  host %d: %s\n  tgt  %d: %s" % (i, host[i], i + skew, tgt[i + skew]))
    print("PASS" if not bad else "FAIL")
    sys.exit(1 if bad else 0)

def mixer(a):
    m = MIXERS[a.platform]
    tmp = tempfile.mkdtemp(prefix="bwschk-")
    try:
        enc = os.path.join(ROOT, "vgm2vgr3")
        files = []
        for vgm, name, loop in (m["music"], m["sfx"]):
            dst = os.path.join(tmp, name)
            r = sh([enc] + (["--loop"] if loop else []) + [os.path.join(ROOT, vgm), dst])
            if r.returncode: sys.exit("encode failed:\n" + r.stdout + r.stderr)
            files.append(dst)
        for f in ("vgr3_play.c", "vgr3_play.h", "vgr3_format.h", m["glue"]):
            shutil.copy(os.path.join(ROOT, f), tmp)
        glue = os.path.join(tmp, os.path.basename(m["glue"]))

        dm = os.path.join(tmp, "dump_mixer")
        r = sh(["cc", "-O1", "-std=c99", "-I" + ROOT, "-o", dm, os.path.join(ROOT, "test/dump_mixer.c")])
        if r.returncode: sys.exit("host build failed:\n" + r.stderr)
        n = a.frames + MAXSKEW
        host = sh([dm] + files + [str(n)]).stdout.split()

        off = outregs_offset(m)
        dumps = emulate(a, m["plat"], glue, "_vgr3MixerFrame", "_g_mixer", off + m["regs"], n)
        tgt = ["".join(d[off:off + m["regs"]]).lower() for d in dumps]
        compare(a, tgt, host)
    finally:
        if a.keep: print("scratch:", tmp)
        else: shutil.rmtree(tmp, ignore_errors=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-n", "--frames", type=int, help="default 120; 200 for mixer targets (covers the fade)")
    ap.add_argument("--bws", default=os.path.expanduser("~/PuzzlingPlans/8bitworkshop"))
    ap.add_argument("--keep", action="store_true", help="keep the scratch dir")
    ap.add_argument("platform", choices=sorted(PLAYERS) + sorted(MIXERS))
    ap.add_argument("song", nargs="?")
    a = ap.parse_args()
    if a.platform in MIXERS:
        a.frames = a.frames or 200
        return mixer(a)
    a.frames = a.frames or 120
    plat, glue, song, ptr, embed = PLAYERS[a.platform]
    song = os.path.join(ROOT, a.song or song)

    tmp = tempfile.mkdtemp(prefix="bwschk-")
    try:
        enc = os.path.join(ROOT, "vgm2vgr3")
        vgr3 = os.path.join(tmp, embed)
        r = sh([enc, "--loop", song, vgr3])
        if r.returncode: sys.exit("encode failed:\n" + r.stdout + r.stderr)
        for f in ("vgr3_play.c", "vgr3_play.h", "vgr3_format.h"):
            shutil.copy(os.path.join(ROOT, f), tmp)
        src = open(os.path.join(ROOT, "players", glue)).read()
        src = re.sub(r"^static (Vgr3Player g_player)", r"\1", src, flags=re.M)
        if "Vgr3Player g_player" not in src: sys.exit("glue has no g_player")
        open(os.path.join(tmp, glue), "w").write(src)

        dr = os.path.join(tmp, "dump_regs")
        r = sh(["cc", "-O1", "-std=c99", "-I" + ROOT, "-o", dr, os.path.join(ROOT, "test/dump_regs.c"),
                os.path.join(ROOT, "vgr3_play.c")])
        if r.returncode: sys.exit("host build failed:\n" + r.stderr)
        n = a.frames + MAXSKEW
        host = sh([dr, vgr3, str(n)]).stdout.split()

        nbytes = 2 * ptr + NREGS
        # sample when vgr3Frame() returns, not at a video frame boundary: the
        # vint handler can straddle the boundary and leave regs half-updated
        dumps = emulate(a, plat, os.path.join(tmp, glue), "_vgr3Frame", "_g_player", nbytes, n)
        tgt = ["".join(d[2 * ptr:2 * ptr + NREGS]).lower() for d in dumps]

        compare(a, tgt, host)
    finally:
        if a.keep: print("scratch:", tmp)
        else: shutil.rmtree(tmp, ignore_errors=True)

main()
