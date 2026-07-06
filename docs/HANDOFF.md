# L10GL handoff brief — state as of 2026-07-06 (evening)

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. This file records the live debugging
state: symptom 1 (diagnosed -- monitor scaling moire, parked) and
symptom 2 (engine writes not landing -- open, in progress).

## Test setup (fixed, do not re-derive)

- Machine `david-ta970`: S3 ViRGE/DX (PCI id 0x8a01), 4MB VRAM,
  x86-64 Linux, tested over SSH by the human (David). He runs the
  binaries and reports logs + photos of the monitor. You cannot see
  the screen; design every change so one run produces decisive output.
- **There is no `/dev/fb0` on this machine** — no kernel fb driver is
  bound to the card. The chip boots into the bootloader's leftover VBE
  mode: 800×600 raster, CR67 Mode 13, 32-bit pixels, pitch 3200.
- Because of that, `virge_init` performs a **native scanout takeover**
  (`virge_scanout_takeover` in `src/backends/virge/virge.c`): switches
  scanout to Mode 9 (15-bit RGB555), doubles all horizontal CRTC
  timings (15/16bpp modes count two character clocks per 8 pixels on
  this chip family), sets CR50 bits 5-4 = 01b, programs pitch in
  quadwords (LSW = pitch/8, CR43 bit 2 clear), adopts 800×600/stride
  1600 into the ctx, and restores everything at cleanup. **This is
  hardware-verified working** as of commit `301d448`: `scantest`'s
  CPU-drawn full-screen pattern displays correctly (border, corner
  markers, color bands, 555 discriminator green).

## Ground rules (from PLAN.md, non-negotiable)

- Register facts need citations from `docs/datasheets/` (DB019-B, by
  PDF page). Where the databook is silent (per-depth timing scaling,
  CR50), the kernel `s3fb` driver and 86Box are the documented
  behavioral references — consult, never copy code (86Box is GPL).
- One logical change per commit; never mix refactor with behavior.
  Every commit builds clean: `make -B BACKEND=virge && make scantest`.
- Never write an unbounded register-poll loop (see the
  `virge_wait_vsync` latched-VSY-INT lesson in virge.c).
- State in each commit message exactly what the human should observe
  on hardware.

## Symptom 1: DIAGNOSED — monitor scaling moiré (not a driver bug; parked)

The dark vertical band ~1/3 across in CPU-drawn `scantest` content is
**monitor-side analog scaling moiré**, confirmed 2026-07-06, not a
register defect:

- It is a **dimming** — the correct content shown darker as thin
  horizontal slivers inside a ~30px-wide vertical stripe, full height.
  A brightness modulation, not corrupted/missing data, so the
  framebuffer writes are correct and the artifact is purely in display.
- "Vertical band of horizontal slivers" is the signature of analog-VGA
  sampling interference.
- The test monitor is a **1440x900 LCD**; the driver outputs 800x600,
  so the monitor scales 800->1440 horizontally (1.8x, non-integer).
  Non-integer scaling beats the source pixel rate against the panel's
  sample rate, worst at one X -> the band. A CRT would not show it.

Driver causes exonerated on hardware: monitor auto-adjust (no change);
CR3B/SFF at two values -- `new_CR00-5` = 254 and `2x BIOS SFF` = 234
(no change, commit `048bc06`); EHB/EHS re-encode verified bit-exact
against DB019-B sec.16 PDF pp.148-153. The takeover's pixel-level
timing is identical to Mode 13 (HT/HDE/sync pixel-preserved under the
char-clock doubling: 132 chars x 8 px = 264 chars x 4 px = 1056 px),
so the monitor sees the same 800x600@60 signal it always did -- the
band was simply hidden in Mode 13's garble. The earlier CR3B read-only
diagnostic is commit `2937667`. The CR3B change to `2x BIOS SFF` is
KEPT: it restores the BIOS-intended 120-px FIFO refill window, which is
more correct than `CR00-5` even though neither affects this band.

