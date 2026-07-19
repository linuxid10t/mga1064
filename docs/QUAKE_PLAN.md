# Quake compatibility plan

This document is the execution plan for Phase 7 of `PLAN.md`: making GLQuake
run on L10GL. It replaces the previous ordering in which full OpenGL 1.1
compliance was Phase 7; that work is now Phase 8 (`docs/GL11_PLAN.md`) and
builds directly on what lands here.

The priority is **up and running first**: a real GLQuake binary booting,
rendering, and completing `timedemo demo1` on the swrast backend is the
milestone that everything in Stage 1 and Stage 2 serves. ViRGE hardware
support (Stage 3) comes after the game is proven correct in software, exactly
as the rest of the project validates frontend work against swrast before
touching silicon.

## Why Quake is the right Phase 7 target

GLQuake (id Software, 1997) is the canonical era-correct workload for this
hardware class, and it is a *small* GL client: pure GL 1.0-style immediate
mode, no vertex arrays, no display lists, no stencil, no GLU dependency
(its `MYgluPerspective` calls `glFrustum` directly). Most of its state
surface already exists in `src/l10gl_gl.c` after Phase 4:

- immediate mode with triangles/strips/fans/quads, `glVertex2f/3f/3fv`,
  `glTexCoord2f`, `glColor3f/4f/4fv`;
- both matrix stacks, `glFrustum`/`glOrtho`/`glViewport`/`glDepthRange`
  (so `gl_ztrick`'s alternating `GL_LEQUAL`/`GL_GEQUAL` scheme and the
  weapon depth-range hack work — all eight depth functions are mapped);
- `glCullFace(GL_FRONT)`, depth test/mask, `glShadeModel`;
- texture objects, RGB/RGBA `glTexImage2D` (Quake expands its 8-bit
  palette to RGBA itself, and its `gl_solid_format`/`gl_alpha_format`
  integer internal formats 3 and 4 are already accepted);
- the full 1.1 blend-factor set on swrast, including the
  `glBlendFunc(GL_ZERO, GL_SRC_COLOR)` multiply that the lightmap pass
  needs (`src/backends/swrast/swrast.c`);
- fullscreen context ownership, double-buffered presentation, and console
  restore — the exact services GLQuake otherwise gets from GLX/wgl.

What is missing is a bounded list of entry points and four real semantic
gaps (rectangular textures, alpha test, `glTexSubImage2D`, texture
lifetime). Phase 7 closes exactly that list and nothing more; everything
else GL 1.1 requires stays in Phase 8.

## Scope, non-goals, and the license boundary

- **Target application:** the original id Software GLQuake source release
  (GPL-2.0), built for Linux, with its video/input layer ported to
  L10GL's `l10glCreateContext`/`l10glSwapBuffers` calls — the same
  substitution `gears` proved in Phase 4.
