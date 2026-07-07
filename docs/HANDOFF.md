# L10GL handoff brief — state as of 2026-07-06 (evening)

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. This file records the live debugging
state: symptom 1 (diagnosed — monitor scaling moiré, parked) and
symptom 2 (narrowed to the 3D Z unit: wrong TZS scale + Z fetch/update
not touching the CPU-visible z_base region — kill sequence pending).

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

## Open symptom 2: now isolated to the 3D Z unit

The investigation narrowed in stages, each hardware-verified:

1. **2D fills are correct.** `filltest` passes cleanly after the
   XP/YP direction fix (`b0059e2`) — rects land at the right
   addresses, full height, correct stride. The old PLAN.md "row
   ceiling ~299" / "narrow width" mysteries did not reproduce on the
   fixed scanout for FB-destination fills.
2. **The 3D cutoff is Z-gated.** A uniform-z (0.5) Gouraud triangle
   over a Z-buffer cleared to 0xFFFF, varying only CMD_SET ZBC
   [22:20]: NEVER→empty, ALWAYS→full, LEQUAL→full, **LESS→cut at
   y≈234** (top ~24% of pixels render; walk is bottom-up so the walk
   *starts* failing and begins passing at y≈234).
3. **Z-clear coverage is complete.** CPU readback at
   `z_base + y*1600` reads 0xFFFF at every sampled row, both after
   `virge_clear_z` and after the subsequent color fill. The Zzb=0
   boundary model is refuted.
4. **The current analysis** (see "established engine facts" below for
   the receipts): with the driver's `VIRGE_Z_FIXED(z) = z<<15`, the
   compare-side integer is `TZS>>15` = **0** for any z<1. Therefore
   LEQUAL (0 ≤ anything) is **vacuous — it always renders full and
   proves nothing**; and LESS (0 < Zzb) renders a pixel iff the word
   the Z unit *actually fetches* is nonzero. The LESS image is a map
   of fetched memory content. Since the CPU-visible z_base region is
   all 0xFFFF, **the Z unit is not fetching (nor, per the ZUP
   write-back check, writing) the CPU-visible z_base region**. The
   y≈234 boundary lives at whatever addresses it really touches.
   Note the programmed Z region (0xEA600–0x1D4C00) crosses the 1MB
   boundary — a plausible wrap point for a Z-port addressing erratum
   that the 2D engine and CPU aperture handle fine.

**Kill sequence (next hardware run, one build):**

1. Fix `VIRGE_Z_FIXED`: the S16.15 integer part is the full 16-bit Z
   value, so `VIRGE_Z_FIXED(x) = x × 65535 × 32768` (z=1.0 →
   0x7FFF8000; compare value = TZS>>15). Same ×65535 on TdZdX/TdZdY.
   This is the Z twin of the V10 color-scale fix (`d101112`); correct
   the virge.h comment. Needed regardless of the addressing bug.
2. ALWAYS + ZUP draw at a *distinctive* depth (z=0.7 → Zs=0xB332; do
   not use 0.5→0x7FFF, which is also RGB555 white and false-positives
   against the framebuffer).
3. CPU-scan all 4MB for runs of that value → the Z unit's real write
   base/stride/orientation, directly comparable to the programmed
   Z_BASE/Z_STRIDE. Nothing found → the Z unit never writes: suspect
   the ZUP bit or a Z-unit enable rather than addressing.
4. Independent discriminator: set Z_BASE = 0x200000 (2MB) and re-run
   the LESS matrix. Boundary moves/vanishes → fetched-content map
   confirmed (addressing story, incl. the 1MB-crossing hypothesis).
   Boundary stays at y≈234 → walk-relative, internal to the span
   engine — different investigation.

Once Z works: the real acceptance test is two overlapping triangles
at z=0.3/0.7 drawn in both orders occluding identically. Then
`./triangle` / `./cube` should finally render correctly; remaining
shape/gradient defects at that point are V7/V9 sub-pixel issues
(PLAN.md Phase 0).

**Retroactive warning:** every earlier conclusion that leaned on
"LEQUAL renders full" as a control is invalid — LEQUAL was vacuously
true at the current Z scale.

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
  coverage probe, and Z-profile-under-LESS/ALWAYS readbacks. The kill
  sequence above lands here.
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
