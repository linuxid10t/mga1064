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

## Current state snapshot (July 2026)

Works (verified on a real ViRGE):
- PCI discovery, BAR0 mapping, the full CR40/CR53/CR31/CR66/AFC/S3d-reset
  enable sequence (`src/backends/virge/virge.c:51`, `virge.c:992`)
- 2D rectangle fill and Z clear (`virge.c:322`, `virge.c:386`)
- 3D Gouraud triangle path (`virge.c:473`) — the cube demo spins

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

## Phase 0 — ViRGE correctness fixes

Small, independent bug fixes. Do these first; each is one commit. These are
in the primary card's already-live paths, so regressions here are expensive.

### V1. Fix the 2D line CMD_SET mask bug
`src/backends/virge/virge.c:731`:

```c
uint32_t cmd = ((0x06 << 27) & 0x1F)  /* 2D line = bits [30:27] = 0110 */
```

`(0x06 << 27) & 0x1F` evaluates to **0** — the command-type field is erased
and the register write at `0xA900` programs a BitBLT (command 0), not a line.
The intended value is `(0x06 << 27)` with no mask (or `& (0x1F << 27)` if a
mask is wanted at all). Also note the line path never sets
`VIRGE_2D_CMD_DRAW_ENABLE` (bit 5) — the same "engine runs but writes no
pixels" trap already fixed for rect fill in commit `98a6169`. Set it.

*Acceptance:* `virge_draw_line` programs command type 0110 with DE set;
compiles under `make BACKEND=virge`; human verifies a line-drawing test on
hardware.

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
- enabled → `VIRGE_ZB_NORMAL` + map `enum l10gl_depth_func` to `VIRGE_ZBC_*`
  (the enums at `virge.h:337` cover all 8 GL functions; note ViRGE compares
  `Zs <op> Zzb`, same operand order as GL — verify LESS maps to
  `VIRGE_ZBC_LESS`, no inversion)
- `depth_writes_enabled` → `VIRGE_ZUP_ENABLE`/`VIRGE_ZUP_DISABLE`

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
bound texture format has alpha. Document in `l10gl.h` that ViRGE blending
is fixed-function `src*A + dst*(1-A)` and the `blend_func` factors are
advisory (only `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` is honored in hardware).

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
CR36 "Configuration 1" bits 7-5 and map to fitted VRAM (per DB019-B:
000=4MB, 100=2MB VX variants differ — gate the decode on the PCI device ID
already stored in `pci.device_id` and fall back to 2MB, the smallest
shipped config, when the decode is ambiguous). Store into
`ctx->vram_size`; texture allocation (`l10gl_virge.c:313`) already checks
against it.

*Acceptance:* boot log prints detected VRAM; on the human's card it matches
reality; texture OOM check uses the detected value.

### V7. Sub-pixel correctness pass on triangle edge setup (verify-on-HW task)
`virge_draw_triangle_gouraud` truncates vertex coordinates to int for edge
setup and computes slopes from truncated endpoints. This produces cracks
and shimmer between adjacent triangles. Per the DB019-B triangle
programming example: XSTART/XEND are S11.20 (already the case) but the Y
walk starts at the integer scanline of the bottom vertex — the start X must
be adjusted by the sub-pixel Y offset (`x_start += dXdY * frac(y_bot)`),
and similarly attribute starts (Z/R/G/B/A) should be evaluated at the
first sample point, not at the raw vertex. Implement the adjustment; keep
it behind small, separately committed steps because this touches the
proven-working path.

*Acceptance:* the cube demo shows no pixel cracks along shared face edges;
no regression in overall shape. This one requires human hardware validation
before merging further work on top.

### V8. Verify 16bpp destination format vs scanout format (555 vs 565)
`virge.h:316` notes the S3d engine's 16bpp destination format is
**ZRGB1555**, but the glue converts colors as RGB565 (`l10gl_virge.c:25`)
and the fbdev console mode may scan out either 555 or 565 depending on how
the fb driver set up the DAC/streams. If the engine interpolates/writes
1555 while the display decodes 565 (or vice versa), colors are subtly wrong
(green channel shifted, intensity off). Read the actual channel layout from
`FBIOGET_VSCREENINFO` (`red/green/blue.offset/length`), make the glue's
color packing match it, and confirm from DB019-B what the S3d 16bpp
destination write format really is at our mode. This is investigation +
small fix, not a feature; it also feeds directly into Phase 3's mode work,
where we get to *choose* the scanout format instead of guessing it.

