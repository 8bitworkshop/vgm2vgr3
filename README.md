# vgm2vgr3 / VGR3

A register-based compression format and toolchain for VGM chiptune
captures.

## Why

Most music trackers have a bespoke format for their music files,
requiring bespoke players for each platform.

Instead, you can export a VGM from the tracker (if the tracker supports it).
A VGM file records the precise timing of every write to an audio register.

The raw VGM stream is pretty huge.
It mixes timing, chip selection, and register semantics
together, which makes it awkward to compress well and awkward to play
back on constrained 8-bit targets.

This utility compresses the VGM file into a VGR3 file,
which is much smaller and doesn't need to be decompressed to play.

## The approach

- A channel is just `{base, pc, wait, call stack}`. Ops write bytes into
  the register file and set a dirty bit; the platform glue flushes dirty
  bytes at end of frame in whatever order the chip needs.
- Because the register file is global, one physical voice can be split
  across several channels at zero decoder cost (NES pulse volume/duty vs.
  period, SN76489 tone period low/high bytes, ...). Splits fall on byte
  boundaries and channels never share a byte.
- Repetition is captured with token-level `CALL` backreferences into ROM
  rather than a RAM history window, so the decoder needs no ring buffer
  and no per-song working memory.
- The encoder is self-verifying: it decodes its own output with the real
  playback routine and refuses to write a file that doesn't match the
  source frame by frame.
- For sound effects and fade out, a mixer layer sits on top of the player
  layer.

## File format (`vgr3_format.h`)

Instead of storing the original VGM command stream (a
timestamped log of raw port writes), a `.vgr3` file splits a song into
per-voice *channels*. Each channel owns a contiguous slice of a global
shadow register file and runs its own tiny opcode stream.

```
Header (16 bytes)
Channel table [numChans]        (4 bytes each: base, width, u16 start)
Dict table [dictCount]          (2 bytes each: u16 offset)
<data blob: opcode streams + dictionary entries, back to back>
```

All multi-byte fields are little-endian and all addresses are 16-bit
offsets into the data blob, so a file is capped at 64 KB.

Opcodes (one byte, operands follow):

| Byte | Op | Operands |
|---|---|---|
| `1mmm mwww` | `SET`: mask in the high `K` bits, wait in the low `K` | one value byte per set mask bit, then a `u8` wait if the wait field is 0 |
| `0x00-0x5B` | `DICT`: run the SET at `dict[op]` | -- |
| `0x5C` | `EXT`: prefix for a bulk op | sub-op byte, then its operands |
| `0x5C 0x00` | `EXT`/`LOAD`: write all `W` window bytes (GB wave RAM) | `W` bytes, no wait |
| `0x5D` | `JUMP` (top-level loop) | `u16` addr |
| `0x5E` | `END`: hold forever | -- |
| `0x5F` | `CALLL` | `u16` addr, `u8` count |
| `0x6n` | `CALLS`, count `n+1` | `u8` backward distance from the opcode |
| `0x7n` | `CALLM`, count `n+1` | `u16` addr |

`K` is `7 - W` for channels up to 7 bytes wide; a `SET` with mask 0 is a
pure WAIT. Wider channels have no mask bits and write with `LOAD`. A
`CALL` runs `count` items starting at an address and may itself contain
`CALL`s, up to `VGR3_MAX_DEPTH` (4) levels deep. Every `SET` ends with a
wait >= 1; each frame a channel executes items until one sets a wait.

See `vgr3_format.h` for the register-file layouts per chip.

## Encoder: `vgm2vgr3`

```
vgm2vgr3 [--rate N] [--depth N] [--dict N] [--window N] [--layout N]
         [--no-cross] [--far-penalty N] [--force] [--loop] [-v]
         [--samples out.dpcm] in.vgm out.vgr
```

- `--rate N` (default 60): playback tick rate in Hz. Should match the
  target's vblank rate for tick-synchronous platforms.
- `--depth N` (default 4): maximum nested `CALL` depth.
- `--dict N` (default 92): dictionary entry cap.
- `--window N` / `--no-cross` / `--far-penalty N`: greedy-parse knobs.
  Left unset, the encoder searches a small grid of these per song and
  keeps the smallest result, since the greedy parse is sensitive to them.
- `--layout N`: force one channel layout instead of choosing per voice.
- `--loop`: give a source with no loop point a synthetic loop at frame 0.
- `--force`: write the file even if the self-check fails.
- `--samples out.dpcm`: also dump the NES DPCM data blocks, to be loaded
  at `$C000`.
- `-v`: per-voice/per-setting sizes.

The encoder recognizes SN76489 (latch/data protocol), AY8910, NES APU,
Game Boy DMG (including wave RAM), POKEY, and SID. One chip per file; if
several are present it encodes the first one it knows.

VGM has no dedicated SID chip. DefleMask exports SID as YM2151 register
writes (`0xB6 aa dd`) where `aa` is a SID register `$D400+aa`; the
encoder recognizes that when the YM2151 clock is non-zero and every such
register is in `0x00-0x18`. A real YM2151 (registers `>= 0x20`) is
rejected as unsupported.

## Decoder: `vgr3_play.c` / `vgr3_play.h`

The decoder is
one small per-tick interpreter that knows nothing about any chip; all
chip/platform knowledge lives in a thin platform-specific glue layer on
top of it.

The generic playback core, meant to be dropped onto a target unmodified:

