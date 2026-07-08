# L10GL handoff brief — state as of 2026-07-07

Audience: an implementing agent picking up this project cold. Read
`PLAN.md` (roadmap + current-state snapshot) and
`docs/datasheets/README.md` (datasheet page index + verified register
facts) before changing code. Live state: symptom 1 (diagnosed — monitor
scaling moiré, parked); symptom 2 (3D Z-buffer cutoff — RESOLVED);
double-buffering with vsync page-flip (LANDED 2026-07-07); back-face
"bleedthrough" — RESOLVED on silicon 2026-07-08 (perspective cull +
attribute-base prestep; cubefb 0 blends/misplaced AND monitor-confirmed
by David — no bleed visible). NEW symptom 2026-07-08:
black wedges out of face-corner vertices (coverage gaps — old cubefb was
blind to background-inside-face). Cause: integer-dy edge slopes vs the
fractional-Y prestep. Float-dy fix (195260c) closed the wedges but left
9/36 orientations with a coverage NOTCH at shared face diagonals — root
caused + fixed (lr=0 X-attribute-delta sign error, df35256) and VERIFIED
on silicon 2026-07-08: cubefb 36-sweep = 0 holes (see follow-up #4).
CURRENT OPEN AXIS: textured_cube — texture UV does not interpolate across
a triangle (texprobe shows U,V dead-constant); all register addresses/
format/s/command verified datasheet-correct, so the bug is in the deltas
or the perspective W-divide. See follow-up #5 + run `sudo ./texprobe`.

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

## Push workflow (non-negotiable — David has corrected agents on this repeatedly)

The workspace machine has **no ViRGE card**; David tests on a separate box
(`david-ta970`) by **pulling from GitHub**. Consequence: he cannot run
anything until it is pushed.

For every change:
- Build locally first (`gcc` needs no card): `make -B BACKEND=virge <target>`
  to confirm it compiles before he ever pulls.
- `git commit` with the **expected hardware observation** in the message
  (what David should see on `david-ta970`), then `git push origin main`
  **immediately**.
- **Never hold a commit "until hardware-verified" — that deadlocks him.**
  Push first; he pulls, runs on `david-ta970`, and reports logs/photos back.

Gotcha: `gltritest`/`tritest`/`filltest`/`fliptest`/`dztest` are NOT in
`DEMOS`, so `make -B BACKEND=virge` does NOT rebuild them — build each
target explicitly (a stale `gltritest` binary still reports the old Z base
`0xea600` + the old vsync timeout).

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
diagnosis below (a culling/setup issue, not a Z-buffer failure).

## Back-face "bleedthrough": RESOLVED 2026-07-08 — VRAM-clean (cubefb 0 blends/misplaced) AND monitor-confirmed by David

(History: the "monitor scaling, closed" call of 2026-07-07 was retracted the
same day — scaling blends colors into a fringe, it does not bleed pure face
colors through. The early cubefb only checked blends at 5 orientations;
solid misplaced face color was invisible to it.)

The upgraded `cubefb` (36-orientation sweep; blends + MISPLACED + excess
signals) settled the discriminator: the bleed IS a per-frame VRAM artifact
(10/36 orientations contaminated; 9/36 after the snapped-slope + edge-X
prestep commit `25786e7`). It is NOT animation/swap/monitor.

**Diagnosis** (per-elimination): seamtest proves two front-facing faces
never generate fragments for the same pixel (watertight partition), so Z
never arbitrates between them and no front-front pair can contaminate. The
remaining fragment source inside a front face is a face that should have
been culled: the demos' `normal_view[2] >= 0` test is the ORTHOGRAPHIC
approximation, which at camera_dist=5 mis-classifies faces within ~11° of
edge-on. A barely-back-facing sliver projects INSIDE the silhouette,
overlapping its front neighbor with near-tied Z along their shared edge —
exactly the "misplaced solid face color" signal, only near edge-on. Its
visibility was amplified by a second, backend-side gap: the attribute bases
(TZS etc.) were still sampled at the raw bottom vertex after `25786e7`
moved (TXS, TYS) to the prestepped scanline position, biasing each
triangle's Z by up to one edge-walk step — many 16-bit LSBs on a steep
near-edge-on face — which inflates near-tie noise.

**Fix (perspective cull `3d4e49c` + attribute-base prestep `25786e7` +
`cubefb` z_cmd_bits pin to LESS) — VERIFIED on silicon 2026-07-08.** cubefb
36-sweep reports 0 blends / 0 misplaced / 0 holes, AND David confirms on
the live monitor (after the df35256 run) that no bleedthrough is visible
— the display path is clean, closing the gap a static VRAM readback
alone could not. The earlier "monitor scaling, closed" call was wrong
(scaling fringes; it does not bleed pure face color through); the real
fix was the cull + base prestep, and the monitor view is now the
exonerating evidence.
- Perspective-correct cull in cube/cubefb/cubediag:
  `dot(normal_view, center_view) >= 0` skips the face (exact for planar
  faces; unit-cube center == normal, so `center_view = normal_view +
  (0,0,CAMERA_DIST)`).
- Backend: attribute bases (Z, colors, and U/V/W in the textured path) are
  now prestepped along edge 02 to (TXS, TYS) — the missing half of the
  README §prestep. Textured path also gained the snapped-slope edge prestep
  and the |det| < 1 degenerate guard for parity with the Gouraud path.
- `cubefb` now pins `z_cmd_bits` to LESS (the glue state cube.c runs
  under); it previously ran on the virge_init LEQUAL default, a different
  near-tie tie-break than the demo it instruments.

Silicon facts that stand (and constrain any future theory):
- **Z pipeline correct:** ZBC matrix FULL, exact Z-writeback, TdZdX and
  TdZdY both measured/intended = 1.000 (see Symptom 2 / dztest).
- **Coverage watertight:** `seamtest` shows NO shared-edge double-draw (the
  two-triangle overlap test's boundary column did not flip with draw order)
  and the start/end fill rules combine to a clean partition in every
  configuration (left triangle fills to N−1, right from N).
- **Contamination is orientation-gated:** only near-edge-on configurations
  (Top near-parallel + Left near-perpendicular) — the cull-wedge signature.

Investigation trail:
- **Back-face cull (`3d4e49c`, test CORRECTED 2026-07-08):** the original
  `normal_view[2] >= 0` orthographic test was itself the residual-bleed
  culprit; replaced with the perspective dot-product test in all three
  cube demos.
- **LEQUAL + back-to-front sort (`d97577a`, then REVERTED `3d3a579`):** added
  on a wrong "shared-edge Z-tie" model. seamtest proved coverage watertight,
  so draw order / depth-func are irrelevant to edges; reverted to LESS +
  fixed face order + cull.
- **Depth range (`f8fb056`, KEPT):** `sz = (z_eye + camera_dist − 3)/4.0`
  (~56k Z levels) — still correct, reduced grazing-angle noise.
- **Z-gradient scale — exonerated** by `dztest` (TdZdX/TdZdY = 1.000).

Diagnostics retained: `cubediag` (interactive cube + color legend, same
LESS path as cube), `seamtest` (fill-rule / watertight proof), `cubefb`
(36-sweep contamination detector — the verdict tool for this fix).

**2026-07-08 follow-up — sweep CLEAN on silicon, cull fix confirmed; new
symptom: black vertex wedges.** With the cull + base-prestep fix the
36-sweep reports 0 blends / 0 misplaced, but David sees black triangles
growing out of some face-corner vertices. cubefb was BLIND to those: a
background pixel inside a face trips no signal (MISPLACED skips any pixel
with a bg neighbor). Cause found in setup: `compute_dxdy_snap` divided by
the INTEGER scanline count while the prestep multiplies by fractional Y —
the slope is distorted by dy_true/dy_int, which for a short shallow edge
at a corner (1–2 scanlines, large ΔX; dy_true either side of 1) mis-places
the span end by tens of px on the vertex rows: a background notch (or
spill) at the vertex. Fix: `edge_slope` divides by the TRUE float dy
(clamped to the S11.20 range — only reachable when the accumulator is
never stepped before reload), `edge_x_at` computes the prestepped edge X
by bounded interpolation between the endpoints (exact for any slope, no
overflow, bit-identical for both triangles sharing the edge), and the
attribute bases are evaluated at (TXS, TYS) via the plane equation
instead of the yf·edge-walk shortcut (bounded even where the slope
clamped). cubefb gained a verdict-driving HOLES signal: background >1.5px
inside the convex hull of the 8 projected vertices (exact silhouette for
a convex solid). UNVERIFIED on silicon — re-run the sweep AND eyeball the
spinning cube (expected: 0 holes, no wedges).

**2026-07-08 follow-up #2 — notch ROOT-CAUSE-LOCALIZED to Z=LESS, not
coverage (diagap on david-ta970).** The 9/36 coverage holes survive the
float-dy fix, so `diagap` reproduced the cube's exact Left-face shared
diagonal at 310deg in isolation and split it. Decisive result:
  - A-alone (LESS): a_left drifts INLAND of trueX (correct rows 351-364,
    then freezes at ~0.9 px/row, +40px inland by row 377).
  - B-alone (LESS): b_right = floor(trueX) every row. Correct.
  - both-LESS A-first == both-LESS B-first == 40px gap (NOT draw-order).
  - **both-ALWAYS A-first: 0px gap; a_left = ceil(trueX) every row.**
Same tri A, same geometry, only Z mode differs => **Z=LESS is rejecting
tri A's OWN pixels on its diagonal side.** Nothing competes with A in
the alone pass (fresh clear_z 1.0), so A's interpolated Z must be >= 1.0
on those columns. Tri A is lr=0 (edge-02/long on the right, the diagonal
edge-12 on the left); virge.c seeds TZS at (x_start,y_bot) on edge-02
(`virge.c:1107-1130`) and walks it up/across via
`ew_z = -dzdy + slope02*dzdx`. That Z walk diverges past 1.0 as it
reaches the diagonal, WORSE toward the edge-12 seed vertex v1=P7 (row
377) — exactly the observed growth direction. Consistent with all three
edge-stepping refutations (Z=ALWAYS is watertight => the X edges are
correct). The bug is in **Z setup** (seed and/or gradient), asymmetric
across the two coplanar tris because their vertex orderings differ (A's
diagonal = edge-12/top-half; B's = edge-01/bottom-half, fine).
**Confirm probe (commit c738f52, awaiting run):** diagap's Z-readback
pass dumps A's written Z across the scanline — prediction is gap columns
read saturated ~1.0 while red columns show a normal gradient, which
distinguishes seed-offset (uniform) vs gradient-sign/magnitude (tilts
past 1.0 mid-span). Run `sudo ./diagap` (310deg) and a clean-angle
control. Do NOT commit any TXEND12 edge perturbation — X edges are correct.

**2026-07-08 follow-up #3 — notch SOLVED; fix landed (df35256), awaiting
silicon verification.** diagap's full-span Z readback (commit 6e3afe3)
on david-ta970 pinned it: the X-direction attribute deltas were
programmed with the raw plane gradient for BOTH L/R directions, but the
engine seeds each attribute at TXS (edge 02) and iterates toward TXEND
adding +TdAdX per pixel — which runs in -X for lr=0 (R-to-L). So for
lr=0 the per-pixel step must be -dA/dx, not +dA/dx. Tri A {0,4,7} is
lr=0 with a wide span (its diagonal is edge-12); its Z gradient tilted
the WRONG WAY across the span (same magnitude ~0.0068/px, opposite
sign), climbing to 1.000 (saturated) at the diagonal where the true Z
(coplanar tri B, lr=1, correct) is ~0.086. Z=LESS rejected those
pixels => the notch. The seed (edge-02 Z ~0.70) and Y-walk were correct
— only the across-span X-slope sign was wrong. Flat-shaded cube faces
hid it in color (color X-deltas == 0); Z=ALWAYS hid it in Z; only a
wide lr=0 span with a real Z gradient under Z=LESS exposed it.
**Fix:** `sx = (lr_direction == 0) ? -1.0f : 1.0f;` applied to all
X-direction deltas (color R/G/B/A, Z, texture U/V/W) in both the
Gouraud and textured triangle paths; the Y-direction deltas (`ew_*`,
edge-walk along side 02) are lr-independent and stay raw. **Verify on
next run:** `sudo ./diagap` (310deg) → both-LESS passes 40px→watertight
and A's diagonal Z drops 1.000→~0.1 (matching B's ~0.086, no
saturation); `sudo ./cubefb` 36-sweep → 0 holes. Tri B (lr=1) is
untouched and stays correct.

**2026-07-08 follow-up #4 — VERIFIED on silicon (df35256 closes the
coverage-hole axis).** David ran `sudo ./diagap && sudo ./cubefb` on
david-ta970: diagap 310deg is watertight across ALL passes (A-alone LESS,
B-alone LESS, both-LESS A-first, both-LESS B-first, both-ALWAYS = 0px;
was 40px). The Z-readback shows A's diagonal Z now **0.127** at y=377
x442 (was saturated 1.000), climbing correctly to 0.661 at the seed
edge-02 — slope magnitude ~0.0068/px in the correct sign, matching B's
true diagonal Z ~0.086 (no saturation). cubefb 36-sweep: **0 of 36
orientations contaminated** (0 blends / 0 misplaced / 0 holes; was 9/36,
310deg worst at 1196 holes). The 9/36 coverage-hole axis is **CLOSED**.
Tri B (lr=1) untouched, stays correct. Any remaining visible artifact is
NOT in VRAM (cubefb reads VRAM directly and is clean). The back-face
"bleedthrough" axis is ALSO resolved — David confirms no bleed is
visible on the live monitor (see the section below).

**2026-07-08 follow-up #5 — OPEN: textured_cube mapping broken — UV does
not interpolate across a triangle.** With the flat cube + bleedthrough
both verified clean on silicon, David reports textured_cube "still isn't
right" — and it was the one demo NEVER silicon-verified (line 180 only
ever confirmed it animates/swaps). This is the current active axis.