**Not driver-fixable** without driving the panel's native 1440x900 -- a
non-standard 16:10 mode needing P6 native modesetting and a PLL the
ViRGE may not cleanly hit. **Parked.** Free mitigation: the monitor's
manual coarse/fine clock & phase controls can reduce it. The scanout
itself is correct; this is cosmetic and does not block engine work.

## Open symptom 2: engine writes don't land (the next big one)

`./triangle` leaves the **previous scantest pattern** on screen with
some noise at the top. Scanout is correct (symptom 1 aside), so this
means the 2D clear (`virge_fill_rect`), Z-clear, and 3D triangle are
not writing the framebuffer where/how they should. The "noise at top"
is probably where the engine output actually went.

What to check, in order:

1. Re-verify the 2D fill on the now-correct scanout: a standalone test
   that fills known rectangles at known coords (e.g. 100×100 at
   (50,50), full-width bar at y=300) and then **CPU-reads back** VRAM
   through `ctx->fb` at stride 1600 and prints pass/fail per corner —
   plus a photo. That separates "engine writes wrong address/stride"
   from "engine doesn't run".
2. The engine register programming after the takeover: `engine_init_3d`
   and every 2D op program `VIRGE_2D/3D_DEST_SRC_STR` with
   `ctx->stride` (1600) and clip/width with `ctx->width/height`
   (800/600) — confirm in the boot log ("Screen: 800x600 ... stride
   1600") and re-read the datasheet units (2D width in pixels at 16bpp,
   stride in bytes, DB019-B PDF p.232/246).
3. PLAN.md's two old mysteries — "row ceiling ~299" and "narrow widths
   fill nothing" — were measured through the old wrong scanout and are
   flagged likely-artifacts, but if fills still misbehave now they are
   real engine bugs: re-run those exact tests on the fixed scanout
   before theorizing.
4. CR50: the takeover sets only bits 5-4 (pixel length, per s3fb). On
   older S3 cores CR50 bits 7-6/1-0 also encode a "GE screen width"
   used by the blit engine. DB019-B does not document CR50 at all;
   86Box's `vid_s3_virge.c` is the reference for whether the ViRGE 2D
   engine consumes it. If it does, the engine may be blitting with a
   wrong internal pitch — which would fit symptom 2.
5. The V8-era color packing and V9/V10 triangle fixes are already in;
   once fills land correctly, `./triangle` should show a clean scalene
   triangle with a smooth red→green→blue ramp. Shape/gradient defects
   at that point are V7/V9 sub-pixel issues (see PLAN.md Phase 0).

## Diagnostic inventory

- `sudo ./scantest` — phase 1: three CPU-drawn layout-hypothesis strips
  (C = 555/1600 must be the clean one); phase 2: full-screen CPU
  pattern at adopted geometry. No engine involvement.
- `sudo ./triangle`, `./cube` — engine paths (2D clear + Z-clear + 3D).
- `sudo ./fbtest` — fbdev-based pattern; useless on this machine (no
  /dev/fb0), kept for machines that have one.
- Boot log prints: FB/fbdev status, "CRTC raw"/"CRTC truth" dump
  (pre-takeover), takeover geometry + register readbacks, engine
  banner. Always ask for the full log with any photo.
- Reference sources: `docs/datasheets/DB019-B_*.pdf` (use `pdftotext -f
  <page> -l <page> -layout`), kernel `drivers/video/fbdev/s3fb.c`
  (fetch from kernel.org), 86Box `src/video/vid_s3_virge.c`.

## Recent commit trail (context for git archaeology)

`b155aaa` CRTC truth dump · `d3cca61` scantest v1 · `266ac9f`
wait_vsync latch fix · `bc0fc30` takeover v1 (CR67-only — put the
monitor out of range) · `50cab91` demos adopt geometry · `301d448`
takeover v2 (hmul=2 + quadword pitch — works) · `3671c38` PLAN.md
update. Read `301d448`'s message for the full timing-scaling story.
