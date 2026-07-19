# Maximum OpenGL 1.1 compatibility plan

This document is the execution plan for taking L10GL from its current small
OpenGL-style shim to the fullest correct OpenGL 1.1 implementation practical
on the S3 ViRGE/DX. It supplements `PLAN.md`; the numbered work below is
Phase 7 of that roadmap.

The target is **correctness first, acceleration second**. A command must not
silently degrade merely because the ViRGE cannot express it. Every feature
will be classified as one of:

1. exact common frontend behavior;
2. exact native ViRGE acceleration;
3. exact software fallback with a defined synchronization boundary; or
4. unavailable because the selected context legitimately has no such buffer.

The authoritative API contract is the OpenGL 1.1 specification, Version 1.1,
4 March 1997:
<https://registry.khronos.org/OpenGL/specs/gl/glspec11.pdf>.
The 336-command inventory is the union of the `GL_VERSION_1_0` and
`GL_VERSION_1_1` requirements in Khronos's official API registry:
<https://github.com/KhronosGroup/OpenGL-Registry/blob/main/xml/gl.xml>.
The hardware contract is S3 DB019-B in `docs/datasheets/`, especially
sections 15.4 and 19 (absolute PDF pages 127-134 and 250-274). Register code
must continue to cite the relevant databook section in comments.

## What “as full as the hardware can handle” means

L10GL will offer an RGBA, monoscopic, fullscreen console context. GLX, WGL,
window-system visuals, direct rendering infrastructure, and X11 integration
remain outside the project. They are not OpenGL core rendering commands.

OpenGL 1.1 explicitly permits a context without right, auxiliary, depth,
stencil, or accumulation buffers. L10GL may therefore report:

- no stereo or auxiliary color buffers;
- zero stencil bits and zero accumulation-channel bits on the native ViRGE
  context;
- the existing 16-bit depth buffer where the selected VRAM layout fits; and
- the actual RGB channel widths of the selected scanout mode.

The stencil and accumulation entry points still have to exist, maintain the
specified state, report errors where the specification requires them, and
make no framebuffer changes where a missing buffer makes the operation a
no-op. Absence of those buffers is not, by itself, a reason to misrepresent
the rest of the context.

There will be three measurable targets:

| Target | Meaning | Release claim |
|---|---|---|
| API complete | All core 1.1 command symbols, types, tokens, defaults, errors, and queries are present | “OpenGL 1.1 API surface” |
| Software complete | The swrast RGBA context implements the applicable core semantics and is the pixel oracle | “OpenGL 1.1-compatible software renderer” after validation |
| ViRGE maximum | Every exactly representable operation is accelerated; the rest uses an exact fallback or an honestly absent buffer | “OpenGL 1.1-compatible ViRGE renderer” only after the final gate |

Until the matching acceptance gate passes, `glGetString(GL_VERSION)` must not
claim OpenGL 1.1. Passing unit tests is not permission to call the driver
officially conformant; use that word only after a recognized conformance suite
has also passed.

## Current baseline

As of 2026-07-18, `src/l10gl_gl.c` exports 48 of the 336 OpenGL 1.1 command
names (14.3% symbol coverage). The useful rendering coverage is higher than
that number because the missing scalar/vector/type variants are mostly thin
conversions, but semantic coverage has not yet been inventoried command by
command. Phase 7 item C0 establishes the real baseline and replaces informal
percentages with auditable counts.

Already working through the GL shim on swrast and ViRGE:

- immediate triangles, triangle strips/fans, quads, quad strips, lines, and
  line strips;
- MODELVIEW and PROJECTION transforms, viewport/depth range, near clipping,
  culling, flat/smooth shading, depth testing, and the supported blend path;
- one directional light with ambient/diffuse material;
- texture objects with square power-of-two level-zero RGB/RGBA uploads,
  nearest/linear filtering, and repeat/clamp selection; and
- clear, finish, double-buffered presentation, errors, and L10GL-owned
  fullscreen context lifetime.

Important current limits include only near-plane clipping, one light, no
state-query surface, no vertex arrays, no display lists, no pixel/raster
commands, no evaluators, no selection/feedback, and only a fraction of the
specified texture and per-fragment state.

## Hardware mapping