SYMPTOM — `texprobe` v1 RAN on david-ta970 (commit 7fda1ca): rendered a
face-on quad (constant w=1.0, UV 0,0->1,1) with a UV-ENCODING 64x64
ARGB1555 texture (texel(x,y) color = R=x>>1, G=y>>1) and CPU-read the
framebuffer (the sampled color per pixel IS the (u,v) the engine
computed). Result: coverage 100%, but **U and V are DEAD-CONSTANT across
the quad interior** — R pinned at 13 (U~=0.41, texel 26) at every
interior x; G oscillates 1..2 (V~=0.04, texel ~3) at every y. They do
NOT interpolate 0..31. Only a start value survives; the texture
gradients are not taking effect. (So each cube face reads as ~one flat
texel color — almost certainly what David sees.)

RULED OUT (all verified against datasheet DB019-B §19.4):
  - NOT the missing `*(tex_width-1)` in `virge.c:tex_coord_fixed()` —
    that would pin R at 0 (texel 0), not 13.
  - NOT wrong register addresses — TUS=0xB538, TdUdX=0xB520, TdVdX=0xB51C,
    TdUdY=0xB52C, TdVdY=0xB528, TdWdX=0xB50C, TdWdY=0xB510, TWS=0xB514,
    TVS=0xB534, TEX_BASE=0xB4EC all match the datasheet (the 0xB524 "gap"
    is TdDdY, the mipmap D-delta).
  - NOT the `s` field — s=log2(64)=6 (datasheet: "value of 4 => 2^4=16x16").
  - NOT the UV format — `S(4+s).(27-s)` = S10.21 with perspective, exactly
    what `tex_coord_fixed` (val*2^(27-s)) produces.
  - NOT the command — 0101 = lit-texture-with-perspective.
  - NOT texture placement — TEX_BASE = z_base+z_size, valid VRAM after
    both color buffers + Z.
