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
- ViRGE's maximum texture side is documented as 512 texels (DB019-B
  section 19.4, PDF p. 251). The Mystique limit still needs confirmation
  before Phase 5 exposes its texture path.
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

The P1 mode contract and P2 console ownership/restore are now implemented;
P3/P4 ViRGE swapping landed earlier in bring-up.
This phase makes the display pipe a first-class part of the driver: adopt
or set the video mode, own the console politely, and add back-buffer
flipping. Mode setting is staged — fbdev-mediated first (small, portable,
works today), native CRTC programming last (the true "direct hardware"
end state, but the riskiest register work in the whole plan).

### P1. Mode negotiation and fbdev mode setting (all backends)

**Implemented and native-ViRGE-verified 2026-07-18; fbdev mode-switch sign-off
pending.** `l10gl_ctx` now publishes authoritative stride and packed channel
layout in addition to actual geometry. A shared fbdev path reads both fixed and
variable mode data, attempts `FBIOPUT_VSCREENINFO` when geometry/depth/required
channels differ, re-reads the result, and rejects ignored or unusable modes
with the live mode and an example `fbset` command. ViRGE requires its fixed
RGB555 destination layout; MGA-1064 uses real padded pitch for engine and Z
layout; swrast accepts the live packed channel layout. Offscreen and native
ViRGE-takeover modes publish the same contract without fbdev. `test-mode`
covers standard layouts, matching, padded strides, actual-mode publication,
and frontend validation. David confirmed the primary ViRGE/DX machine's
no-fbdev native takeover still works without regressions after P1. Actual
`FBIOPUT_VSCREENINFO` acceptance still needs a machine booted with
`s3fb`/`matroxfb`.

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

**Complete and hardware-verified 2026-07-18.** Backends now declare their
fbdev target in the vtable. Before
backend initialization, `src/console.c` snapshots `fb_var_screeninfo`, finds
the active VT, verifies that VT is mapped to the target framebuffer when the
kernel supports `FBIOGET_CON2FBMAP`, saves its existing KD mode, and enters
`KD_GRAPHICS`. Destruction reverses ownership after backend cleanup: restore
the exact saved fbdev mode first, restore the prior KD mode, then close both
descriptors. Initialization failures unwind the same state. Offscreen swrast
and the primary no-fbdev ViRGE takeover do not touch a VT. `test-console`
covers acquisition, inactive framebuffer mapping, failure unwind, exact mode
restoration, restore-error continuation, and idempotent release.

David verified both target-machine ownership paths. `l10gl-run -- ./cube`
unbinds `simple-framebuffer`, leaves P2 inactive in the child, renders normally,
and rebinds fbcon after Ctrl-C. A direct 800x600x32 swrast run through
`/dev/fb0` reported `VT1 is in KD_GRAPHICS`; its static frame remained clean,
and Ctrl-C reported `restored fbdev mode and console ownership` before the
console resumed. This satisfies P2 acceptance. P1's separate request-to-switch
branch still needs `s3fb`/`matroxfb`; adopting an already matching live mode is
verified by this swrast run.

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

**Complementary outside-process handoff (2026-07-17):** `tools/l10gl-run` provides an
outside-the-process ownership handoff for machines with a kernel framebuffer:
it detaches fbcon, unbinds the `/dev/fb0` owner and selected PCI driver, runs
L10GL against the released card, then rebinds the exact drivers and fbcon. This
flow is hardware-verified end to end. When that flow removes `/dev/fb0` before
the child starts, the in-process P2 layer correctly remains inactive.

**Console-pixel redraw follow-up (2026-07-18; hardware verified after a clean
boot):** a
P6 native run restored the correct raster but left the released 16-bit color
buffers and all-ones Z buffer visible through the rebound 32-bit simplefb,
appearing shifted upward with a white bottom. This is the already-diagnosed
incoherent-BAR console-pixel problem, not a CRTC shift. After rebind/reattach,
`l10gl-run` now switches from the active VT to a spare VT and immediately back;
fbcon redraws the retained text cells without clearing the user's console.
The initially reported post-exit displacement survived repaint because the
pre-run console itself had already been left in that displaced state by an
earlier failed modeset. A reboot restored the firmware/simplefb baseline; from
that clean baseline the P6 run and launcher hand-back returned a correct
console. Save/restore therefore returns the exact inherited state as designed.

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

