# L10GL handoff brief — state as of 2026-07-07

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. Live state: symptom 1 (diagnosed — monitor
scaling moiré, parked); symptom 2 (3D Z-buffer cutoff — RESOLVED);
double-buffering with vsync page-flip (LANDED 2026-07-07); a newly-
visible back-face bleedthrough quality issue (OPEN).

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

## Symptom 2 (3D Z-buffer cutoff): RESOLVED 2026-07-06/07

The 3D cutoff (cube/triangle cut at ~2/5 height, y≈234 of 600; 2D fills
full height) was **two compounding bugs**, both fixed and verified:

1. **VIRGE_Z_FIXED scale** (commit `ae351e6`, the V10 color-scale twin).
   The Z unit compares the S16.15 *integer part* (TZS>>15) as the full
   0–65535 Z word. The old `z<<15` made every z<1.0 compare as **0**, so
   ZBC_LESS became "pass iff the fetched Z word is nonzero" (the cutoff)
   and ZUP wrote 0 (no depth ordering). Fixed: `VIRGE_Z_FIXED(x) =
   x*65535*32768`, same ×65535 on the deltas (virge.h).

2. **Z_STRIDE clobbered by 2D commands on real DX silicon** (commit
   `f0811f1`, renamed `engine_init_3d` → `program_3d_state`). On real DX
   hardware, executing 2D commands (`virge_clear_z`, `virge_fill_rect`)
   resets `VIRGE_3D_Z_STRIDE` (0xB4E8) to its all-ones default **0xFF8
   (4088)**. The clear wrote Z at stride 1600 (2D) but the 3D fetch then
   used 4088 — they disagreed, and below y≈234 the 3D unit fetched garbage
   Z and failed the test. 86Box never reproduced this (it models the 2D
   and 3D register files as separate — the "fidelity caveat"). Fix:
   re-arm `program_3d_state` (DEST/Z base, strides, clip) before **every**
   3D primitive, in both `virge_draw_triangle_gouraud` and
   `virge_draw_textured_triangle`.

**Verified:** `gltritest` ZBC matrix — LEQUAL/LESS/ALWAYS all FULL at
maxy=549, count=89899; NEVER empty. Z_BASE toggle (0xea600 vs 0x200000)
both FULL. `./cube` renders full height (cutoff gone). This DX-specific
"2D-invalidates-3D-Z_STRIDE" behavior is hardware-established knowledge
no on-hand document contains — recorded in `docs/datasheets/README.md`'s
verified-facts list.

The Z-layout probe in gltritest still prints `[MISMATCH]` (measured
stride ~1597 vs 1600) — a false-positive measurement artifact (stray
0xB332 hits in uninitialized VRAM corrupt the first/last endpoint math),
NOT a regression; the matrix is authoritative.

## Double-buffering + vsync page-flip: LANDED 2026-07-07

Tear-free animation via CRTC page flip (commits `2863663`..`7b4c67b`).
The cube/textured_cube were single-buffered and unsynced (torn); now they
render into a back buffer and `l10gl_swap_buffers` flips it to scanout at
vertical blank. Two hardware facts were settled first by `fliptest`
(`demos/fliptest.c`), both cited in `docs/datasheets/README.md`:

- **Display-start-address unit = DWORD (byte/4).** CR31 bit 3 (ENH MAP,
  which the driver sets) selects doubleword addressing (DB019-B PDF
  p.193). fliptest cycled CR0C/CR0D+CR69 through x1/x2/x4/x8 of the
  back-buffer offset; only **x4** gave a clean full-screen page flip
  (x8 sheared "red-over-green", x2 hit the Z-region garbage, x1 hit
  end-of-VRAM garbage).
- **The per-boot vsync timeout was a missing interrupt enable.**
  `virge_wait_vsync` polled the VSY INT latch (Subsystem Status bit 0),
  but that latch only reports an interrupt that is ENABLED, and the
  driver never set VSY ENB (Subsystem Control bit 8, PDF p.300). fliptest
  confirmed: VSY INT sets in ~60ms with VSY ENB, never without. 0x3DA
  bit-3 (live retrace) also works as an independent source. Fix: OR
  VSY_ENB onto the clear write (`b904aa4`); the "wait_vsync timed out"
  warning is gone.