So it is a subtler bug than addressing/format/scale.

REMAINING SUSPECTS: (a) the per-pixel DELTAS TdUdX/TdVdX (+Y) are
computed non-zero (dudx~=0.0025 for the 400px-wide quad) but the engine
is not applying them; OR (b) the START register / perspective W-DIVIDE is
broken. Leading hypothesis = the **perspective W-divide**: U/V are S10.21
but W is **S12.19** (different fixed-point scales) and W is documented as
the "homogeneous coordinate / depth coordinate for 3D texture maps";
`virge.c`'s comment claims the engine "interpolates U/W, V/W, 1/W then
divides" but the driver feeds plain U (not U*W) and W=1/z_eye. The
datasheet W register (§19.4) adds "format S12.19, where S must be 0".
Secondary (revisit AFTER UV is fixed): `textured_cube.c` has NO back-face
cull (the perspective cull `3d4e49c` went into cube/cubefb/cubediag only),
so it draws all 12 tris on Z=LESS alone.

PENDING DIAGNOSTIC — `texprobe` v2 (commit f6c635c) NOT YET RUN. Adds:
(1) a dump of the ACTUAL programmed registers read back via
`virge_read32` (TUS/TVS/TWS/TdUdX/TdVdX/TdWdX/TdUdY/TdVdY/TdWdY/TEX_BASE/
CMD_SET — ground truth; write-only regs may read 0 or 0xffffffff); (2)
TEST 2 = two CONSTANT-UV quads (du=dv=0): uv=(0.5,0.5) then (0.9,0.1).
  RUN: `git pull && make -B BACKEND=virge texprobe && sudo ./texprobe`.
  INTERPRET TEST 2:
    - uv=(0.5,0.5) -> R16/G16 AND (0.9,0.1) -> R28/G3: the START register
      WORKS => bug is in the DELTAS (TdUdX/TdVdX not applied per pixel).
    - still reads ~R13/G2 regardless of programmed UV: the START register
      or the perspective W-divide is broken (engine not using the U/V we
      write). Cross-check the reg dump for what TUS/TVS/TWS actually hold.
