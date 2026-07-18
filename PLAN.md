# L10GL Implementation Plan

This is the roadmap for taking L10GL from "pixels are being drawn, the ViRGE
2D and 3D engines are live" to a usable, simple Linux OpenGL driver. It is
written for implementing agents: each task is scoped to be independently
implementable, committable, and verifiable, with file references and
acceptance criteria.

## Goal and guiding principles

The end state is a **userspace OpenGL 1.1-subset driver** that talks directly
to vintage fixed-function hardware over PCI MMIO — exactly the way the current
demos work. Explicit non-goals: no DRM, no DRI, no Mesa integration, no kernel
module, no X11/GLX. An application links against L10GL (optionally through a
thin `gl.h`-compatible shim) and renders full-screen on the console. The
library is responsible for the video mode: first by adopting/requesting it
through fbdev, ultimately by programming the CRTC directly (Phase 3).

Priorities, in order:

1. **S3 ViRGE is the primary card.** It is the one being tested on real
   hardware and the only backend with proven-live 2D and 3D engines. Every
   frontend feature must work on the ViRGE first.
2. **The architecture must stay multi-card.** The `l10gl_backend` vtable
   (`src/l10gl.h:118`) is the contract. Nothing card-specific may leak above
   it. The Matrox MGA-1064 backend must keep compiling and be kept
   structurally in sync, even while it remains untested on hardware.
3. **Hardware truth over theory.** The git history shows that most bring-up
   time went into undocumented enable bits (CR40/CR53/CR66/AFC, DE bit,
   clip-field ordering). Preserve the diagnostic-printf style in init paths,
   keep changes small, and never bundle a behavioral change with a refactor —
   the human tests on real hardware and needs to bisect.

**Primary sources are in the repo**: `docs/datasheets/` contains the full
S3 ViRGE databook (DB019-B) and the Matrox MGA-1064SG specification, with
a section→PDF-page index and a list of already-verified register facts in
`docs/datasheets/README.md`. Read the relevant section before writing
register code; cite it in comments. Task descriptions below cite PDF page
numbers of DB019-B where a claim was verified.

## Current state snapshot (2026-07-06, evening)

**The persistent "garbled screen" root cause was found and fixed.** The
primary test machine has **no fbdev driver at all** (`/dev/fb0` absent
— nothing owns the card), and the chip was scanning out the
bootloader's leftover VBE mode: 800×600 raster, CR67 Mode 13, 32-bit
pixels at pitch 3200 (proven visually by `demos/scantest.c` phase 1).
Every engine draw at 640×480/16bpp/stride-1280 was therefore noise +
repetition *by construction*, independent of engine correctness — and
every prior hardware observation was made through that mismatched
scanout. Fixes, all committed:

1. **Native scanout takeover** (`virge_scanout_takeover`, `virge.c`):
   when `/dev/fb0` is absent at 16bpp, virge_init adopts the live CRTC
   raster and owns the scanout: CR67 bits 7-4 → Mode 9 (15-bit 555,
   matching the 3D engine's ZRGB1555-only output), **all horizontal
   CRTC timings doubled** (15/16bpp modes count two character clocks
   per 8 pixels on this family — switching CR67 alone doubles hsync
   and the monitor drops sync, as a hardware round proved), CR50
   bits 5-4 = 16bpp pixel length, pitch in **quadwords** (LSW =
   pitch/8, CR43 bit 2 clear). Per-depth scaling and CR50 are not in
   DB019-B — the kernel `s3fb` driver is the documented reference.
   BIOS raster timings/PLL otherwise kept; originals saved (incl. the
   CR11 CR0-7 write-protect) and restored at cleanup; idempotent if a
   dead run left Mode 9 programmed. This is the P6 slice pulled
   forward; full modesetting remains P6.
2. **`virge_wait_vsync` was broken-by-design**: SUBSYS_STATUS bit 0
   (VSY INT) is a *latched* interrupt status (DB019-B §22, PDF
   pp.299-301), not a live retrace level — it must be cleared via
   Subsystem Control VSY CLR before waiting. The old code hung forever
   on every call after the first. Now clear-then-wait, bounded 250ms.
3. **Geometry adoption**: the adopted width/height/stride propagate
   through the glue into `l10gl_ctx`; all demos read the actual
   geometry back after `l10gl_create`.

Two diagnostic tools now exist and stay: `demos/fbtest.c` (CPU test
pattern via fbdev — for machines that *have* an fb driver) and
`demos/scantest.c` (scanout-layout probe over BAR0 + takeover
experiment; its phase 1 now shows strip C clean since virge_init
already owns the scanout).

**Likely-resolved, needs one confirming re-test**: the two open
mysteries below (row ceiling ~299, narrow-width fills) are almost
certainly artifacts of watching a 1600-byte-stride engine draw through
a 3200-byte-pitch scanout (600 drawn rows appear as ~300 scanned rows;
a 96px fill appears as a 48px sliver at 32bpp). The 24bpp "asked red,
saw green" observation is likewise suspect. Re-run the fill tests once
on the fixed scanout before closing them in this document.

Older history (kept for context): an earlier snapshot claimed 2D rect
fill and 3D triangles were hardware-verified; direct register probing
on 2026-07-06 found that too optimistic. Three real bugs were found
and fixed on real hardware earlier that day (all committed):

1. **32bpp silently treated as 8bpp.** `virge_init`'s dest_format
   selection (`virge.c` near line 1101) only had cases for bpp 2/3; any
   other value (including every 32bpp test run against this driver)
   fell into the 8bpp case, corrupting every width/stride/format
   register 4x against what the caller and every other register assumed.
   Now rejected outright with an error (bpp must be 1/2/3 — this chip
   generation has no 32bpp destination format at all).
2. **AFC bit 4 (Enable Linear Addressing) was never set** — only bit 0
   was. Per DB019-B PDF p.22-4/Appendix B.8, bit 4 governs whether the
   CPU-side BAR0 aperture is linear or legacy-windowed, separate from
   CR31's ENH_MAP (which only affects the *engine's* own addressing).
   Now set unconditionally in `virge_init`.
3. **`CLIP_L_R`/`CLIP_T_B` don't reliably hold freshly written values**
   (proven the same way `DEST_BASE` was: write a pattern, read it back
   immediately, get something else). With hardware clipping (CMD_SET
   bit 1, HC) enabled, every fill degenerated to painting exactly pixel
   (0,0). Disabling HC (now the case at all 5 CMD_SET call sites) makes
   fills cover the full rectangle instead. Root cause still open; the
   frontend is relied on to only submit on-screen coordinates until this
   is understood and clipping can be restored.