Backend (`8527a9e`): two color buffers — buffer 0 at offset 0 (the
scanout at init), buffer 1 at stride*height; `ctx->fb_base` is always the
current render target and starts at 0, so single-buffer callers and the
offset-0 readback diagnostics are UNCHANGED. `virge_swap_buffers`:
wait_engine → set_display_start(just-drawn buffer) → wait_vsync (let the
flip land before reusing the old front) → flip fb_base. Z buffer shared
across both color buffers (cleared per frame). Z base moved 0xea600 →
0x1d4c00 (still inside 4MB). Takeover/cleanup now save+restore CR0C/CR0D/
CR69 so the console returns to offset 0 after a swapping run. Frontend
(`6e4f7f8`): `l10gl_swap_buffers(ctx)`, EGL-style, NULL-safe (mga1064
leaves the slot unset). Demos (`7b4c67b`): cube/textured_cube call it
after wait_engine each frame; `triangle.c` is static and stays
single-buffered.

**Verified on hardware (2026-07-07):** cube/textured_cube animate
tear-free. `tritest` rebuilt explicitly (`make -B BACKEND=virge tritest`)
confirms the new layout end-to-end: boot prints `FB base: 0x0 (back buf
0xea600), Z base: 0x1d4c00` with **no vsync timeout** (commit 2 — the
takeover's own wait_vsync no longer prints the 250ms warning), and the
Z-on triangle renders FULL HEIGHT (123339 px, bbox y=[21,579]) — so the
offset-0 render-target invariant holds and Z-buffering is correct at the
double-buffer layout. (gltritest still needs an explicit rebuild to re-run
its matrix, but tritest already covers 3D+Z at the new layout; the earlier
"stale gltritest" paste is moot.) tritest's own verdict line — "FULL
HEIGHT with Z ON … the cube cutoff is NOT Z" — matches the bleedthrough
diagnosis below (a depth-range/Z-fight issue, not a Z-buffer failure).

## Open: back-face color bleedthrough (unmasked by tear-free output)

Now that the cube is tear-free, back-face color bleeds through the front
faces — a depth/edge quality issue, NOT a double-buffer regression (the
shared Z buffer is cleared each frame; the ZBC matrix is FULL). Likely
pre-existing but hidden by tearing. Suspects, in evidence order:

- **Depth range widened — HELPED but not cured.** `demos/cube.c`
  `project()` used to map eye-z to a ~0.002 slice of [0,1] (~130 of 65536
  Z levels separating front/back) → Z-fight at grazing angles. Commit
  `f8fb056` widened it to `sz = (z_eye + camera_dist − 3) / 4.0` (~32k
  levels). Bleedthrough is now **"reduced but not gone"** — a residual
  systematic error remains.
- **Z-gradient scale — TdZdX EXONERATED, TdZdY UNTESTED.** `dztest`
  (2026-07-07) measured the per-pixel X Z-gradient on silicon at
  **measured/intended = 1.000** — TdZdX is exactly right, FALSIFYING the
  86Box-cited "TdZdX is half-strength (added to the post-<<1 accumulator,
  needs 2^16)" claim. (This also makes 86Box's parallel "TdZdY is fine"
  untrustworthy — it must be measured.) The X probe cannot exercise Y: its
  constant-z left edge sets `slope02 = 0` and `dzdy = 0`, so the programmed
  `TdZdY = −dzdy + slope02·dzdx` is 0 — the per-scanline Z edge-walk never
  runs. dztest now also probes Y (z=f(Y), dzdx=0). **ACTIVE NEXT STEP:** run
  it — a Y ratio ≠ 1.0 names the bug (×2 if ~0.5); 1.0 exonerates all
  gradients and pivots to edges/precision.
- **Edge/precision (secondary):** sub-pixel seam rules and LEQUAL ties at
  the cube's shared silhouette edges (the V7/V9/V10 Phase-0 thread). Only
  worth pursuing once both Z-gradient axes are verified 1.0.

Repro: `sudo ./cube`. Acceptance test: two overlapping triangles at
z=0.3/0.7 drawn in both orders occluding identically.

## Established engine facts (verified against 86Box, 2026-07-06)

Register-file architecture, from the MMIO decode in 86Box
`vid_s3_virge.c` (behavioral reference per repo rules — consult,
never copy):

- The three **2D banks (0xA4xx BitBLT / 0xA8xx line / 0xACxx poly)
  are aliases of ONE shared register file**; the **3D banks
  (0xB0xx line / 0xB4xx triangle) have their own separate file**. A
  2D kick does not invalidate 3D dest/stride/clip/Z state; one-time
  `engine_init_3d` programming is architecturally sound.
- **3D kick semantics:** with autoexecute OFF, the triangle launches
  on the **CMD_SET (0xB500) write** — all parameters must be written
  before it. 0xB57C (TY01/TY12 + L/R in bit 31) merely latches, and
  kicks only when AE is ON. L10GL's order (params → TY01_Y12 →
  CMD_SET last) is correct.
- **3D clip is dead state with HC off** (the rasterizer applies
  clip_t/b/l/r only when the triangle's CMD_SET has HC set; L10GL
  disables HC everywhere). The clip registers cannot cause cutoffs.