```c
int  vgr3Init(Vgr3Player *p, const uint8_t *file);
void vgr3Frame(Vgr3Player *p);
```

- One `Vgr3Player` holds the shadow `regs[]`, a `dirty[]` bitmap, and one
  `Vgr3Chan` per channel. Fixed-size, no per-song history.
- `vgr3Frame()` must be called exactly once per tick per player (e.g.
  once per vblank). It updates `regs[]` and sets a bit in `dirty[]` for
  every byte written this frame.
- Platform glue then writes the dirty bytes to the hardware in whatever
  order the chip wants, and clears `dirty[]`.
- Deliberately dependency-free (no libc calls), so it compiles unmodified
  under a cross compiler like SDCC or cc65.

## Optional: mixer (SFX overlay and fade)

Build with `-DVGR3_MIXER` to play several `.vgr3` files at once: music on
layer 0 and short sound effects on layers above it. `-DVGR3_FADE` adds a
music fade-out. Neither exists in a plain build, and the decoder core is
the same code either way. Design and measurements: `notes/sfxfade.md`.

```c
vgr3MixerInit(music);            /* layer 0 */
vgr3MixerPlay(1, sfx);           /* overlay on layer 1 */
vgr3MixerStop(1);                /* optional: an overlay stops itself */
vgr3MixerFade(0, 4);             /* -DVGR3_FADE: fade layer 0 over 16*4 frames */
vgr3MixerFrame();                /* once per tick */
/* glue flushes g_mixer.outRegs / g_mixer.outDirty, then clears outDirty */
```

- Every layer keeps its own shadow registers and all layers' channels keep
  running, so the music never pauses or desyncs. Only what reaches the
  hardware is merged: a layer owns a register byte from the first time it
  writes it, and the highest owner wins.
- When an overlay stops, or one of its channels parks on `END`, the bytes it
  held go back to the music at its current values. This includes note
  triggers, so an interrupted note is retriggered rather than left silent.
- Fade needs two per-chip hooks from the glue (`Vgr3FadeOps`: which bytes
  are volumes, and how to attenuate one); `vgr3AttenLow4/SN/GB` cover the
  common chips.
- `-DVGR3_SINGLE_MIXER` drops the mixer pointer argument and uses one
  static `g_mixer`, which is much cheaper on cc65/sdcc. Size the arrays
  per platform with `VGR3_MAX_LAYERS`, `VGR3_MAX_CHANS` and
  `VGR3_MAX_REGS` before including the header.
- Decode cost is per channel playing, so budget for short 1-3 channel
  effects, not a second full song. Flush the previous frame's writes first
  and decode second in the interrupt to keep register writes at a fixed
  offset.
- Not yet covered: registers shared between voices (AY R7, NES `$4015`, GB
  NR50-52, ...) and chips other than SN76489 and NES in the tests. See the
  TODO in `notes/sfxfade.md`.

`nes/mixdemo.c` is a NES demo: music, an SFX over it at frame 20, then a
fade. `make nes-demo` stages it (`BWS=` a checkout of 8bitworkshop builds
it).

## Platform glue

Hardcoded, chip-specific players on top of `vgr3_play.c`:

- `vgr3_coleco.c` -- ColecoVision, single SN76489 on the fixed OUT port;
  reconstructs the latch/data byte protocol from the shadow registers.
- `vgr3_nes.c` -- NES APU, `$4000+r` for shadow byte `r`, ascending register order. Uses the mixer (with fade); the other players use the plain `Vgr3Player`.
- `vgr3_pokey.c` -- Atari POKEY, `$D200+r`; ascending order keeps
  `AUDCTL` ahead of the `STIMER`/`SKRES` command registers.
- `vgr3_gb.c` -- Game Boy DMG, `$FF10+r` for shadow bytes `0x00-0x16`
  (NR10-NR52) and `0x20-0x2F` (wave RAM); ascending, with the unused
  `0x17-0x1F` hole skipped.
- `vgr3_sid.c` -- Commodore 64 SID, `$D400+r`. Flushes a dirty byte at a
  time like the others; each voice's control/gate byte is held back and
  written last within its byte so AD/SR land before the gate.
- `vgr3_msx.c` -- MSX with AY8910.

These need the target toolchain (they `#embed` a `.vgr3` file) and are
not built by this Makefile; they are reference glue for a port.

## Comparison with other VGM players

- vgmcomp2 (SN76489 samples): VGR3 is 26–36% smaller on songs of 11 KB or
  more.  It is 9–30% larger on the tiny ~350 byte sound effects.
  vgmcomp2 is also lossy, while VGR3 is verified
  lossless, so this isn't like for like.

- gzip, xz, zstd (not playable on 8-bit): VGR3 is smaller on every sample,
  by 5–63% against the best of xz and zstd.

- Tracker modules (DefleMask .dmf, NES): VGR3 is 50%-300% larger than the
  .dmf module files, without samples.

## Building and testing

```
make                 # builds vgm2vgr3
make roundtrip       # encodes every sample under samples/; vgm2vgr3
                     # writes nothing unless the decoded file matches the
                     # source frame by frame, so a clean run is a real
                     # correctness signal across the whole sample set
make roundtrip-loop  # same with --loop (exercises the loop-around state)
make mixertest       # mixer and fade host tests (test/): overlay and fade
                     # checked against independent players on SN76489, NES
make bwstest         # test players with 8bws CLI
make nes-demo        # stage/build nes/mixdemo.c
```