**Implemented 2026-07-18; simple-framebuffer fallback verified, true swrast
fbdev page-flip and MGA hardware sign-off pending.**
Offscreen swrast
now rotates two private color buffers, and PPM output reads the completed
buffer only after `l10gl_swap_buffers`. The fbdev path requests a second
virtual page and uses `FBIOPAN_DISPLAY` with `FB_ACTIVATE_VBL` when the driver
and mapped VRAM support it. Otherwise it remains direct single-buffered; a CPU
copy is not a page flip and lets scanout observe horizontal bands while the
copy is in progress. David observed exactly that regression on the fixed
`simple-framebuffer`, so the CPU-copy path was removed. `test-swrast` proves
that no offscreen frame is dumped before swap and that consecutive swaps
preserve distinct completed frames; `test-mode` checks virtual-page selection
and rejects pages beyond `smem_len`. David confirmed the corrected
simple-framebuffer fallback renders a clean static frame. Continuous animation
still shows horizontal bands because that fixed driver exposes only one page,
so scanout necessarily observes software rendering in progress; this is the
expected single-buffer limitation, not a geometry/stride regression.

MGA-1064 now plans front/back/Z surfaces around the live CRTC scanout without
overlap, uses the real padded stride for each allocation, and falls back to a
correct single-buffer path when VRAM cannot hold three surfaces. Swap waits for
engine idle and bounded vsync, writes the 20-bit start address in the
datasheet-mandated CRTCC/CRTCD/CRTCEXT0-last order, then retargets YDSTORG to
the old front. Cleanup restores both the saved console pixels and exact start
registers. `test-mga1064` covers a 4MB 800x600x16 layout, nonzero live fronts,
32bpp capacity fallback, alignment rejection, and the 8-byte-unit CRTC
encoding. This path is structurally complete but remains untested on a real
Mystique with `matroxfb`.

### P6. Native ViRGE mode setting (no fb-driver dependency)

**P6a implemented 2026-07-18; hardware-inert foundation complete.**
`src/backends/virge/virge_mode.c` defines the six fixed 640x480, 800x600,
and 1024x768 modes at 60/75 Hz, validates that each timing fits the verified
16bpp horizontal-x2 ViRGE CRTC representation, and computes SR12/SR13 DCLK
parameters from the databook formula and documented PLL/VCO limits. The first
hardware pass remains restricted to the conservative 60 Hz entries. The new
`test-virge-mode` checks every timing, sync polarity, invalid-mode rejection,
all six clocks' representability, and exact PLL encodings for the three 60 Hz
clocks. Nothing calls this module from `virge_init` yet, so this commit adds no
register writes and cannot change the proven takeover path. P6b is the pure
CRTC encoder plus complete register snapshot/restore description; the opt-in
writer follows only after those bytes are covered by tests.

**P6b implemented 2026-07-18; still hardware-inert.** The pure encoder now
builds a complete masked standard/extended CRTC image, sequencer PLL/unlock
image, Misc Output value, and DAC mask for any fixed 16bpp mode. Nonzero masks
are the exact register snapshot set the eventual writer must restore. The
800x600 image reproduces the silicon-verified takeover values (`CR00=03`,
`CR01=c7`, `CR13=c8`, `CR3B=ea`, `CR5D=21`, `CR67=30`); every fixed mode
round-trips through the encoded overflow fields to its requested geometry and
pitch. The FIFO-fetch policy preserves the target BIOS mode's verified 3 us
refill interval at each dot clock. P6c is the opt-in save/apply/restore writer;
its first 800x600@60 clock-isolation step is hardware-verified.