- **Write masks** (values silently truncated): base addresses
  `& 0x3FFFF8` on 4MB cards; strides `& 0xFF8`; clip fields and
  TY01/TY12 counts 11-bit.
- **Z pipeline:** span start `z = (base_z > 0) ? base_z<<1 : 0`
  (clamped non-negative); compare uses `z >> 16` as unsigned 16-bit
  vs the fetched word; ZBC codes match DB019-B p.129 / virge.h; on
  pass with ZUP the **new Zs is written back** (fail writes nothing);
  Z addresses = `z_base + y*z_str`, top-down, plain linear VRAM.
  Scale quirk: `TdZdX` accumulates in the post-shift domain
  (effectively 16 fractional bits) while TZS/`TdZdY` are pre-shift
  (15) — matters once real Z gradients are programmed.
- 86Box renders the current failing case full-screen under LESS — the
  observed DX behavior is **off the emulator's map**; from here only
  hardware probes are authoritative.

## Diagnostic inventory

- `sudo ./scantest` — phase 1: three CPU-drawn layout-hypothesis strips
  (C = 555/1600 must be the clean one); phase 2: full-screen CPU
  pattern at adopted geometry. No engine involvement.
- `sudo ./triangle`, `./cube` — engine paths (2D clear + Z-clear + 3D).
- `sudo ./filltest` — 2D-fill readback: programs known rects via
  `virge_fill_rect`, CPU-reads VRAM back at corners/center, prints pass/fail
  + a bounding-box locater if a color lands wrong. PASSES as of `385d11d`
  (2D engine exonerated).
- `sudo ./tritest` — raw-path 3D triangle + Z-buffer readback (chip driver
  only, no frontend).
- `sudo ./gltritest` — the symptom-2 workhorse: full demo path
  (l10gl_create → clear → draw) with ZBC compare-code matrix, Z-clear
  coverage probe, and Z-profile-under-LESS/ALWAYS readbacks. Now the Z
  regression test: LESS row must be FULL. **Rebuild caveat: gltritest is
  NOT in `DEMOS`, so `make -B BACKEND=virge` does NOT rebuild it — use
  `make -B BACKEND=virge gltritest` explicitly.** A stale binary still
  reports `Z base: 0xea600` (pre-double-buffer layout) and the old vsync
  timeout.
- `sudo ./fliptest` — double-buffer groundwork probe (virge.c only): CPU-
  draws two patterns and cycles the CRTC start address through x1/x2/x4/x8
  divisors to settle the display-start unit on silicon, plus reports a
  working vsync source (0x3DA retrace vs VSY INT latch w/ VSY ENB).
  Settled x4/dword + VSY ENB on 2026-07-07.
- `sudo ./dztest` — per-pixel Z-gradient probe (virge.c only): draws a
  z=f(X) ramp triangle (dzdy=0) and a z=f(Y) ramp triangle (dzdx=0),
  CPU-reads the Z buffer, and reports the rendered per-pixel Z-word slope
  vs intended for BOTH axes (TdZdX and TdZdY). X verified 1.000 on
  2026-07-07; Y is the active open measurement (the X probe can't reach
  it). NOT in `DEMOS` — build explicitly: `make -B BACKEND=virge dztest`.
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
takeover v2 (hmul=2 + quadword pitch — works; read its message for
the full timing-scaling story) · `048bc06` CR3B for doubled scanout ·
`c7c96f0` symptom 1 parked (monitor moiré) · `93e92ed`/`b0059e2`/
`385d11d` filltest + 2D fill direction fix (2D exonerated) ·
`e7b66bc`/`52ae126` tritest (cutoff correlates with Z on) ·
`7fa057e`..`01c220f` gltritest: ZBC matrix (LESS cuts at y≈234),
Z-clear coverage verified 0xFFFF, ZUP write-back missing from CPU
view — leading to the "Z unit addressing" analysis above.
`ae351e6` VIRGE_Z_FIXED ×65535 (symptom-2 fix #1) · `f0811f1` re-arm
program_3d_state per primitive (symptom-2 fix #2: 2D clobbers Z_STRIDE
to 0xFF8 on DX) — cutoff resolved · `2863663` fliptest (×4 start unit +
VSY ENB) · `b904aa4` wait_vsync: enable VSY interrupt · `8527a9e`
backend double-buffer primitives (2 color buffers, page flip) ·
`6e4f7f8` l10gl_swap_buffers frontend API · `7b4c67b` cube/textured_cube
swap each frame (tear-free). Open after this: back-face bleedthrough
(tiny cube depth range), Ctrl-C console restore (#7).