- **License boundary:** GLQuake's source is GPL-2.0 and this project is
  MIT. Follow the established 86Box rule (`PLAN.md` F4): run it as a
  separate program, never copy its code into this repository. The port
  (a fork of id's tree with a `vid_l10gl.c`/`in_l10gl.c`) lives in a
  **separate repository**; this repository gains only GL API features,
  tests, and documentation. Nothing in `libl10gl.a` may include or link
  GPL code.
- **Test data:** the Quake shareware episode (`pak0.pak`) is
  redistributable in unmodified form and drives all automated testing
  (`timedemo demo1`). Do not commit game data to this repository; the
  test rig fetches it.
- **Non-goals for Phase 7:** sound (Quake runs fine with `-nosound`),
  networking beyond what compiles by default, multitexture
  (`GL_SGIS/ARB_multitexture` — GLQuake probes and falls back cleanly),
  screenshots on hardware (`glReadPixels` from VRAM is a known-unreliable
  path, Phase 8 C7), and any GL feature Quake does not call.

## GL usage manifest (Q0 — authoritative)

Audited against the id Software GLQuake GPL-2.0 source release
(`id-Software/Quake`, commit `bf4ac42`), `WinQuake/gl_*.c` GL renderer and
`gl_vidlinuxglx.c` platform layer — the tree the Q9 port forks. The audit
is read-only and out-of-tree (the GPL boundary forbids the source from
entering this repository). The figures below come from a comment-stripped
sweep of *direct* core GL calls; the manifest that drives the automated
gate is `tests/quake_gl_symbols.def`, exercised by
`tests/test_quake_linkgate.c`.

**Link posture.** The Linux GLX build calls core GL directly (no `qgl`
function-pointer indirection for core functions), so every entry point in
the table must be present in `libl10gl.a` for the port to link. Extensions
are runtime-probed and skipped when `glGetString(GL_EXTENSIONS)` is empty,
so they are **not** link dependencies:

- `glMTexCoord2fSGIS` / `glSelectTextureSGIS` — SGIS_multitexture,
  `dlsym`'d into `qgl*` pointers in `gl_vidlinuxglx.c:554-555`; clean
  fallback to single-texture.
- `glColorTableEXT` / `gl3DfxSetPaletteEXT` — EXT shared texture palette,
  `dlsym`'d, `is8bit = false` path (`gl_vidlinuxglx.c:106`).
- `gl{Array,Color,TexCoord,Vertex,Texture}PointerEXT` — EXT_vertex_array;
  Quake-internal `PROC` pointers (`gl_vidnt.c`, Windows only; the Linux
  port defines/stubs them itself).
- `glBindTextureEXT` — Windows-only `wglGetProcAddress` probe.
- `glFog{,i,fv,f}` — **dead code**: the entire fog block is a commented-out
  "Experimental silly looking fog" section in `gl_rmain.c` (≈line 1131).

**Entry points.** 47 distinct direct GL core calls; 35 are already in
`libl10gl.a`, 12 are the Q0–Q6 gap (the gate reports exactly this set).
The 12 missing symbols, with the subsystem that uses them:

| Symbol | Used for | Source | Owner |
|---|---|---|---|
| `glGetString` | init banner, `GL_EXTENSIONS`/`GL_VENDOR`/`GL_RENDERER`/`GL_VERSION` probe | `gl_rmisc.c`, `gl_rmain.c` | Q1 |
| `glTexParameterf` | min/mag filter + wrap (`gl_texturemode`) | `gl_rsurf.c`, `gl_draw.c` (38 calls) | Q1 |
| `glColor3ubv` | particle color from `d_8to24table` | `r_part.c:720` | Q1 |
| `glPolygonMode` | `GL_FILL` at setup | `gl_rmain.c` | Q1 |
| `glDrawBuffer` | `GL_BACK`/`GL_FRONT` | `gl_rmisc.c`, `gl_rmain.c` | Q1 |
| `glReadBuffer` | `GL_FRONT`/`GL_BACK` around screenshot/envmap | `gl_rmisc.c:112,162` | Q1 |
| `glReadPixels` | screenshots, envmap capture | `gl_rmain.c` | Q1 stub (Phase 8 C7 real) |
| `glHint` | `GL_PERSPECTIVE_CORRECTION_HINT` (`gl_affinemodels`) | `gl_rmain.c:567,575` | Q1 |
| `glGetFloatv` | capture `GL_MODELVIEW_MATRIX` into `r_world_matrix` | `gl_rmain.c:927` | Q1 |
| `glTexSubImage2D` | dynamic lightmap updates | `gl_rsurf.c:444,546,711` | Q4 |
| `glAlphaFunc` | `GL_GREATER 0.666` for console text/HUD/sprites/fence | `gl_draw.c`, `gl_rsurf.c` | Q5 |
| `glTexEnvf` | `GL_MODULATE`/`GL_REPLACE` for alias-model passes | `gl_rmain.c:567` | Q6 |

The other 35 (`glBegin`/`glEnd`, the `glVertex*`/`glTexCoord*`/`glColor*`
immediate family, the matrix stack, `glFrustum`/`glOrtho`/`glViewport`/
`glDepthRange`, clear/enable/disable/cull/depth/blend/shade, `glTexImage2D`,
`glBindTexture`, `glFlush`/`glFinish`) are present from Phase 4. Note
GLQuake **self-manages texture-object names** — it never calls
`glGenTextures`/`glDeleteTextures`/`glIsTexture` (it `glBindTexture`s its
own integer names); those entry points remain for Phase 8 and other clients.

**Tokens.** 60 GLQuake-referenced tokens; 40 are already in
`include/GL/gl.h`. The 20 missing tokens land with the entry point that
uses them (defining a token makes no behavior claim and `GL_EXTENSIONS`
stays empty, so this is honest): `GL_POLYGON` (Q2), `GL_ALPHA_TEST` (Q5),
`GL_TEXTURE_ENV`/`GL_TEXTURE_ENV_MODE`/`GL_MODULATE`/`GL_REPLACE` (Q6),
`GL_LUMINANCE`/`GL_ALPHA`/`GL_INTENSITY`/`GL_RGBA4` (Q7/Q10 formats), and
`GL_FILL`, `GL_PERSPECTIVE_CORRECTION_HINT`, `GL_FASTEST`, `GL_NICEST`,
`GL_MODELVIEW_MATRIX`, `GL_VENDOR`/`GL_RENDERER`/`GL_VERSION`/
`GL_EXTENSIONS`, `GL_FLOAT` (all Q1). `GL_DECAL` is not referenced by
Quake; it arrives with its Q6 behavior. The guarded 8-bit paletted path
also references `GL_COLOR_INDEX`/`GL_COLOR_INDEX8_EXT`; the port defines
the latter locally and Q1 adds `GL_COLOR_INDEX` so the (runtime-skipped)
path compiles.

**Semantic gaps:**

1. **Rectangular textures.** `glTexImage2D` currently rejects
   `width != height` for every backend (`src/l10gl_gl.c`). Quake wall
   textures and skins are routinely non-square powers of two (128×32,
   64×128, the 512×256 console background); even booting to the console
   needs this. Lightmap blocks are 128×128 and unaffected.
2. **Alpha test.** No `GL_ALPHA_TEST` stage exists anywhere. Quake enables
   `glAlphaFunc(GL_GREATER, 0.666)` for console text, HUD pics, sprites,
   and fence textures; without it they render as opaque quads.
3. **`glTexSubImage2D`.** Dynamic lightmaps are updated through subimage
   uploads every frame the world lighting changes.
4. **Texture lifetime.** `glDeleteTextures` frees GL names only; backend
   storage lives until context teardown (swrast allocation list, ViRGE
   VRAM bump allocator). Quake creates a fresh texture set on every level
   load, so a level change or two exhausts memory.

## Stage 1 — API surface: link and boot on swrast

### Q0. GLQuake GL-usage manifest and link gate

Audit the GLQuake source (the id GPL release) and produce a checked-in
manifest — a table in this document plus a machine-readable list under
`tests/` — of **every** GL entry point, token, primitive mode, pixel
format, and internal format the engine references, annotated with the
subsystem that uses it (2D, world, lightmaps, alias models, particles,
sky/water, screenshots). This is the Quake-scoped analogue of Phase 8's
C0 and permanently replaces the informal inventory above.

Add a link-gate test: a translation unit that references exactly the
manifest's symbols must link against `libl10gl.a`. Until Q1 lands the
test documents the failures; afterward it must stay green.

*Acceptance:* the manifest is complete against the actual source (cite
file/function per row); `make check` reports the current link-gate status;
no row is marked "unknown".

### Q1. Trivial entry points, aliases, and tokens

Implement every manifest row that is a conversion or an honest stub:
`glGetString` (real vendor/renderer/version strings; the version string
must keep reporting the honest pre-1.1 tier per the Phase 8 rule; an
empty-but-valid `GL_EXTENSIONS` so GLQuake's multitexture probe cleanly
falls back), `glTexParameterf` delegating to the existing `glTexParameteri`
paths, the byte-color `glColor*` variants Q0 found, `glPolygonMode`
(accept `GL_FILL`, record `GL_INVALID_ENUM`-free state, error on modes the
project does not draw), `glDrawBuffer` (accept `GL_BACK`; `GL_FRONT`
returns an error until a use case exists), and `glReadPixels` as a
documented stub recording `GL_INVALID_OPERATION`. Add all missing tokens
to `include/GL/gl.h`.

*Acceptance:* the Q0 link gate passes; new entry points have `test-gl`
coverage for conversion correctness and error behavior; `glGetString`
output identifies backend and honest version tier.

### Q2. `GL_POLYGON` primitive assembly

Quake draws world surfaces, water, and sky as `glBegin(GL_POLYGON)` with
convex vertex loops. Add `GL_POLYGON` to the shim's primitive map and the
streaming assembler in `src/l10gl_pipeline.c` as a fixed-origin fan
(identical decomposition to `L10GL_TRIANGLE_FAN` for convex input, which
is all GL guarantees anyway), preserving flat-shading provoking-vertex
rules and the no-allocation property. Add `GL_LINE_LOOP` at the same time
if Q0 shows any use; otherwise leave it to Phase 8 C2.

*Acceptance:* pipeline tests cover polygon assembly, winding, culling, and
near-clip interaction; a swrast capture of a textured convex polygon
matches the equivalent triangle fan byte-for-byte.

### Q3. Rectangular power-of-two textures

Relax the shim's square-only rule into a per-backend capability:

- **Common frontend:** accept `width != height` when both are powers of
  two within the backend's limit; keep rejecting non-power-of-two sizes.
- **swrast:** store and sample rectangles natively — the sampler already
  takes independent width/height, so this is mostly removing the
  restriction and testing wrap behavior on both axes.
- **ViRGE:** DB019-B §19.4 (PDF p. 251) defines square 2^s textures only.
  Represent a W×H rectangle as its bounding square with the short axis
  **tile-replicated** to fill the square (e.g. 128×32 stored as 128×128
  containing four vertical repeats). Unlike padding plus UV rescale,
  replication keeps `GL_REPEAT` exact — Quake's world UVs span many
  repeats — at a VRAM cost that Q10's format work offsets. Document the
  cost table; `GL_CLAMP` on a replicated axis is inexact and must be
  reported in the limitations doc if Q0 shows Quake needs clamped
  rectangles.

*Acceptance:* swrast renders repeated and clamped rectangular textures
correctly under new pixel tests in `make check`; the ViRGE path has a
replication unit test on the stored image plus a human hardware run of a
new `demos/` rectangle-texture proof; square-texture behavior is
unchanged byte-for-byte.

### Q4. `glTexSubImage2D`

Keep the converted ARGB8888 image of every GL texture in shim-side system
memory (the conversion buffer `glTexImage2D` currently frees), apply
subrectangle updates there, and re-upload through the existing
`tex_image_2d` backend call. This is semantically exact and fast enough
for swrast; per-frame full re-uploads on ViRGE are a known cost recorded
for Q12, not a correctness problem. Enforce the specified error behavior
(offsets/sizes within the level, format/type as in `glTexImage2D`).

*Acceptance:* tests cover interior/edge subrectangles, unpack alignment,
and error cases; a swrast scene updated via subimage matches a control
scene uploaded whole; memory accounting shows one retained CPU copy per
texture, freed with the GL name.

### Q5. Alpha test

Add `glAlphaFunc` and the `GL_ALPHA_TEST` enable with full state
(function + reference), a frontend cap bit, and a swrast fragment stage in
specification order (alpha test before depth write and blending, so
rejected fragments touch neither color nor depth). All eight compare
functions, since the plumbing is shared with depth compare.

ViRGE has no alpha-test stage in silicon. For Stage 3, map Quake's usage
(binary 0/255 texture alpha, `GL_GREATER 0.666`) onto the chip's
texture-alpha blend mode (DB019-B §15.4.8.5) with the documented
depth-write caveat, as period miniGL drivers did; record it as an
approximation in the limitations notes. Exactness on hardware is Phase 8
C6/C10 territory.

*Acceptance:* a swrast truth-table test pins every compare function and
the no-depth-write-on-reject rule; the console-font case (text over a
scene, transparent texels invisible, depth untouched) renders correctly in
a pixel test.

### Q6. Texture environment: `GL_MODULATE`, `GL_REPLACE`, `GL_DECAL`

The pipeline currently hardwires modulate (vertex color × texel) in both
swrast (`swrast.c` texture path) and ViRGE (`l10gl_virge.c`, TB=01).
Add `glTexEnvf/i` with per-context environment state:

- `GL_MODULATE`: current behavior.
- `GL_REPLACE`: fragment = texel, ignoring vertex color — the mode
  GLQuake sets for world texture passes. Frontend can implement it
  exactly by forcing white vertex color into the modulate path; ViRGE
  additionally has a native decal mode to evaluate.
- `GL_DECAL`: implement per the RGB/RGBA equations in swrast; on ViRGE
  use the native decal blend only if equation-level tests prove it
  matches, per the Phase 8 C5 rule.

`GL_BLEND`-environment and `GL_ADD` stay out of scope (Quake does not use
them; Phase 8 C5/C6).

*Acceptance:* equation tests pin all three modes for RGB and RGBA
textures on swrast; REPLACE with non-white vertex color demonstrably
ignores the color; ViRGE runs a mode-comparison demo for human sign-off.

## Stage 2 — GLQuake runs correctly on swrast

### Q7. Lightmap formats and the multiply pass

Verify end-to-end that the world lightmap path works on swrast:
`-lm_4`-style `GL_RGBA` lightmaps upload through the existing path, and
the `glBlendFunc(GL_ZERO, GL_SRC_COLOR)` multiply pass produces a lit
world. Add `GL_LUMINANCE` (and, if Q0 confirms use, `GL_ALPHA`/
`GL_INTENSITY`) as an accepted `glTexImage2D` format/internal format with
1-byte unpack expanded to ARGB8888, so GLQuake's *default* configuration
(`gl_lightmap_format` luminance with `GL_ZERO, GL_ONE_MINUS_SRC_COLOR`)
also works rather than requiring a command-line switch.

*Acceptance:* a synthetic two-pass test (checker texture × gradient
lightmap) matches an analytically computed image on swrast for both the
RGBA and luminance formats; dynamic light updates through Q4 visibly
modulate the result.

### Q8. Texture lifetime and delete semantics

Make `glDeleteTextures` actually release storage so per-level texture
churn survives:

- **shim:** free the retained CPU copy (Q4) with the name.
- **swrast:** free the backend allocation on delete (it owns a private
  allocation list; add removal).
- **ViRGE:** replace the bump allocator's leak-until-teardown behavior
  with a free-list allocator over the texture heap region (first-fit with
  coalescing is sufficient at this scale), preserving the existing
  alignment rules. Deleting must make VRAM reusable; fragmentation limits
  are acceptable and documented.

*Acceptance:* a create/delete/create stress test at swrast and (human
sign-off) ViRGE shows stable memory across simulated level reloads; the
existing bump-allocation demos still pass; OOM still reports cleanly.

### Q9. The GLQuake port and the swrast "up and running" gate

In the separate GPL repository: port GLQuake's platform layer to L10GL —
`vid_l10gl.c` using `l10glCreateContext`/`l10glSwapBuffers`/`glFinish`
(the Phase 4 `gears` substitution, plus honoring the actual geometry the
context reports), and `in_l10gl.c` reading raw keyboard from the owning VT
and mouse from evdev, following the console-ownership discipline
`src/console.c` established (the game must never fight P2 for the VT).
Build with `-nosound` first. Document the exact build/run steps in that
repository and link them from here.

This repository gains the automated gate: a scripted run (fetch shareware
data, build the port, run `L10GL_BACKEND=swrast` offscreen with
`L10GL_FRAMES`-bounded capture) that starts the game, plays
`timedemo demo1` to completion, and dumps frames.

*Acceptance — the Phase 7 headline milestone:* `timedemo demo1` completes
on swrast without GL errors on the console; captured frames show a
textured, lightmapped world, sky and water, alias models, particles, and
readable HUD/console text; the reported timedemo frame total matches the
demo's canonical frame count.

## Stage 3 — GLQuake on the ViRGE

Stage 3 begins only after the Q9 gate is green. Every item here follows
the established hardware rules: one behavioral change per commit,
`tools/l10gl-run`, human sign-off, full console recovery.

### Q10. VRAM budget: 16-bit texture uploads and level-fit policy

At 640×480 RGB555 double-buffered with Z, roughly 2.2MB of the 4MB card
remains for textures — before Q3's replication overhead. Add
ARGB1555/ARGB4444 upload paths in the shim (the backend formats already
exist in `l10gl_virge.c`; the shim currently converts everything to
ARGB8888) selected by internal format, halving texture VRAM. Publish a
sizing note: recommended `gl_max_size 256`, expected budget per shareware
level, and what OOM looks like when a level exceeds it (clean error, not
corruption). Q8's allocator makes level transitions survivable.

*Acceptance:* format-conversion tests pin 1555/4444 packing; a VRAM
accounting test walks a recorded shareware level's texture set and proves
it fits the budget; human verifies texture quality on hardware.

### Q11. First hardware run: fullbright world

Run the ported GLQuake on the ViRGE/DX with lightmaps disabled
(`r_fullbright 1`, dynamic lighting off) — geometry, textures, alpha-test
approximation (Q5), sky, water, models, HUD, at 640×480@60 via the proven
P6 native modeset. This isolates rasterization and texture correctness
from the lightmap-blending question.

*Acceptance:* the shareware start map renders recognizably and navigably;
no register-level hangs across a full `timedemo demo1`; Ctrl-C and normal
exit both restore the console; FPS recorded as the hardware baseline.

### Q12. ViRGE lightmap strategy

The multiply blend (`GL_ZERO, GL_SRC_COLOR`) does not exist in ViRGE
silicon — blending is fixed-function source-alpha only (`PLAN.md`
capability table; DB019-B §15.4.8.5). Decide the lit-world approach with
a spike per option, in this order of preference:

1. **CPU lightmap compositing** (the vQuake approach): pre-multiply
   lightmaps into the affected surface textures on the CPU and upload the
   composite — exact output, costs VRAM (a surface cache) and upload
   bandwidth on dynamic light changes; meshes well with Q4's retained CPU
   copies.
2. **Vertex lighting approximation:** sample the lightmap at polygon
   vertices into the Gouraud path — cheap, era-authentic look, visibly
   coarser.
3. **Defer to Phase 8 C10** hybrid software fallback — exact but far off;
   only if 1 and 2 both fail on quality or budget.

The decision, measurements, and rejected options get recorded here the
way Phase 6 recorded the autoexecute and triangle-reuse rejections.

*Acceptance:* the chosen path renders a lit E1M1 on hardware, visually
compared against the swrast reference; dynamic lights (rockets, muzzle
flashes) either work or are cleanly disabled by default with the cvar
documented.

### Q13. Phase acceptance: playable Quake on the ViRGE

Close Phase 7 with the end-to-end gate on the target machine.

*Acceptance:* from a fresh boot, `l10gl-run`-launched GLQuake at
640×480@60 plays the shareware E1M1 start-to-exit with lighting per Q12,
level transition succeeds (Q8/Q10), exit and Ctrl-C restore the console,
and `timedemo demo1` FPS is recorded in this document. Playability, not
frame rate, is the bar — this chip was never fast at GLQuake and single-digit
FPS does not block acceptance. Performance follow-ups go to the Phase 6
methodology with before/after numbers.

## Execution order

```text
Q0 -> Q1 -> Q2 ------------------+
       |                         |
       +--> Q3 -> Q4 -> Q7 ------+--> Q9 (swrast gate)
       |          |              |
       +--> Q5 ---+---> Q8 ------+
       +--> Q6 ------------------+

Q9 -> Q10 -> Q11 -> Q12 -> Q13   (hardware stage, strictly after Q9)
```

Q1 through Q6 are independently committable against `make check`; Q9 is
the integration point and the "up and running" milestone the phase is
named for. Nothing in Stage 3 starts before Q9 passes.

## Relationship to Phase 8

Every Q-item is a scoped-down slice of a Phase 8 C-item and must be built
so Phase 8 extends rather than replaces it: Q1 feeds C1 (queries/ABI), Q2
feeds C2 (primitives), Q3/Q4/Q7/Q8/Q10 feed C5 (complete texture
behavior), Q5/Q6 feed C6 (per-fragment state), and Q12's findings feed
C10 (hybrid fallback). The honest-version rule is unchanged throughout:
`glGetString(GL_VERSION)` keeps reporting the pre-1.1 tier until Phase 8's
gates pass — running Quake does not make the driver OpenGL 1.1.