`texprobe` drives the REAL frontend bind/upload/draw path (= the exact
textured_cube code) and reaches `struct virge_ctx` via `ctx.backend_data`
(`hw` is the first field of `virge_private`). Build:
`make -B BACKEND=virge texprobe`.

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
  vs intended for BOTH axes (TdZdX and TdZdY). BOTH verified 1.000 on
  2026-07-07 — the Z-gradient pipeline is silicon-correct. NOT in `DEMOS`
  — build explicitly: `make -B BACKEND=virge dztest`.
- `sudo ./cubediag [angle_deg]` — rotating cube + per-face color legend
  (front-end path, same as cube.c). Full-saturation flat face colors + a
  static color-key on the right, to identify which face is which while
  inspecting the (monitor-side) bleed. An angle arg freezes one orientation
  and prints which faces are visible; otherwise slow-rotates. Build:
  `make -B BACKEND=virge cubediag`.
- `sudo ./seamtest` — measures the S3d triangle span fill rule on silicon
  (start/end edge, both L/R directions, integer vs half-integer X) and runs
  a two-triangle shared-edge overlap check. Result: coverage is WATERTIGHT
  (no double-draw) -- the end edge is exclusive for L/R=1, inclusive for
  L/R=0, combining to a clean partition at every shared edge. Refutes the
  "double-draw" bleed hypothesis. Build: `make -B BACKEND=virge seamtest`.