With HC disabled, a real full-rectangle 2D fill was confirmed on hardware
(exact byte-level pixel matches across many sampled rows/columns) —
this is the first genuinely verified engine-write-to-visible-VRAM result
of the project. However, two **new, still-open** issues were found in
the process, and neither is explained yet:

- **A fixed row-count ceiling around row 298-299**, independent of the
  requested height (tested at 296 → fills completely; 300 → fills 299 of
  300; 600 → fills the same ~299 rows and stops at the identical
  absolute row). Confirmed not a race/timing artifact (identical result
  after an extra 500ms post-idle sleep) and not a CRTC mode mismatch
  (direct CR07/CR12 read confirms real VDE=599, i.e. a genuine 600-line
  active display — so the ceiling isn't "the console is actually
  shorter than requested").
- **Narrow widths (96px and 100px) fill almost nothing** (pixel (0,0)
  only, even with an 8-byte-aligned stride — 96×3=288 rules out the
  stride-alignment theory), while width 800 fills correctly up to the
  row ceiling above. The mechanism connecting width to how much of the
  rectangle actually draws is not yet identified. Needs more hardware
  probing at a range of widths (e.g. 200, 400, 600) before theorizing
  further — do not assume this is the same bug as the row ceiling.

Separately, and not yet re-tested against the above fixes: the color
channel order looked shifted in one test (asked for red 0x00FF0000,
observed a byte pattern matching pure green) — possibly related to
V8's 16bpp 1555-vs-565 mismatch, but this was 24bpp, so if it
reproduces it's a distinct issue worth its own investigation.

The cube demo does now render dynamic (frame-to-frame changing) content
using the fixes above, rather than a static or fully blank screen, but
is still visually wrong ("flickering top third of the screen" was the
most recent hardware report) — consistent with, but not yet proven to
be explained by, the row-ceiling issue above. Two older cube symptoms
and their diagnoses, neither resolved yet:

- **Cube renders all black** → color fixed-point scale bug, task V10
  (since applied — commit `d101112`).
- **Scene repeated ~5 times across the screen** → originally diagnosed as
  a pure stride/pitch mismatch (P1). Given the row-ceiling and
  narrow-width findings above, treat this diagnosis as unconfirmed until
  re-tested against the three fixes already applied — the repeat/tear
  pattern may turn out to be the same underlying issue as the new
  row-ceiling/width finding rather than a separate stride bug.

Written but unverified or known-broken:
- Textured triangle path (`virge.c:813`) and texture upload/bind glue
  (`src/backends/virge/l10gl_virge.c:297`)
- 2D line drawing (`virge.c:640`) — contains a definite bug, see V1
- MGA-1064 backend (`src/backends/mga1064/`) — compiles, untested

Structural gaps:
- Depth/blend state is cached in the glue layer but **never reaches the
  hardware**: every triangle hardcodes `VIRGE_ZBC_LEQUAL | VIRGE_ZUP_ENABLE`
  and no blend bits (`virge.c:622`, `virge.c:976`)
- No mode setting: `virge_init` trusts the caller's width/height/bpp and
  assumes the console is already in that mode; it never checks the actual
  fbdev mode, assumes stride = `width * bpp` (fbdev pitch can differ), and
  never saves/restores console state on exit
- Single-buffered: draws land on the visible framebuffer (tearing)
- The whole 3D transform pipeline (rotation, projection, lighting) lives in
  the demos (`demos/cube.c:32`), not in the library
- Backend is chosen at compile time (`Makefile:20`), not probed at runtime
- No software reference backend, so nothing is testable without vintage
  hardware

---

## Target OpenGL feature profile per backend

Derived from the datasheets in `docs/datasheets/` (ViRGE: DB019-B §15.4,
§19; Mystique: MGA-1064SG spec §1.5, DWGCTL pp. 85-86). This is the
contract for Phase 2 (what the pipeline must feed), Phase 4 (what the GL
shim may promise), and Phase 5 (Mystique caps). Both cards are
fixed-function GL 1.1-subset targets; neither has stencil, accumulation,
multitexture, or general blend factors — those are permanently out of
scope for hardware paths.

**Hardware baseline (both cards, what the GL shim can always promise):**
flat/smooth-shaded Z-buffered triangles and lines; 16-bit depth buffer
with `glDepthFunc`/`glDepthMask`; color+depth clear; scissor
(hardware clip rectangle); perspective-correct texturing with
decal and modulate `glTexEnv` modes; texture repeat wrapping; hardware
dithering; double-buffered page flip. All transform, lighting, and
clipping is frontend software on every card.

| GL 1.1 feature | ViRGE (86C325) | Mystique (MGA-1064SG) |
|---|---|---|
| Depth functions | all 8 | 7 — no `GL_NEVER` (emulate: skip draw) |
| Texture filters | nearest, bilinear, all 4 mipmap modes incl. trilinear | **nearest only** |
| Mipmapping | hardware (D interpolation) | none |
| Blending | `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` only (source or texture alpha) | **none** — texel color-key transparency only (≈ limited `GL_ALPHA_TEST`) |
| Fog | hardware (alpha-driven; excludes source-alpha blending) | none |
| Tex env modes | decal, modulate, add ("complex reflection") | decal, modulate (mono + true-color lighting) |
| Texture formats | ARGB8888/4444/1555, PAL8, Blend4 (compressed), YUV | RGB565/555/332, CLUT4/CLUT8 (on-chip LUT→565) |
| Wrap modes | repeat, border color (`TEX_BDR_CLR`) | repeat and clamp |
| 3D render targets | 8bpp, 16bpp (ZRGB1555 only — see V8), 24bpp RGB888 | 16bpp only |

Consequences:
- `glEnable(GL_BLEND)` on the Mystique cannot be honored; the shim reports
  caps honestly and draws opaque (GL apps of the era handled this — the
  Mystique famously shipped without blending). Its color-key transparency
  can back a limited `GL_ALPHA_TEST` for 1555-style textures (M2 decides).
- `l10gl.h` needs a `L10GL_CAP_FOG` bit (fog is currently not exposed at
  all) and the Phase 2 pipeline should compute per-vertex fog alpha for
  backends that claim it.
- After M1/M2, mga1064 caps become `GOURAUD|ZBUFFER|LINES|TEXTURE|
  PERSPECTIVE|DITHER` — never `BLEND`, `BILINEAR`, `TRILINEAR`, or `FOG`.
  The ViRGE claims all of those plus `FOG`.
- Maximum texture sizes are not yet verified from either document —
  confirm before hardcoding limits (`l10gl_virge.c:368` currently caps
  s at 9 = 512×512 unverified).
- swrast (F3) implements the union of both profiles and is the reference
  for every feature here.

---

## Phase 0 — ViRGE correctness fixes

Small, independent bug fixes. Do these first; each is one commit. These are
in the primary card's already-live paths, so regressions here are expensive.

### V1. Fix the 2D line CMD_SET mask bug
`src/backends/virge/virge.c:731`:

```c
uint32_t cmd = ((0x06 << 27) & 0x1F)  /* 2D line = bits [30:27] = 0110 */
```

`(0x06 << 27) & 0x1F` evaluates to **0** — the command-type field is erased
and the register write at `0xA900` programs a BitBLT (command 0), not a
line. Worse, the unmasked value was *also* wrong: per the datasheet's 2D
line example (§15.4.4.3, DB019-B PDF p. 124, which writes
`0001 100S ... 0010 00S1` to MMA900) the 2D line command code is
**0011b (3)**, not 0110b — so the fix is `(0x03 << 27)`. The same example
confirms DE (bit 5) must be set — the "engine runs but writes no pixels"
trap already fixed for rect fill in commit `98a6169` — and that the mono
pattern bit (8) is *not* set for lines. The comment in `virge.c` claiming
"bits [28:27] = 11" in `virge.h`'s 2D command notes is the correct reading;
the code just never implemented it. While here, verify END0/END1 sign rules
(top 5 bits must be 0, §15.4.4.3 PDF p. 123) and the X-major half-pixel
X START adjustment against the worked example.

*Acceptance:* `virge_draw_line` programs command type 0011b with DE set;
compiles cleanly; human verifies a line-drawing test on hardware.
(Independently confirmed: 86Box's command decode uses line = `3 << 27`.)

### V2. Fix Z-clear width/format mismatch
`src/backends/virge/virge.c:386` (`virge_clear_z`): the comment says "use
8bpp dest format since we're writing raw bytes" and the width math agrees
(width in bytes: `ctx->width * 2`), but the command word uses
`ctx->dest_format` (16bpp on a normal run). At 16bpp the width field is in
pixels, so the fill covers `width * 2` 16-bit pixels per row against a
`width * 2`-**byte** stride — each scanline overruns into the next row.
Either (a) use `VIRGE_DEST_8BPP` and keep byte-based width, or (b) keep
16bpp and pass width in pixels (`ctx->width`) with clip right =
`ctx->width - 1`. Option (b) is preferred: the fill color is already a
16-bit Z value packed twice, and 16bpp fills are what's proven to work.
(The 2D engine's 16bpp mode is format-agnostic — "RGB1555 or RGB565",
DB019-B PDF p. 232 — so raw 16-bit Z words pass through unmodified.)

*Acceptance:* Z-clear width, clip, stride, and dest format are dimensionally
consistent (all-pixels or all-bytes). Human verifies depth testing still
behaves (cube faces occlude correctly) on hardware.

### V3. Plumb depth state into the triangle command word
The glue caches `depth_func_val`, `depth_test_enabled`,
`depth_writes_enabled` (`src/backends/virge/l10gl_virge.c:191`) but both
draw paths hardcode `VIRGE_ZB_NORMAL | VIRGE_ZBC_LEQUAL | VIRGE_ZUP_ENABLE`.

Add a helper in `l10gl_virge.c` (or pass a `z_cmd_bits` field through
`struct virge_ctx` like `dest_format`) that computes:
- depth test disabled → `VIRGE_ZB_NONE` (skip Z compare and Z fetch entirely)
- enabled → `VIRGE_ZB_NORMAL` + map `enum l10gl_depth_func` to `VIRGE_ZBC_*`.
  Verified: the compare codes at `virge.h:337` match §15.4.6 (DB019-B PDF
  p. 129) exactly, and the operand order is `Zs <op> Zzb` — same as GL, no
  inversion needed. Note the datasheet defines Z-buffering as active iff
  ZB MODE (bits 25-24) = 00b *and* compare code ≠ 000b; it doesn't describe
  ZB MODE = 11b, so verify `VIRGE_ZB_NONE` behaves as "no Z" on hardware
  (fallback: ZB_NORMAL + ZBC_ALWAYS + ZUP per depth mask)
- `depth_writes_enabled` → `VIRGE_ZUP_ENABLE`/`VIRGE_ZUP_DISABLE` (bit 23,
  PDF p. 129)

Apply in both `virge_draw_triangle_gouraud` and
`virge_draw_textured_triangle` (change their signatures to accept the bits,
or store them in `struct virge_ctx`).

*Acceptance:* `l10gl_depth_func(ctx, L10GL_ALWAYS)` visibly disables
occlusion in the cube demo; `l10gl_enable_depth_test(ctx, 0)` does too;
default behavior unchanged. GL uses LESS as default — switch the default
command bits from LEQUAL to the cached state so the frontend default
(`L10GL_LESS`, `src/l10gl.c:34`) is what actually runs.

### V4. Plumb alpha blending into the triangle command word
Same pattern as V3. The glue caches `blend_enabled` but never applies it.
When blending is enabled, set CMD_SET bits 19-18: `VIRGE_BLEND_SRC_ALPHA`
for Gouraud triangles, and for textured triangles choose
`VIRGE_BLEND_SRC_ALPHA` vs `VIRGE_BLEND_TEX_ALPHA` based on whether the
bound texture format has alpha (semantics verified, §15.4.8.5, DB019-B PDF
p. 134). Document in `l10gl.h` that ViRGE blending is fixed-function
`src*A + dst*(1-A)` and the `blend_func` factors are advisory (only
`SRC_ALPHA, ONE_MINUS_SRC_ALPHA` is honored in hardware). Two datasheet
constraints to encode: fog (bit 17) and source-alpha blending are mutually
exclusive (§15.4.8.4), and correct transparency with Z requires opaque
geometry first, then transparent draws with Z-update disabled (§15.4.8.5)
— document the latter as an application-side rule.

*Acceptance:* a demo triangle with `a = 0.5` and blending enabled renders
translucent on hardware; with blending disabled, opaque.

### V5. Add the missing `l10gl_draw_line` frontend wrapper
The vtable has `draw_line` and both backends implement it, but there is no
public `l10gl_draw_line()` in `src/l10gl.c` / `l10gl.h`. Add the thin
wrapper (same pattern as `l10gl_draw_triangle`).

*Acceptance:* compiles for both backends; a demo can draw a wireframe.

### V6. Detect actual VRAM size (CR36) instead of assuming 4MB
`virge.h:61` hardcodes 4MB with a comment explaining the risk. During
`vga_ensure_new_mmio()` (I/O port access is already set up there), read
CR36 "Configuration 1" bits 7-5. Verified decode for the base 86C325
(DB019-B PDF p. 197): 000 = 4MB, 100 = 2MB, all other values reserved;
the bits are power-on straps and reads need no unlock. VX/DX/GX variants
have different memory-size tables — gate the decode on the PCI device ID
already stored in `pci.device_id` and fall back to 2MB, the smallest
shipped config, when the decode is ambiguous or reserved. Store into
`ctx->vram_size`; texture allocation (`l10gl_virge.c:313`) already checks
against it.

*Acceptance:* boot log prints detected VRAM; on the human's card it matches
reality; texture OOM check uses the detected value.

### V7. Sub-pixel correctness pass on triangle edge setup (verify-on-HW task)
`virge_draw_triangle_gouraud` truncates vertex coordinates to int for edge
setup and computes slopes from truncated endpoints. This produces cracks
and shimmer between adjacent triangles. **DB019-B does not contain the
setup formulas** (§15.4.5.2, PDF p. 127, defers to S3-provided driver
code) — but the 86Box emulator's rasterizer does, and its derived
semantics are documented in `docs/datasheets/README.md` ("Behavioral
reference: 86Box"). The division of labor is now known: the hardware
performs sub-pixel attribute correction in X (5 fractional bits) and
uses a ceil()-based span rule; the **driver** must do the Y prestep —
TYS is the integer scanline of the bottom vertex, and TXS/TXEND01 plus
all attribute bases must be pre-stepped from the vertex by the
fractional Y distance along their respective edges. Implement exactly
that; do V9 first (it redefines what TXEND01/12 and the Y deltas mean).
Keep it behind small, separately committed steps because this touches
the proven-working path.

*Acceptance:* the cube demo shows no pixel cracks along shared face edges;
no regression in overall shape. This one requires human hardware validation
before merging further work on top.

### V8. Fix the 16bpp pixel-format split: 3D writes 1555, glue packs 565
Settled against the databook — this is a real bug, not an investigation.
The **3D engine's** 16bpp destination format is **ZRGB1555 only** (3D
CMD_SET description, DB019-B PDF p. 250), while the **2D engine's** 16bpp
mode is format-agnostic ("RGB1555 or RGB565", PDF p. 232) and the scanout
format is independently chosen by CR67 bits 7-4 — Mode 9 = 15-bit,
Mode 10 = 16-bit (PDF p. 216). The current glue packs all colors as
RGB565 (`l10gl_virge.c:25`). Consequence on a 565 console (the common
fbdev default): 2D fills scan out correctly but every Gouraud/textured
triangle is written as 555 and decoded as 565 — green shifted, colors
subtly wrong, and fills visibly disagree with triangles.

Fix: the driver must run 16bpp with **15-bit (555) scanout end-to-end**.
Read the live channel layout from `FBIOGET_VSCREENINFO`
(`green.length`: 5 vs 6); if the mode is 565, request the 555 equivalent
via fbdev (or, until Phase 3 P1 lands, fail loudly with an explanation);
pack all colors as ARGB1555 to match. Native CR67 programming of Mode 9
belongs to P6, which this task feeds.

*Acceptance:* a test pattern of pure red/green/blue/white 2D fills next
to Gouraud ramps of the same colors shows identical, unshifted color on
hardware; color packing in the glue is derived from the live mode's
channel layout, not hardcoded.

### V9. Fix triangle edge/attribute register semantics (found via 86Box)
The 86Box ViRGE emulation — which runs period S3 drivers correctly, so
its interpretation reflects what real drivers program — contradicts the
current setup in three ways (full derivation in
`docs/datasheets/README.md`, "Behavioral reference: 86Box"):

1. **TXEND01 is the span-end X at the *first (bottom)* scanline**, i.e.
   ≈ the bottom vertex X pre-stepped along edge 01 — not the middle
   vertex X that `virge.c:564` programs today.
2. **TXEND12 is the span-end X at the *middle* vertex's scanline** —
   not the top vertex X (`virge.c:565`).
3. **The Y deltas are edge-walk deltas along side 02, not plane ∂/∂y**:
   the engine walks upward and moves along edge 02 each scanline, so
   program `TdAdY = −dA/dy + slope02·dA/dx` for every attribute (color,
   Z, and in the textured path U/V/W/D). `virge.c:591-601` currently
   writes the raw y-down plane gradient — wrong sign and missing the
   edge term.

The cube demo can't see these bugs: its faces are flat-colored (all
color deltas zero) and its depth arrangement is forgiving. They will
surface the moment Phase 2 produces smooth shading or real texturing.
Because 86Box is an emulator, confirm with a minimal hardware test
first: one static triangle with known vertices and per-vertex colors,
photographed before/after — if the current code really renders it
misshapen (bottom span jumping straight to the middle vertex X), the
fix direction is proven. Fix both the Gouraud and textured paths.

*Acceptance:* a static test triangle renders with correct shape and a
smooth color ramp on hardware; the spinning cube with per-vertex (not
per-face) colors shows no shearing or color banding.

### V10. Fix the color fixed-point scale — the all-black-cube bug
Confirmed against 86Box's pixel pipeline
(`dest_pixel_gouraud_shaded_triangle`: `channel = value >> 7`, clamped
to 0–255): the integer part of the S8.7 color format **is the 8-bit
channel value (0–255)**. The comment at `virge.h:411` ("Normalized
0.0–1.0 maps to 0–128. The hardware scales to the destination channel
width internally") is wrong — there is no internal scaling.
`VIRGE_COLOR_FIXED(x)` multiplies by 128 only, so a fully saturated
channel programs intensity 1/255: the observed all-black cube rendering
at 0.4% brightness. Fix: scale by 255·128 (`(int16_t)(x * 32640.0f)`),
apply identically to color starts and deltas in both triangle paths,
and correct the header comment. Watch the delta range: gradients can
now overflow int16 for steep ramps across few pixels — clamp rather
than wrap.

*Acceptance:* the cube demo shows its six face colors at full
brightness on hardware; a black-to-white Gouraud ramp spans the full
range instead of rendering black.

---

## Phase 1 — Framework: runtime backends, build system, software reference

These unblock everything else (especially agent-side testing) and touch no
proven hardware paths.

**Status (2026-07-17): F1, F2, F3, and F5 complete.** Both hardware backends use
shared sysfs discovery and read-only probes; frontend demos autodetect at
runtime with `L10GL_BACKEND` overrides; `make` builds both backends into
`libl10gl.a` with dependency-tracked objects. README and `docs/BACKEND.md` now
describe that architecture. The plain-C swrast fallback provides offscreen PPM
and opt-in fbdev output plus a pixel-level regression suite. F4 remains
optional; Phase 2 is now unblocked.

### F1. Runtime backend registry and probe
Add to the vtable:

```c
int (*probe)(void);   /* returns >0 if this card is present, 0 if not */
```

Implement `probe()` for virge and mga1064 by refactoring their existing PCI
scan (`virge.c:142` `pci_find_device`) so probe and init share the
discovery code. The PCI-sysfs scanning helpers are currently duplicated
between the two backends — hoist them into a shared
`src/pci_scan.c`/`pci_scan.h` used by all backends (pure refactor commit,
no behavior change).

Add to the frontend:

```c
const struct l10gl_backend *l10gl_autodetect(void);
int l10gl_create_auto(struct l10gl_ctx *ctx, int w, int h, int bpp);
```

backed by a static registry array `l10gl_backends[]` in `l10gl.c`
(ViRGE listed first — priority card). Demos switch from `#ifdef
BACKEND_VIRGE` to `l10gl_create_auto`, with an optional
`L10GL_BACKEND=virge` environment override for forcing.

*Acceptance:* `./cube` with no build-time backend selection picks the ViRGE
on the test machine; `L10GL_BACKEND=mga1064 ./cube` forces the Matrox path
(and fails cleanly with "device not found" when absent).

### F2. Build all backends into a static library
Rework the Makefile: compile `src/l10gl.c`, `src/pci_scan.c`, and **all**
backends into `libl10gl.a`; link demos against it. Delete the
`BACKEND=`/`-DBACKEND_*` machinery (superseded by F1). Add `-MMD`
dependency tracking and separate object files (the current Makefile
recompiles everything from sources on each demo).

*Acceptance:* `make` produces `libl10gl.a` and all demos in one invocation;
`make clean && make` works; no `#ifdef BACKEND_*` remains in demos.

### F3. Software reference backend (`swrast`)

**Complete 2026-07-17.** The always-available final registry entry implements
top-left Gouraud/Z/textured rasterization, alpha blending, lines, rectangle
fills, perspective-correct texture coordinates, nearest/bilinear sampling,
RGB565/RGB888 offscreen output, PPM frame dumps, and opt-in mapping of existing
16/24/32-bit fbdev modes. `L10GL_FRAMES=N` gives `cube` and `textured_cube`
bounded capture runs. `make check` validates output pixels without a GPU.

Add `src/backends/swrast/`: a plain-C rasterizer implementing the full
vtable — Gouraud + Z + textured triangles, lines, rect fill — rendering
into either `/dev/fb0` (via standard fbdev mmap, no PCI) or an offscreen
malloc'd buffer with PPM/PNG dump (`L10GL_SWRAST_DUMP=frame%04d.ppm`).

This matters for three reasons: implementing agents can actually run and
screenshot the demos in CI/containers; it is the reference for validating
frontend math (Phase 2) independent of register-programming questions; and
it is the model "new backend" showing the vtable can be implemented with
zero hardware.

Keep it simple and correct, not fast: top-left fill rule, plane-equation
attribute interpolation (mirroring `virge.c:496` so results are comparable),
16bpp RGB565 and 24bpp output.

*Acceptance:* `L10GL_BACKEND=swrast ./cube` runs in a container and dumps
frames; a dumped frame visually matches the expected cube; textured_cube
works too.

### F4. 86Box-based ViRGE test rig (high value, optional)
86Box emulates the ViRGE family (`src/video/vid_s3_virge.c`) faithfully
enough to run period S3 drivers. A scripted rig — 86Box headless with a
ViRGE/DX adapter, booting a minimal 32-bit Linux disk image with an
fbdev console, the L10GL demos on the image, screenshots captured per
run — would give implementing agents a **register-level feedback loop
for ViRGE code without vintage hardware**, closing the biggest testing
gap in this plan. Steps: build 86Box in CI, prepare a reusable disk
image (build demos statically on the host, inject into the image),
drive the emulator with a fixed boot script, diff screenshots against
swrast renders of the same scene. Caveats: emulator fidelity is strong
evidence but not silicon (bilinear is an emulator option there); keep
final sign-off on real hardware. License note: 86Box is GPL-2.0 —
run it as a tool, never copy its code into this MIT project.

*Acceptance:* a single `make emu-test` (or script) boots the emulated
ViRGE machine, runs the cube demo, and produces a screenshot; a wrong
register write (e.g. reverting V1) visibly breaks the screenshot.

### F5. Update README + add `docs/BACKEND.md`
README still describes ViRGE as a "future backend" and documents the old
`BACKEND=` build. Rewrite for the post-F1/F2 world. Add `docs/BACKEND.md`:
the new-backend porting guide (vtable contract, probe/init lifecycle, who
owns vertex sorting, fixed-point conventions, the "enable-bit archaeology"
lessons from the ViRGE bring-up — CR40/CR53/AFC/DE-bit — as a checklist for
other vintage cards).

*Acceptance:* docs match the code; a new agent could start a backend from
`docs/BACKEND.md` alone.

---

## Phase 2 — Frontend geometry pipeline

**Status (2026-07-18): Phase 2 complete; X1 through X6 are verified.** Matrix
stacks and viewport math live in `src/l10gl_xform.c`; current-attribute
capture, model-space transformation, streaming primitive assembly,
eye-space directional lighting, homogeneous near clipping, texture dispatch,
perspective texture W, and post-projection culling live in
`src/l10gl_pipeline.c`. `test-xform` and `test-pipeline` cover both layers.
Existing screen-space drawing remains unchanged.

Move the 3D math out of the demos and into the library, so applications
supply model-space geometry and the library handles transform → light →
clip → project → rasterize. This is the biggest step toward "an OpenGL
driver" rather than "a triangle pusher". All of it is hardware-independent
and must be developed and validated against `swrast` (F3), then confirmed
on the ViRGE.

Add `src/l10gl_xform.c` (+ types in `l10gl.h` or a new `l10gl_xform.h`).
Keep it a layer **above** the existing draw API: `l10gl_draw_triangle`
keeps taking screen-space vertices; the pipeline emits into it. Backends
are untouched by this phase.

### X1. Matrix stacks and viewport

**Complete 2026-07-17.** MODELVIEW/PROJECTION stacks have the specified 32/4
depths and return bounded overflow/underflow errors. All matrices are
column-major and post-multiplied. Initialization uses the backend's actual
raster for the default viewport; the NDC helper converts GL lower-left
viewport coordinates into backend top-left scanout coordinates. Perspective,
frustum, ortho, arbitrary load/multiply, transform, viewport, and depth-range
behavior are deterministic under `make check`.

4x4 float matrices, MODELVIEW and PROJECTION stacks (depth 32/4),
`load_identity`, `mult`, `translate/rotate/scale`, `frustum/perspective/
ortho`, `push/pop`, plus `l10gl_viewport(x, y, w, h)` and depth range.
Column-major, GL conventions, so the Phase 4 shim is mechanical.

### X2. Immediate-mode vertex path and primitive assembly

**Complete 2026-07-17.** `l10gl_begin/end`, `vertex3f`, `color4f`, `normal3f`,
and `texcoord2f` stream triangles, alternating-winding strips, fixed-origin
fans, independent lines, and line strips through X1 into existing backend draw
calls without allocation. Binding a texture selects textured triangle dispatch;
binding NULL selects Gouraud dispatch. CCW front/back culling is computed in
NDC before the framebuffer Y flip. Normals are captured for X4 lighting and
X5 derives perspective texture W after homogeneous clipping.

`l10gl_begin(prim)` / `l10gl_vertex3f` / `l10gl_color4f` / `l10gl_normal3f`
/ `l10gl_texcoord2f` / `l10gl_end()`. Assemble TRIANGLES, TRIANGLE_STRIP,
TRIANGLE_FAN, LINES, LINE_STRIP (enums already exist, `l10gl.h:32`) into
individual triangles/lines fed through the pipeline. Include back-face
culling (`l10gl_cull_face`) computed post-projection.

### X3. Near-plane clipping

**Complete 2026-07-17.** Assembled triangles are clipped against `Z + W >= 0`
before perspective division using Sutherland-Hodgman. One input triangle emits
zero, one, or two triangles, with clip coordinates, color, alpha, normal, and
UV interpolated at new vertices. Near-boundary vertices are snapped within a
small relative epsilon to avoid numerical slivers. X/Y remain handled by the
backend clip rectangle; a frontend guard rejects projected triangles taller
than the ViRGE's 2047-line field. Far-plane crossings and lines that cross a
depth plane still use conservative whole-primitive rejection.

Mandatory before this pipeline can be trusted: the hardware cannot handle
vertices behind the eye (the legacy cube formerly dodged this with a depth
clamp). Clip assembled triangles against the near plane in clip space
(Sutherland–Hodgman, 1 plane → 0, 1, or 2 output triangles),
interpolating color/uv/z/w. Guard-band note: X/Y clipping can be left to
the hardware clip rectangle (already enabled), but clamp the post-viewport
scan counts to the 11-bit fields (`virge.h:296`) — reject or clip
triangles taller than 2047 lines.

### X4. Lighting

**Complete 2026-07-17.** Lighting is opt-in and computes one eye-space
directional diffuse term plus configurable ambient light for every submitted
vertex. `l10gl_light_dir` validates and normalizes a light-ray direction;
`l10gl_light_color`, `l10gl_light_ambient`, and `l10gl_material` configure the
RGB terms and material alpha. Normals use normalized inverse-transpose
MODELVIEW, including non-uniform and reflected transforms; singular matrices
safely produce ambient only. Lit channels are clamped to `[0,1]` and captured
before X3 clipping, which then interpolates them normally. Disabled lighting
preserves the established current-color path exactly.

Fixed-function subset: one infinite directional light + ambient
(`l10gl_light_dir`, `l10gl_light_color`, `l10gl_material`), evaluated
per-vertex from `l10gl_normal3f` in eye space, exactly replacing the math
formerly carried by `demos/cube.c`. Multiple lights and specular are stretch
goals — do not block on them.

### X5. Perspective-correct texture W

**Complete 2026-07-18.** Projected immediate-mode vertices now emit
`w = 1 / clip.w`. With the standard perspective constructors, positive clip W
is positive eye-space depth; orthographic clip W remains 1 and therefore keeps
affine interpolation. X3-generated vertices first interpolate homogeneous
clip W and only then take the reciprocal, which is the correct value at the
new near-plane intersection. The existing screen-space API and its explicit W
values are unchanged. Capture tests cover analytic perspective depths,
MODELVIEW translation, orthographic projection, textured dispatch, and
near-clipped vertices.

The pipeline computes `w = 1/z_eye` per vertex and feeds it through
(`struct l10gl_vertex.w` already exists) so ViRGE perspective-correct
texturing (`virge_draw_textured_triangle`) finally gets real W values instead
of the demo's hand-rolled ones.

### X6. Port the demos

**Complete and ViRGE-verified 2026-07-18.** `cube` now describes
only its mesh, materials, light, camera, and animation while X1-X4 perform all
transform, culling, clipping, and lighting work. `textured_cube` likewise uses
immediate-mode model-space vertices and UVs, with X5 generating the reciprocal
W consumed by the existing perspective texture paths. A 53.130102-degree
projection and a camera-convention Z reflection reproduce the legacy screen
geometry. Both demos' first 640x480 RGB565 swrast PPMs are byte-for-byte
identical to their pre-port baselines, and both pass eight-frame ASan/UBSan
smoke runs. Together the source files shrink by 160 lines. `rawtri` is now the
canonical unchanged screen-space hardware bring-up demo; `triangle` remains a
compatibility build of the same source. David confirmed the pipeline `cube`,
`textured_cube`, and `rawtri` outputs look correct on the real ViRGE/DX.

Rewrite `demos/cube.c` and `demos/textured_cube.c` on the pipeline
(begin/end, matrices, one directional light). Demos shrink dramatically;
this is the proof the pipeline is sufficient. Keep the old screen-space
demo as `demos/rawtri.c` — it is the minimal hardware-bring-up test and
must stay for backend debugging.

*Phase acceptance:* cube and textured_cube render identically (modulo
sub-pixel) to the current versions, on both swrast and ViRGE; no 3D math
remains in the demo files beyond scene description.

---

## Phase 3 — Presentation: mode setting, double buffering, vsync flip

Currently the driver draws straight to the scanout buffer of whatever mode
the console happens to be in, and trusts the caller that it *is* that mode.
This phase makes the display pipe a first-class part of the driver: adopt
or set the video mode, own the console politely, and add back-buffer
flipping. Mode setting is staged — fbdev-mediated first (small, portable,
works today), native CRTC programming last (the true "direct hardware"
end state, but the riskiest register work in the whole plan).

### P1. Mode negotiation and fbdev mode setting (all backends)
Change the init contract: `width/height/bpp` passed to `l10gl_create` are
a *request*, and the backend reports what it actually got. Concretely:

- Add `stride` (bytes per scanline) and a pixel-format descriptor (channel
  offsets/lengths, from `fb_var_screeninfo`) to `struct l10gl_ctx`, filled
  in by the backend at init.
- In backend init: open `/dev/fb0`, `FBIOGET_VSCREENINFO`. If the current
  mode already matches the request, adopt it. If not, attempt
  `FBIOPUT_VSCREENINFO` with the requested mode (this works when a real
  chip driver — `s3fb` for the ViRGE, `matroxfb` for the Mystique — is
  loaded; `vesafb`/`simplefb` are fixed-mode and will refuse). Re-read and
  use the **actual** resulting values everywhere; fail with a clear error
  if the result is unusable rather than rendering into a mismatched mode.
- Replace every `ctx->width * ctx->bpp` stride computation in both
  hardware backends with the real `finfo.line_length` — the current
  assumption breaks on any driver that pads the pitch.
- Frontend: `l10gl_create` prints requested vs actual mode; demos read the
  final geometry from the context instead of trusting their argv.

*Acceptance:* on the test machine, `./cube 800 600 16` either switches the
console to 800×600 (with `s3fb`) or fails with a message naming the actual
mode and how to set it (with `vesafb`); rendering geometry/stride always
comes from the live mode; no `width * bpp` stride math remains.

### P2. Console ownership and clean restore
Be a well-behaved fullscreen app. At init: save the current
`fb_var_screeninfo`, put the owning VT into graphics mode
(`KDSETMODE`/`KD_GRAPHICS`) so fbcon's cursor blink and kernel printk stop
scribbling over our frames. At cleanup — including the SIGINT path the
demos already catch — restore text mode and the saved fbdev mode. Do this
in the frontend/a shared `src/console.c`, not per-backend; it is pure
fbdev/VT ioctl work with no card-specific code.

*Acceptance:* Ctrl-C from any demo returns to a working text console at
the original mode, every time; no blinking cursor artifacts during
rendering.

**Partial groundwork (2026-07-17):** `tools/l10gl-run` now provides an
outside-the-process ownership handoff for machines with a kernel framebuffer:
it detaches fbcon, unbinds the `/dev/fb0` owner and selected PCI driver, runs
L10GL against the released card, then rebinds the exact drivers and fbcon. This
flow is hardware-verified end to end. It does not complete P2's in-process VT
`KD_GRAPHICS` handling or fbdev mode save/restore.

### P3. Frontend swap-buffers API
`l10gl_swap_buffers(ctx)` + vtable `swap_buffers`. Semantics: finish
pending engine work, make the back buffer visible (at next vsync if
supported), retarget rendering to the other buffer. Backends without flip
support (NULL) make it a no-op after `wait_engine` — single-buffered but
correct.

### P4. ViRGE double buffering
Allocate buffer 2 after the front buffer in the VRAM layout
(`virge.c:1084`; new layout: front, back, Z, texture heap — recheck V6
VRAM budget: 800×600×2×2 + Z 800×600×2 ≈ 2.8MB, so 4MB cards handle
800×600 double buffered, 2MB cards are capped at 640×480). Rendering
retarget = reprogram `VIRGE_3D_DEST_BASE`/`VIRGE_2D_DEST_BASE` per flip.
Scanout retarget = program the display start address over the CRTC.
Verified mechanism (DB019-B PDF p. 193): start address = CRC/CRD
(bits 15-0), extended by CR31 bits 5-4 (addr 17-16) + CR51 bits 1-0
(addr 19-18); if CR69 bits 3-0 are non-zero they become the upper address
bits and supersede the CR31/CR51 extensions entirely. Use the CR69 path —
one register instead of two split fields. Flip at vsync using the
existing `virge_wait_vsync`.

### P5. swrast + mga1064 double buffering
swrast: trivial (two malloc'd buffers or fbdev pan via
`FBIOPAN_DISPLAY`). mga1064: implement the same VRAM-layout + CRTC
start-address pattern (CRTCEXT0 on MGA), untested-on-HW but structurally
complete.

### P6. Native ViRGE mode setting (no fb-driver dependency)
The end-state for the primary card: set the mode by programming the chip
directly, so L10GL runs even on a `vesafb`/fixed-mode console — the same
"direct interaction with the hardware" philosophy as the rest of the
driver. This is the largest single block of new register programming in
the plan; keep it strictly isolated and behind an opt-in
(`L10GL_MODESET=native`) until proven.

Scope, per DB019-B (§13.4 mode setup, PDF pp. 89-101; §9 clocks, PDF
pp. 65-67; CR67, PDF p. 216):
- A small **fixed mode table** (640×480, 800×600, 1024×768 at 60/75Hz,
  16bpp) using canonical VESA timings — no general mode timing calculator.
- Memory/dot clock PLL programming (SR12/SR13, plus the SR15 load
  sequence).
- Standard VGA CRTC timing registers plus the S3 extended overflow bits
  (CR5D/CR5E), screen offset/stride (CR13 + CR51 extension), and the
  enhanced-mode bits already partially handled in bring-up (CR31/CR3A/
  CR58 linear-window size).
- Scanout pixel format via CR67 bits 7-4: program **Mode 9 (15-bit)** at
  16bpp depth so scanout matches the 3D engine's ZRGB1555 output (see
  V8); Mode 13 for 24bpp. Streams-processor scanout paths on later
  variants are out of scope until the base path works.
- **Full save/restore**: snapshot every register the modeset touches at
  init and restore on exit, so the console comes back even from a native
  modeset. Build on P2's restore path.

Reference implementations to consult (as documentation, not code to copy):
the XFree86 `s3virge` driver and the kernel `s3fb` driver both encode the
exact register sequences for these chips.

Do **not** start P6 until P1/P2 are merged and V8 is resolved. mga1064
native modesetting is deliberately out of scope here — it becomes an
M-task once the ViRGE version has proven the structure (a
`set_mode`/`restore_mode` pair on the vtable).

*Acceptance:* on a console left in a non-matching `vesafb` mode, a demo
with `L10GL_MODESET=native` sets 640×480@16bpp on the ViRGE, renders
correctly, and restores the previous console state on exit. Requires human
hardware sign-off at each sub-step (clock first — a wrong PLL can produce
a black screen or an out-of-range monitor signal; program conservative
60Hz timings only until verified).

*Phase acceptance:* cube demo on ViRGE runs at the requested resolution
with no tearing and no visible mid-frame drawing; Ctrl-C always returns a
usable console; `rawtri.c` (single-buffer path) still works.

---

## Phase 4 — OpenGL compatibility shim

A thin `include/GL/gl.h`-subset (`src/l10gl_gl.c`) mapping real GL 1.1
entry points onto the Phase 2 pipeline: `glBegin/glEnd/glVertex*/glColor*/
glNormal*/glTexCoord*`, matrix functions, `glEnable/glDisable`
(DEPTH_TEST, BLEND, CULL_FACE, TEXTURE_2D, LIGHT0), `glClear`,
`glDepthFunc`, `glBlendFunc`, `glTexImage2D`, `glBindTexture`,
`glTexParameteri`, `glViewport`, `glFrustum/glOrtho`. Context creation and
buffer swap stay L10GL-specific (`l10glCreateContext()`,
`l10glSwapBuffers()`) — that is the honest equivalent of what GLX/EGL
would provide and keeps us out of windowing-system business.

Ship `demos/gears.c` — a port of the classic glxgears drawing loop with
only the setup/swap lines changed — as the proof.

*Acceptance:* gears compiles against our `GL/gl.h` with its rendering code
unmodified, and runs on swrast and ViRGE.

This phase is deliberately last-but-one: it is mechanical once Phase 2
exists, and worthless before.

---

## Phase 5 — Matrox parity and second-card validation

Keep the promise that the architecture is multi-card. All tasks here must
not disturb ViRGE code.

- **M1.** Bring mga1064 up to the current vtable: probe (F1), the V3/V4
  state-plumbing pattern, mode negotiation via fbdev (P1, using
  `matroxfb`), swap_buffers (P3/P5). Where the Mystique lacks a feature
  (no bilinear filtering, no real alpha blending), return honest caps
  bits — the frontend already treats caps as advisory.
- **M2.** Mystique texture mapping: the MGA-1064SG has hardware texturing
  (TEXORG/TEXWIDTH/TEXHEIGHT/TEXCTL and TMR0-8 texture-mapping registers).
  Implement `tex_image_2d`/`bind_texture`/`draw_textured_triangle` per the
  MGA-1064SG spec. Untestable until the human swaps cards — mark the code
  as HW-unverified in comments, mirror the diagnostic-printf style so
  bring-up is bisectable later.
- **M3.** A bring-up checklist doc for the Mystique modeled on the ViRGE
  war stories (which enable bits gate which register banks), so the
  eventual hardware session is efficient.
- **M4.** Native Mystique mode setting, only after P6 has proven the
  structure on the ViRGE: implement the same `set_mode`/`restore_mode`
  vtable pair against the MGA-1064SG CRTC/PLL (CRTCEXT registers, system
  PLL), same fixed mode table, same save/restore discipline.

*Acceptance:* `make` builds everything; swrast-validated demos run
unchanged when `L10GL_BACKEND=mga1064` is forced on a machine with the
card (human validation, later).

---

## Phase 6 — Performance (only after everything above is stable)

Ordered by expected win/effort on the ViRGE:

1. **FIFO-aware submission.** Every draw currently spins for full engine
   idle (`virge_wait_engine`) before touching registers. Verified
   (DB019-B PDF p. 300): MM8504 read bits 12-8 report S3d FIFO slots
   free, and the FIFO is 16 slots deep. Wait for enough free slots
   instead of idle so triangle setup overlaps rasterization. Keep
   full-idle waits before CPU framebuffer access and buffer flips.
2. **Dirty-state tracking.** Skip re-writing CLIP/STRIDE/DEST_BASE per
   draw (`virge_fill_rect` reprograms them every call); cache last-written
   values in `virge_ctx`, invalidate on Z-clear's base swap.
3. **Autoexecute mode.** Program CMD_SET once per state change with
   `VIRGE_CMD_AUTOEXEC`, then per-triangle write only the geometry
   registers ending at the TY01_Y12 kick (`virge.h:295`) — roughly halves
   register traffic per triangle.
4. **Triangle-strip aware register reuse** and, much later, the ViRGE DMA
   command queue (probably not worth it for this project's goals).

Each item needs before/after frame-rate numbers from the cube demo
(add a simple FPS counter to the demos first).

---

## Cross-cutting rules for implementing agents

- **Branching/commits:** one logical change per commit, imperative summary
  line, body explaining the hardware rationale (match existing history).
  Never mix refactors with behavior changes.
- **Compile gate:** every commit must build with
  `make` (all backends, `-Wall -Wextra`) cleanly.
- **Testing:** anything touching frontend math or swrast must be
  demonstrated with swrast frame dumps. Anything touching ViRGE register
  programming cannot be verified by agents — state explicitly in the PR/
  commit what the human should observe on hardware, and keep such changes
  isolated so a failed hardware test bisects to one commit.
- **Register facts need citations.** The full datasheets are in
  `docs/datasheets/` with a section→PDF-page index in its README. When
  adding register programming, read the section first and cite it
  (ViRGE: DB019-B §/page; Matrox: MGA-1064SG spec) in the comment, as the
  existing headers do. Never write register code from memory of the chip.
- **Don't break the bring-up path.** `vga_ensure_new_mmio()` and the S3d
  enable sequence in `virge_init` encode hard-won hardware knowledge;
  changes there require explicit human sign-off.

## Suggested execution order and dependencies

```
Phase 0 (V1..V10) — independent of each other; V7/V8/V9 need HW
                    sign-off; V9 before V7 (V9 redefines the registers
                    V7 presteps); V10 first — it unblocks seeing
                    anything at all on hardware
Phase 1 (F1→F2, F3, F4, F5) — F1 before F2; F3/F4 independent; F5 last
Phase 2 (X1→X2→X3→X4/X5→X6) — requires F3 for validation
Phase 3:
  P1→P2 (fbdev modeset + console restore) — no dependencies; can start
        immediately, and everything else benefits from landing it early
  P3→P4, P5 (double buffering) — require F1/F2
  P6 (native modeset) — last in phase; requires P1, P2, V8 resolved
Phase 4            — requires Phase 2 + 3
Phase 5 (M1→M2→M3→M4) — M1 requires F1/F2 and P1; M4 requires P6
Phase 6            — last
```

Good first parallel assignment for three agents: (1) Phase 0 fixes,
(2) F1+F2 backend registry/build, (3) P1+P2 fbdev mode negotiation and
console restore (F3 swrast is the next pickup after any of these).