**P6c first silicon gate implemented and hardware-verified 2026-07-18.**
`L10GL_MODESET=native` is now an explicit opt-in on the ViRGE no-fbdev path,
but is deliberately restricted to 800x600@60 for its first run. That is the
target machine's already-proven raster and reproduces P6b's verified takeover
bytes, isolating the new SR12/SR13/SR15 programmable-clock load from a
simultaneous resolution change. The path refuses to run while `/dev/fb0`
exists (use `tools/l10gl-run`), snapshots every masked CRTC/sequencer register
plus Misc Output and DAC mask, blanks the screen, loads the PLL and CRTC,
requires masked readback to match, then restores the exact saved register
bytes at cleanup. Default operation remains the hardware-verified live-raster
takeover. After the 800x600 run and restore are confirmed, enable 640x480@60
as the first actual resolution change. That confirmation is complete and P6d
now exposes 640x480 for its isolated silicon test; keep 75 Hz and 1024x768
gated (the
latter also exceeds a 4MB card's double-buffer+Z budget).

The first P6c run stayed synchronized but showed roughly 100-115 black lines
at the top; after cleanup the simple-framebuffer contents were displaced and
white at the bottom. This gate had rewritten the complete vertical VGA timing
set even though the intended experiment was only the already-live 800x600
raster plus a new PLL load. The corrective gate now retains the live vertical,
panning, and addressing-control registers and writes only the exact
horizontal/depth/pitch/start subset already proven by `virge_scanout_takeover`,
plus SR12/SR13/SR15 and the programmable-DCLK select. Cleanup repeats the saved
display-start bytes after restoring the old PLL and waits one retrace before
unblanking, ensuring the console start is latched in its own clock domain.
The corrective run removed the black band completely. The apparent remaining
exit displacement was inherited from an already-corrupted pre-run console,
not introduced by the corrected restore. After reboot re-established the
firmware/simplefb baseline, the same P6 run rendered correctly and returned to
a correct framebuffer console. The 800x600 PLL/save/restore gate is closed;
640x480@60 is now the next P6 resolution-change gate.

**P6d 640x480@60 resolution gate implemented and silicon-verified
2026-07-18.** The opt-in native path now admits 640x480
alongside the verified 800x600 clock gate. Unlike 800x600, 640x480 applies the complete standard and
extended CRTC image because it is a real raster change. The image is locked by
tests to DB019-B's exact field encodings: CR00-CR18, CR5D/CR5E overflow, pitch,
negative sync polarity, the exact built-in 25.175MHz VGA DCLK, and the 3us
FIFO-refill budget. The split-screen line compare was reduced from the
unnecessary ViRGE-extended `0x7ff` to standard VGA `0x3ff`;
all supported rasters are shorter than 1024 lines, this keeps split-screen
disabled, and it preserves `CR5E=00` as observed in the proven live mode.
The first silicon run reached the programmed CRTC image but the monitor
reported out of range. DB019-B section 9.2 identifies the cause in the clock
selection: standard 25.175MHz is a dedicated VGA DCLK selected by Misc Output
bits 3-2=`00`, while the first image had needlessly selected programmable PLL
source `11` and synthesized 25.255MHz. The corrected image selects the exact
built-in clock and sets SR15.1 while selecting it, as the same section
requires. SR12/SR13/SR15 remain fully saved and restored because fixed-clock
selection can update the PLL parameter registers implicitly.

The corrected fixed-clock run still produced an out-of-range signal. Its
readback proves clock select `00` (`Misc=e3`, with unrelated firmware bit 5
preserved) and DFRQ enable (`SR15=03`) were active, so further timing changes
must not be guessed from PLL state. The next diagnostic reads DB019-B Input
Status 1 directly: bit 0 measures 128 horizontal-retrace periods and bit 3
measures three vertical-retrace periods, both before and after the modeset.
It also reports SR01 and CR43, the documented dot-clock divide and horizontal
counter-double controls. Those measured rates will identify the remaining
counter-scaling error.

The hardware measurement returned a correct live baseline of 37.854kHz /
60.279Hz and a correct native 640x480 raster of 31.321kHz / 59.664Hz. The
monitor nevertheless rejected only the native transaction and recovered after
the exact register restore. This exonerates the DCLK and internal CRTC timing
and moves the gate to the external signal path. DB019-B shows that CR33.3=0
does not unconditionally select internal DCLK: it permits feature-connector
VCLK when that path is enabled, while CR33.3=1 forces internal DCLK. The next
gate therefore forces CR33.3, normalizes SR0D's feature/DPMS H/V overrides,
and clears Feature Control bit 3 so VSYNC cannot be ORed with active display.
All three states are snapshotted, verified, logged, and restored. The verified
800x600 clock-only gate explicitly excludes these new writes.

That external-path run still failed; its inherited state was already normal
(`FCR=00`, `SR0D=02`, `CR33=00`, `CR56=00`), and forcing CR33.3 made no
difference. Linux `drivers/video/fbdev/s3fb.c` then exposed the actual pulse
encoding error: it explicitly clears CR5D bits 3/5 as length extensions and
omits them from its horizontal blank/sync end field maps. That agrees with
DB019-B Table 14-1 and CR5D: bit 3 extends blank by 64 DCLKs and bit 5 extends
HSYNC by 32 DCLKs. They are not high bits of absolute end positions. P6d had
encoded 640x480 as `CR5D=28`, incorrectly adding both extensions even though
the desired blank and sync widths fit the base wrapping fields. The corrected
640 image is `CR5D=00`; CR33 returns to s3fb's normal non-DDR value. P6c's
already-accepted 800 clock-only gate retains `CR5D=21` explicitly so this
correction cannot silently change that signed-off path.

The corrected `CR5D=00` image passed on the target ViRGE/DX: the monitor
accepted 640x480, the rotating cube was visible, internal sync measured about
31.33kHz / 59.67Hz, Ctrl-C restored the complete pre-mode register image, and
the monitor recovered to the 800x600 simplefb console. P6d is closed. Keep
75Hz and 1024x768 locked for the next staged gates.

**P6e 640x480@75 refresh/PLL gate implemented and silicon-verified
2026-07-18.** `L10GL_REFRESH=75` selects the existing fixed 75Hz timing
only with `L10GL_MODESET=native`; omitting it retains the proven 60Hz default.
This gate deliberately reuses 640x480's verified resolution, pitch, RGB555
layout, and 4MB double-buffer+Z allocation, isolating the new 31.5MHz
programmable DCLK and 75Hz timing image. DB019-B section 9.1's M/N/R and
135-270MHz VCO limits produce `SR12=63`, `SR13=56`, 31.500MHz at 13ppm error;
section 9.2's immediate-load sequence is the same SR12/SR13 plus SR15.5 pulse
already proven by P6c. Unit tests pin all 25 standard CRTC bytes, `CR5D=00`,
the 3us FIFO-fetch value, sync polarity, and PLL bytes. At the P6e checkpoint,
800x600@75 and both 1024x768 entries remained rejected.

The first P6e silicon run synchronized and restored the console correctly, but
the top half of the active image was black and only the lower half of the cube
was visible. DB019-B's CR15/CR16 definition explains the almost exact
half-frame boundary: CR16 is the low eight bits of the blank width plus
`(CR15 - 1)`, not an absolute `vtotal - 1` endpoint. The encoder programmed
`CR16=f3` for a 500-line raster whose last vertical counter value is `0x1f2`.
Because `0xf3` was never reached before wrap, blanking remained active until
that low byte recurred at line 243. The corrected image uses `CR16=f2`
(`vtotal - 2`), and the general test now derives CR16 from the documented
blank-width formula. This also corrects 640x480@60 from `0c` to the canonical
`0b`. The corrected 75Hz retest displayed the complete cube with no black
upper half, closing P6e. The 60Hz regression also displayed correctly with
the corrected CR16 byte, so both 640x480 refreshes are signed off. The next
isolated gate is 800x600@75; keep 1024x768 locked.

**P6f 800x600@75 full-timing gate implemented and silicon-verified
2026-07-18.** The 800x600@60 transaction remains the byte-for-byte signed-off
clock-only limiter, while `L10GL_REFRESH=75` now applies the complete fixed
800x600@75 CRTC image. DB019-B's PLL constraints produce 49.516MHz from the
49.500MHz target (`SR12=44`, `SR13=51`, 332ppm, 198.066MHz VCO), using the
same immediate SR15.5 load already proven at 40 and 31.5MHz. Tests pin all 25
standard CRTC bytes, positive sync polarity, `CR3B=e2`, `CR5D=01`, and the
corrected `CR15=57 CR16=6f` vertical blank. The full gate retains pre/native
sync measurement and complete state restoration. Both 1024x768 entries remain
locked because their 4MB buffer/Z layout needs a separate policy.

P6d's signed-off 60Hz hardware test over SSH:

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native \
  tools/l10gl-run -- ./cube 640 480 16
```

Acceptance confirmed: the monitor synchronizes at 640x480, the cube is visible,
and Ctrl-C returns to the pre-run 800x600 console.

P6e hardware test over SSH:

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native L10GL_REFRESH=75 \
  tools/l10gl-run -- ./cube 640 480 16
```

The corrected gate is hardware-verified: the complete cube is visible at
640x480@75 without the former black upper half, and the first run already
proved cleanup recovery of the original 800x600 simplefb console. The target
readback is `CR15=df CR16=f2`. P6f subsequently opened 800x600@75; both
1024x768 modes remain locked for their own staged gates.

P6f hardware test over SSH:

```
sudo env L10GL_BACKEND=virge L10GL_MODESET=native L10GL_REFRESH=75 \
  tools/l10gl-run -- ./cube 800 600 16
```

Hardware result: measured sync was 46.893kHz / 75.032Hz; PLL and CRTC readback
matched exactly (`SR12=44 SR13=51`, `CR15=57 CR16=6f`, `CR3B=e2`,
`CR5D=01`). The complete cube was stable with no black band, and Ctrl-C
restored the original 800x600 simplefb console. P6f is closed.

**P6 is complete for the 4MB target card.** By project decision, skip the
1024x768 silicon gates: two 1024x768 RGB555 color pages plus a 16-bit Z buffer
need 4,718,592 bytes before textures, exceeding the detected 4MB VRAM. The
fixed 1024x768 timing entries and pure encoder tests remain useful reference
coverage, but `virge_init` intentionally rejects them rather than weakening
the verified double-buffer+Z presentation contract. Phase 4 is next.

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
  The 1024×768 entries remain encoder-tested but are deliberately unavailable
  on the 4MB target because the required two color pages plus Z do not fit.
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

**G1 implemented 2026-07-18; automated frontend checkpoint complete.**
`include/GL/gl.h` now publishes the first honest GL 1.1 subset and
`src/l10gl_gl.c` supplies a single-current-context bridge over Phase 2.
Implemented calls cover triangle/strip/fan and line immediate mode with
current color/normal/texture coordinates; MODELVIEW/PROJECTION matrices and
stacks; viewport, depth range, frustum, and ortho; independent color/depth
clears; depth, blend, cull, lighting/light0, normalize, and texture-2D enable
state; finish/swap dispatch; and first-error latching through `glGetError`.
The L10GL-specific `l10glCreateContext`/`l10glDestroyContext`/
`l10glSwapBuffers` calls own fullscreen setup and presentation, while
`l10glMakeCurrent` lets tests and native L10GL applications install an
existing context. `test-gl` drives the real transform/primitive frontend
through a capture backend, and the full `make check` suite passes.

**G2 implemented and hardware-verified 2026-07-18.** The streaming primitive
assembler now accepts `GL_QUADS` and
`GL_QUAD_STRIP`, splitting them with preserved winding and attributes without
allocating. Flat shading preserves each triangle's final submitted vertex as
the provoking color, including after near clipping; smooth shading retains
the established Gouraud path. `glLightfv` maps LIGHT0 ambient, diffuse, and a
MODELVIEW-transformed directional position (`w=0`) to Phase 2, while
`glMaterialfv` covers front/front-and-back ambient/diffuse material. The new
`gears` demo keeps its geometry/rendering body on GL calls and changes only
fullscreen setup, swap, and process lifecycle to L10GL. Its 320x240 RGB565
swrast frame was rendered and visually inspected successfully. David then ran
the same demo through `tools/l10gl-run` on the 4MB ViRGE/DX and reported that
it renders correctly; the G2 silicon gate is closed.

**G3 implemented and hardware-verified 2026-07-18.**
The shim now owns GL texture names and implements `glGenTextures`,
`glDeleteTextures`, `glIsTexture`, `glBindTexture`, `glTexImage2D`,
`glTexParameteri`, and `glPixelStorei(GL_UNPACK_ALIGNMENT)`. RGB/RGBA
unsigned-byte uploads are unpacked with the GL row alignment and converted to
backend ARGB8888. The cross-backend contract is level zero, square
power-of-two images through 512x512: DB019-B section 19.4 (PDF p.251)
explicitly caps the ViRGE side exponent at 9. Per-object filter/wrap metadata
is reapplied on bind over the backend's single active selectors. Deleting a
name unbinds and frees shim metadata; swrast allocations and ViRGE bump-heap
storage remain owned by the context and are reclaimed at teardown.

`test-gl` covers name lifecycle, RGBA and padded-RGB conversion, parameters,
enable/disable dispatch, and invalid inputs, then renders a 2x2 GL texture
through real swrast and verifies all four framebuffer quadrants from its PPM.
The `gltexture` proof renders a repeated 64x64 RGBA8888 pattern; its 320x240
RGB565 swrast frame was visually inspected successfully. David then ran the
same proof on the 4MB ViRGE/DX and confirmed that the repeated colored checker
rendered correctly. G3 is closed.

**G4 complete 2026-07-18; Phase 4 accepted.** `gears` compiles against
L10GL's `<GL/gl.h>` with its rendering body expressed entirely in GL calls and
runs correctly on both swrast and ViRGE. The separate `gltexture` proof closes
the GL texture-object path on those same two backends. Clean `make check`,
ASan/UBSan coverage of the shim, correct swrast reference frames, and both
ViRGE visual runs satisfy the Phase 4 acceptance criteria. Phase 5 is next.

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

**Deferred by project decision 2026-07-18.** The current MGA-1064 code and
tests remain supported and must not regress, but no Mystique hardware session
is planned now. Skip M1-M4 and proceed directly to Phase 6. Reopen this phase
only when a Matrox card is installed and available for validation.

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

**B0 FPS instrumentation and hardware baseline completed 2026-07-18.**
`src/l10gl_fps.c` provides a shared `CLOCK_MONOTONIC` counter with
pure timestamp-injection tests. `cube`, `textured_cube`, and `gears` count a
frame only after engine completion and buffer swap, print parser-friendly
two-second interval plus cumulative FPS, and print a final whole-run average.
This deliberately includes vsync/presentation in the user-visible rate.
The 800x600 RGB555, 600-frame ViRGE/DX baseline is: `cube` 57.37 FPS,
`textured_cube` 30.01 FPS, and `gears` 30.13 FPS. `test-fps` pins interval
rollover and whole-run math.

**Item 1 FIFO-aware submission hardware-verified 2026-07-18.** MM8504 bits
12-8 are decoded as the documented free
count for the 16-entry S3d FIFO. Fill, Z clear, line, Gouraud, and textured
submission are split into explicitly counted groups no larger than 16, with
the command kick last in each primitive. Full-idle waits remain before CPU
linear-VRAM access, page flips, and teardown. All three 600-frame workloads
completed on the ViRGE/DX with normal console recovery. Results were `cube`
57.74 FPS (+0.64%), `textured_cube` 30.13 FPS (+0.40%), and `gears` 30.13
FPS (unchanged). This is performance-neutral under the presentation-limited
benchmark but removes unnecessary full-idle serialization safely.

**Item 2 dirty-state tracking hardware-verified 2026-07-18.** A
hardware-independent register cache computes dirty
masks before reserving FIFO entries. The 2D path tracks destination base,
stride, and both clip registers; Z clear leaves the cache accurately pointed
at Z instead of eagerly restoring two framebuffer registers. The 3D path
tracks seven shared registers and invalidates only `Z_STRIDE` and `TEX_BASE`
after a 2D command, matching the established ViRGE/DX silicon quirk. Buffer
flips and texture binds become normal value changes. Cleanup reports emitted
versus considered 2D/3D state writes, and `test-virge-mode` pins initial,
unchanged, changed-value, and targeted-invalidation behavior. All three
600-frame workloads rendered correctly and restored the console. Results were
`cube` 58.49 FPS, `textured_cube` 30.11 FPS, and `gears` 30.13 FPS. The cache
emitted 1203/4800 2D writes in every run and only 1806/19033, 2566/19033, and
1806/1643579 3D shared writes respectively.

**Direct-front raw-performance gate hardware-exercised 2026-07-18.** Strict
`L10GL_VSYNC=0` selects a visible ViRGE front
buffer at offset 0. End-of-frame swaps still drain the engine so FPS counts
completed work, but they do not write the CRTC display start or wait for
vertical retrace. The one-color-buffer layout moves Z and the texture heap one
frame earlier in VRAM. The synchronized double-buffer path remains the
default (`L10GL_VSYNC=1`). `test-virge-mode` pins parsing, both 800x600 layout
images, reclaimed capacity, and rejection of invalid/overflowing layouts.
Tearing and visible partial clears are expected in direct mode. Hardware
results were `cube` 63.85 FPS over all 600 frames, `textured_cube` 32.24 FPS
over 297 frames, and `gears` 34.56 FPS over 217 frames; the latter two were
manually stopped because of the severe expected tearing, but their interval
rates were stable. Console recovery succeeded after every run.

**Item 3 autoexecute tested and rejected on ViRGE/DX 2026-07-18.** DB019-B
section 15.4.3 and CMD_SET bit 0 (absolute PDF pp.110 and
250) define B57C/TY01_Y12 as the triangle kick while AE is set. A one-entry
command cache writes B500 only after a 2D command or when 3D command state
changes, then every triangle ends with B57C. Cleanup uses the documented
AE-clear 3D NOP. On real DX silicon this path was slower for every workload:
direct-front `cube` fell from 63.85 to 22.19 FPS, `textured_cube` from 32.24
to 4.58 FPS with visible rendering corruption, and `gears` from 34.56 to 26.51
FPS. The same binary with `L10GL_AUTOEXEC=0` restored textured cube to 30.11
FPS synchronized, exactly matching the legacy baseline. The B500 launch is
therefore the default again. `L10GL_AUTOEXEC=1` retains the AE path only for
diagnosis or other-chip research; tests pin parsing, command images, B57C,
cache reuse/change, and 2D invalidation.

**Item 4 triangle-parameter reuse tested and rejected on ViRGE/DX
2026-07-18.** DB019-B describes the B500-B57C triangle parameters as
read/write registers and documents executing commands with different
parameters, but it does not promise that execution preserves the effective
interpolation state for later partial programming. Strict
`L10GL_TRI_REUSE=1` skipped identical color/Z, texture, and edge-geometry
values while retaining unconditional `TY01_Y12` and legacy B500 launches and
invalidating every cache after 2D work. Real DX output still rendered but was
severely corrupted and visually unstable ("completely borked" and "quite
trippy"). That proves full triangle parameter images are required on this
silicon. `L10GL_TRI_REUSE=0` remains the default and the opt-in is retained
only to reproduce the rejected experiment; item closed without FPS analysis
because it failed the correctness gate.

Ordered by expected win/effort on the ViRGE:

1. **FIFO-aware submission.** Previously every draw spun for full engine
   idle (`virge_wait_engine`) before touching registers. Verified
   (DB019-B PDF p. 300): MM8504 read bits 12-8 report S3d FIFO slots
   free, and the FIFO is 16 slots deep. Wait for enough free slots
   instead of idle so triangle setup overlaps rasterization. Keep
   full-idle waits before CPU framebuffer access and buffer flips.
   Hardware-verified; performance-neutral in the current vsync-limited runs.
2. **Dirty-state tracking.** Skip re-writing CLIP/STRIDE/DEST_BASE per
   draw (`virge_fill_rect` reprograms them every call); cache last-written
   values in `virge_ctx`, invalidate on Z-clear's base swap.
   Hardware-verified; synchronized presentation remains the dominant limiter.
3. **Autoexecute mode.** Program CMD_SET once per state change with
   `VIRGE_CMD_AUTOEXEC`, then end each triangle at the TY01_Y12 kick. After
   dirty-state tracking this saves one B500 write per same-state triangle.
   Tested and rejected on ViRGE/DX: severe performance loss plus textured
   corruption. Legacy B500 submission remains the default; item closed.
4. **Triangle-strip aware register reuse.** Tested and rejected on ViRGE/DX:
   omitting unchanged parameters corrupts rendering. Full per-triangle images
   remain mandatory; item closed. The much later ViRGE DMA command queue is
   probably not worth it for this project's goals.

Each item needs before/after frame-rate numbers from the same demo, mode, and
frame count. FPS instrumentation is now present; retain the raw interval and
final-average logs with every optimization checkpoint.

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