- `sudo ./cubefb [N]` — sweeps N (default 36) orientations, CPU-reads the
  framebuffer back (bypassing the monitor), and reports contamination
  signals per angle: BLENDS, MISPLACED (solid face color inside another
  face — the "blue on teal" detector), HOLES (background >1.5px inside
  the convex hull of the projected vertices — the "black vertex wedge" /
  coverage-gap detector), and EXCESS (info only). History: 10/36
  contaminated before `25786e7`, 9/36 after, 0/36 after the cull +
  base-prestep fix (but holes were not yet a signal); now the verdict
  tool for the float-dy slope fix. Build: `make -B BACKEND=virge cubefb`.
- `sudo ./diagap [angle-index]` (default 31 = 310°, cubefb's worst) —
  reproduces the cube's exact Left-face shared diagonal (vert0-vert7;
  tri A {0,4,7}=red with the diagonal as edge-12, tri B {0,7,3}=green
  with it as edge-01) in isolation, same rotation+projection as cubefb.
  Five passes (A-alone/B-alone/both-LESS×2 orders/both-ALWAYS) print the
  per-row gap between the green and red runs, then a 6th Z-readback pass
  draws A alone under Z=ALWAYS+Z-update and dumps A's written Z across
  the scanline. This pinned the notch to Z, not coverage (see below).
  Build: `make -B BACKEND=virge diagap`.
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