DB019-B supports the following exact native building blocks:

- Gouraud and textured lines/triangles, a 16-bit Z buffer, all eight depth
  comparisons, and depth-write control;
- nearest, bilinear, all four mipmap filter combinations, perspective texture
  interpolation, repeat, a border color, and textures up to 512x512;
- decal, modulate, and the documented “complex reflection” texture coloring
  path (which must not be assumed equivalent to a GL texture-environment mode
  without equation-level tests);
- dithering and alpha blending using source alpha or texture alpha against
  inverse alpha; and
- alpha-driven fog for supported textured operations, with the documented
  restriction that source-alpha blending and fog cannot be active together.

The 3D path does not provide general blend factors, stencil, accumulation,
general per-fragment logical operations, or a general alpha-test stage. Those
features must use swrast/fallback behavior or an honestly absent buffer.
Frontend work such as transforms, clipping, lighting, texture-coordinate
generation, primitive assembly, selection, feedback, and evaluators does not
require special hardware and can feed the existing triangle/line interface.

The 4MB ViRGE/DX is also a hard resource boundary. Every native allocation
plan must account for front/back color pages, the depth page, textures, and
alignment before accepting an image or enabling a mode. `GL_MAX_TEXTURE_SIZE`
will be 512 on this backend even if a particular collection of live textures
cannot all be resident simultaneously.

## Phase 7 work items

Each item is one or more small, bisectable commits. A slice is complete only
when its automated tests pass under swrast and its hardware-facing portion
has a written ViRGE acceptance run. New API behavior belongs in the common
frontend unless it truly programs a backend-specific feature.

### C0. Normative coverage manifest and test skeleton

Create a generated or checked-in manifest of all 336 OpenGL 1.1 core command
names. Track each command as `exact`, `restricted`, `absent-buffer`,
`software`, `planned`, or `unsupported`, with a test identifier and source
file. Track token/header coverage separately from exported symbols.

Add tests that fail when:

- a required symbol is missing from `libl10gl.a`;
- the public header lacks a required 1.1 type, token, or declaration;
- a manifest row has no owner or no intended test; or
- the reported version overstates the accepted implementation tier.

Acceptance: the current 48/336 symbol baseline and every existing semantic
test are represented; `make check` prints a stable coverage summary.

### C1. Context state, legality, errors, and queries

Build a central GL state record with specification defaults and begin/end
legality. Implement the complete query family (`glGetBooleanv`,
`glGetIntegerv`, `glGetFloatv`, `glGetDoublev`, `glGetPointerv`,
`glIsEnabled`, and `glGetString`) plus all inexpensive scalar/vector/type
aliases for existing commands.

Add attribute and client-attribute stacks, current-value defaults, render
mode, color/depth write masks, front/back draw/read selection, hints, and
honest framebuffer capability values. Do not expose a false hardware cap to
make a query look more complete.

Acceptance: table-driven tests pin every default, type conversion, stack
limit, first-error rule, and forbidden begin/end call. A small program using
only standard GL queries can identify the renderer without L10GL internals.

### C2. Complete primitive assembly and homogeneous clipping

Add `GL_POINTS`, `GL_LINE_LOOP`, and `GL_POLYGON`, all rectangle entry points,
edge flags, front-face selection, polygon mode, point/line size, line stipple,
polygon stipple, polygon offset, and the point/line/polygon smoothing enable
state. Exact antialiasing that the native rasterizer cannot reproduce belongs
in swrast.

Replace near-only clipping with all six canonical homogeneous clip planes,
then add the six user clip planes. Clip points, lines, and polygons while
interpolating every active vertex attribute. Lower points and wide/stippled
lines to small triangles or short backend lines where that is exact; use
swrast where hardware raster rules cannot match.

Acceptance: deterministic unit cases cover every clip plane and primitive,
including flat-shading provoking vertices, shared edges, negative W, and
degenerate geometry. Swrast images establish the reference; ViRGE runs must
show no off-screen register submissions or edge corruption.

### C3. OpenGL 1.1 vertex arrays

Implement all six client arrays, client-state enable/disable, pointer calls,
`glArrayElement`, `glDrawArrays`, `glDrawElements`, and
`glInterleavedArrays`, including the allowed component types, strides, index
types, current-value side effects, and pointer queries.