*Acceptance:* a test pattern of pure red/green/blue/white fills and Gouraud
ramps shows correct, unshifted colors on hardware; color packing in the
glue is derived from (or validated against) the live mode, not hardcoded.

---

## Phase 1 — Framework: runtime backends, build system, software reference

These unblock everything else (especially agent-side testing) and touch no
proven hardware paths.

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

### F4. Update README + add `docs/BACKEND.md`
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
4x4 float matrices, MODELVIEW and PROJECTION stacks (depth 32/4),
`load_identity`, `mult`, `translate/rotate/scale`, `frustum/perspective/
ortho`, `push/pop`, plus `l10gl_viewport(x, y, w, h)` and depth range.
Column-major, GL conventions, so the Phase 4 shim is mechanical.

### X2. Immediate-mode vertex path and primitive assembly
`l10gl_begin(prim)` / `l10gl_vertex3f` / `l10gl_color4f` / `l10gl_normal3f`
/ `l10gl_texcoord2f` / `l10gl_end()`. Assemble TRIANGLES, TRIANGLE_STRIP,
TRIANGLE_FAN, LINES, LINE_STRIP (enums already exist, `l10gl.h:32`) into
individual triangles/lines fed through the pipeline. Include back-face
culling (`l10gl_cull_face`) computed post-projection.

### X3. Near-plane clipping
Mandatory before this pipeline can be trusted: the hardware cannot handle
vertices behind the eye (the cube demo dodges this with a `z < 0.1` clamp,
`demos/cube.c:57`). Clip assembled triangles against the near plane in clip
space (Sutherland–Hodgman, 1 plane → 0, 1, or 2 output triangles),
interpolating color/uv/z/w. Guard-band note: X/Y clipping can be left to
the hardware clip rectangle (already enabled), but clamp the post-viewport
scan counts to the 11-bit fields (`virge.h:296`) — reject or clip
triangles taller than 2047 lines.

### X4. Lighting
Fixed-function subset: one infinite directional light + ambient
(`l10gl_light_dir`, `l10gl_light_color`, `l10gl_material`), evaluated
per-vertex from `l10gl_normal3f` in eye space, exactly replacing the math
in `demos/cube.c:103`. Multiple lights and specular are stretch goals — do
not block on them.

### X5. Perspective-correct texture W
The pipeline computes `w = 1/z_eye` per vertex and feeds it through
(`struct l10gl_vertex.w` already exists) so ViRGE perspective-correct
texturing (`virge.c:927`) finally gets real W values instead of the demo's
hand-rolled ones.

### X6. Port the demos
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
Scanout retarget = program the display start address over the CRTC
(CR31[5:4]/CRTC start-address low/high plus extended CR69 on later
variants — gate on device ID, document register choice from DB019-B §18).
Flip at vsync using the existing `virge_wait_vsync`.

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

Scope, per DB019-B:
- A small **fixed mode table** (640×480, 800×600, 1024×768 at 60/75Hz,
  16bpp) using canonical VESA timings — no general mode timing calculator.
- Memory/dot clock PLL programming (SR12/SR13, plus the SR15 load
  sequence).
- Standard VGA CRTC timing registers plus the S3 extended overflow bits
  (CR5D/CR5E), screen offset/stride (CR13 + CR51 extension), and the
  enhanced-mode bits already partially handled in bring-up (CR31/CR3A/
  CR58 linear-window size).
- Scanout pixel format selection (this resolves V8 definitively: we choose
  555 or 565 rather than inheriting it) — on DX/GX variants via the
  streams processor, on the base 325 via CR67.
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
   idle (`virge_wait_engine`) before touching registers. DB019-B §22
   documents command-FIFO free-slot status in SUBSYS_STATUS; wait for
   enough free slots instead of idle so triangle setup overlaps
   rasterization. Keep full-idle waits before CPU framebuffer access and
   buffer flips.
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
- **Register facts need citations.** When adding register programming,
  cite the datasheet section (ViRGE: DB019-B; Matrox: MGA-1064SG spec) in
  the comment, as the existing headers do.
- **Don't break the bring-up path.** `vga_ensure_new_mmio()` and the S3d
  enable sequence in `virge_init` encode hard-won hardware knowledge;
  changes there require explicit human sign-off.

## Suggested execution order and dependencies

```
Phase 0 (V1..V8)  — independent of each other; V7/V8 need HW sign-off
Phase 1 (F1→F2, F3, F4) — F1 before F2; F3 independent; F4 last
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