The first implementation should normalize array elements into the existing
immediate primitive pipeline. It must not introduce a second transform or
rasterization path. Optimize validated hot cases only after equivalence tests
pass.

Acceptance: immediate and array submissions of the same scene produce
identical captured vertices and swrast pixels for every primitive mode. Gears
gets an optional array-driven path for the real-hardware gate.

### C4. Full fixed-function transform, lighting, and materials

Add the texture matrix stack and double-precision matrix entry points. Expand
lighting to the OpenGL 1.1 minimums and semantics: eight lights; ambient,
diffuse, specular, and emission terms; shininess; positional and directional
lights; attenuation; spotlights; global ambient; local-viewer and two-sided
models; separate front/back materials; color material; and correct normal
matrix handling.

Implement object-linear, eye-linear, and sphere-map texture-coordinate
generation. All of this is frontend math; resulting colors and coordinates
continue through the proven ViRGE Gouraud/textured triangle path.

Acceptance: analytic tests pin transformed light positions, normal handling,
attenuation, spot cutoffs, two-sided materials, specular clamping, texture
matrices, and generated coordinates. Swrast and ViRGE render a multi-light
material test without changing backend register interfaces.

### C5. Complete OpenGL 1.1 texture object behavior

Implement 1D textures, every specified 1D/2D image declaration and query,
subimages, copy images, proxy targets, texture priorities/residency, border
color, level ranges, and complete pack/unpack state. Convert the specified
component formats and data types into backend-native storage with checked
size arithmetic.

For the ViRGE:

- lay out complete mip chains in VRAM and program the documented D gradients;
- validate all six minification filters and both magnification filters on
  silicon;
- support rectangular power-of-two images by padding to a safe square native
  allocation and applying an internal coordinate scale, if pixel tests prove
  the border and LOD behavior exact;
- map replace/decal/modulate only when their GL equations match the hardware,
  and implement the GL texture-environment blend mode exactly in frontend or
  software when the chip's complex-reflection equation differs; and
- use a frontend or software path for environment/color combinations the chip
  cannot reproduce exactly.

Acceptance: texture completeness, proxy behavior, conversions, subimages,
object lifetime, residency, and every filter/wrap combination have tests.
ViRGE mipmap and rectangular-texture probes match swrast within documented
color precision.

### C6. Per-fragment state and swrast completeness

Finish the swrast fragment pipeline in specification order: scissor, alpha
test, stencil test/operations, depth, blending, dithering, logical operations,
and color masks. Add exact fog equations, smoothing coverage, and polygon
offset. Implement `glClearStencil`, stencil state, `glClearAccum`, and
`glAccum`; allocate optional software stencil and accumulation buffers only
for context configurations that advertise them.

Map the exact ViRGE subset to hardware: depth, depth mask, scissor where the
documented clip registers prove reliable, dithering, supported alpha blend,
and supported fog. General blend factors, alpha test, logical operations,
color masks requiring read-modify-write, and unsupported fog/blend
combinations must be routed to an exact fallback rather than approximated.

Acceptance: a per-fragment truth-table suite covers every compare, stencil
operation, 1.1 blend factor with its fixed additive equation, mask, and
ordering case.
The native path has explicit capability predicates with no silent fallback to
opaque drawing.

### C7. Raster position, bitmap, and pixel transfer

Implement raster-position transformation/clipping, `glBitmap`,
`glDrawPixels`, `glReadPixels`, `glCopyPixels`, pixel zoom, pixel maps, and
the full pixel transfer/pack/unpack state. Implement depth and stencil formats
only when those buffers exist and return the specified error otherwise.

Use ViRGE 2D fills/BitBLTs and host transfers for common color cases only
after engine-idle and coherency behavior is proven. The current BAR0 mapping
cannot be assumed to provide reliable CPU reads; engine-assisted transfer or
a system-memory shadow is required before hardware `glReadPixels` can be
called correct.

Acceptance: clipped and zoomed pixel rectangles, bitmap alignment, all pack
and unpack modes, color conversion, read-buffer selection, and copy overlap
are byte-checked under swrast. A dedicated ViRGE transfer probe must round-trip
known pixels before the hardware read path is enabled.

### C8. Display lists

Implement list name allocation, compile and compile-and-execute modes,
nested calls, list base/call lists, deletion, and `glIsList`. Record a
normalized command stream rather than backend register writes so lists behave
identically on every renderer and preserve execution-time state rules.

Acceptance: tests cover nested lists, recursion limits, calls made while
compiling, pointer-data capture rules, error timing, deletion, and reuse.
The existing gears geometry can optionally compile into lists and must render
identically before any performance comparison.

### C9. Evaluators, selection, and feedback

Implement 1D/2D evaluators and maps in the frontend, tessellating into the
normal vertex path. Implement selection and feedback render modes, name-stack
operations, hit records, tokens, buffer overflow behavior, and passthrough.
These modes do not require the ViRGE rasterizer and must not touch hardware
while selecting or feeding back.

Acceptance: the examples and boundary cases in the specification have
deterministic record-level tests; switching back to render mode leaves common
state intact.

### C10. Hybrid fallback and framebuffer ownership

This item is the gate between a broad hardware subset and an honest OpenGL
1.1 ViRGE context. First, implement and test reliable ViRGE color/depth
transfer in both directions using documented engine paths. Then choose one
authoritative ownership model:

1. a synchronized system-memory shadow with explicit upload/download around
   native batches; or
2. a software-owned compatibility frame presented to ViRGE as a whole.

Do not switch renderers per draw until color and depth ownership, pending FIFO
work, texture residency, and read-after-write ordering are all defined. If a
reliable VRAM-to-host path cannot be established, expose separate native and
software context modes instead of pretending mixed fallback is safe.

Acceptance: one frame may alternate native depth-tested geometry, an
unsupported blend operation, and native geometry again and still match the
swrast oracle pixel-for-pixel within RGB555 quantization. ReadPixels and swap
must observe all prior work.

### C11. ABI completion and compatibility corpus

Export every core 1.1 symbol with C linkage and complete `include/GL/gl.h`.
Add compile tests for representative unmodified legacy programs and build a
small application corpus that exercises arrays, lists, lighting, textures,
pixel operations, selection, and feedback. Preserve the L10GL fullscreen
context calls as the only nonstandard setup/presentation boundary.

Acceptance: symbol coverage is 336/336; header/token coverage is complete;
no manifest row remains `planned`; and every non-exact row names either an
absent context buffer or a tested software path.

### C12. Final validation and version gate

Run the full unit suite under normal, ASan, and UBSan builds. Add randomized
state-sequence differential tests against swrast, image comparisons at all
supported modes, allocation-failure tests, and repeated context create/destroy
cycles. Run the compatibility corpus on swrast and real ViRGE/DX, including
console recovery after normal exit and signals.

Only after all applicable core behavior passes may the selected renderer
return an OpenGL 1.1 version string. Keep a renderer-specific limitations
document for precision, maximums, absent buffers, and tested hardware IDs.

Acceptance: the coverage report is complete, all automated configurations
pass, the hardware checklist is signed off, and no command silently changes
meaning based on the backend.

## Recommended execution order

```text
C0 -> C1 -> C2 -> C3 -> C4 -> C5
                         |      |
                         +--> C6 -> C7

C1 -> C8
C2 -> C9
C6 + C7 -> C10
C3..C10 -> C11 -> C12
```

The practical order is C0, C1, C2, C3, C4, C5, C6, C7, C8, C9, C10,
C11, C12. C8 and C9 can move earlier once C1/C2 are stable, but C10 must not
begin until the software fragment and pixel-transfer paths have trustworthy
tests.

## Hardware validation rule

Every ViRGE-facing slice must include the exact commands to build and run it,
the expected init log, the expected image or numerical result, the expected
console recovery behavior, and a rollback environment variable when register
behavior is uncertain. The standard clean gate remains:

```sh
cd /home/david/workspace/L10GL
git status --short
git pull --ff-only
git log -1 --oneline
make clean
make -j"$(nproc)"
make check
```

Hardware commands must use `tools/l10gl-run`, keep synchronized presentation
unless a raw-performance result is specifically requested, and test one
behavioral change per commit. Failed hardware experiments stay documented and
default off, following the Phase 6 autoexecute and triangle-reuse precedent.
