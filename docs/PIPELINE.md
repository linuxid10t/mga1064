# Immediate-mode geometry pipeline

X2 and X3 add a model-space submission and clipping path above the existing
screen-space drawing API. It uses X1 matrix and viewport state, but does not
alter or replace
`l10gl_draw_triangle()`, `l10gl_draw_textured_triangle()`, or
`l10gl_draw_line()`.

## Submission

Begin one supported primitive, set current attributes as needed, submit
vertices, and end it:

```c
l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
l10gl_load_identity(ctx);

l10gl_begin(ctx, L10GL_TRIANGLES);
l10gl_color4f(ctx, 1, 0, 0, 1);
l10gl_vertex3f(ctx, -1, -1, 0);
l10gl_color4f(ctx, 0, 1, 0, 1);
l10gl_vertex3f(ctx,  1, -1, 0);
l10gl_color4f(ctx, 0, 0, 1, 1);
l10gl_vertex3f(ctx,  0,  1, 0);
l10gl_end(ctx);
```

Supported primitives are:

- `L10GL_TRIANGLES`
- `L10GL_TRIANGLE_STRIP`
- `L10GL_TRIANGLE_FAN`
- `L10GL_LINES`
- `L10GL_LINE_STRIP`

`L10GL_POINTS` remains unsupported and `l10gl_begin()` returns `-ENOTSUP`.
Nested begin calls return `-EBUSY`; vertex/end calls outside a block return
`-EPERM`. Incomplete final primitives are ignored. These errno-style results
make native L10GL misuse testable; the eventual GL shim can translate or ignore
them according to GL error semantics.

Primitive assembly is streaming and allocation-free. Triangle strips reverse
the first two source vertices on every odd triangle so all faces retain
consistent winding. Fans retain vertex zero as their origin. Independent lines
consume pairs, while line strips retain only the previous vertex.

Do not change matrices or viewport state between `l10gl_begin()` and
`l10gl_end()`. Legacy OpenGL forbids those operations inside a begin/end block;
the future compatibility shim will enforce that rule.

## Current attributes

`l10gl_color4f()`, `l10gl_normal3f()`, and `l10gl_texcoord2f()` update current
state. Every `l10gl_vertex3f()` captures a copy, so later attribute changes do
not affect earlier vertices. Defaults are opaque white, normal `(0,0,1)`, and
texture coordinate `(0,0)`.

Normals are retained in the captured frontend vertex for X4 lighting but are
not evaluated yet. Color, alpha, normal, and UV are interpolated when X3
creates near-plane intersection vertices.

Binding a non-NULL texture with `l10gl_bind_texture()` selects textured
triangle dispatch. Binding NULL returns to Gouraud triangle dispatch. A backend
without a textured draw hook still receives the frontend's established plain
triangle fallback.

## Transform and culling

For each complete primitive, X2 computes:

```text
clip = projection * modelview * object
ndc = clip.xyz / clip.w
screen = viewport_and_depth_range(ndc)
```

The screen result uses top-left framebuffer Y and is passed to the unchanged
backend vtable. Back-face decisions are made before that Y flip, in NDC, where
counter-clockwise is front-facing as in OpenGL.

Configure culling with:

- `L10GL_CULL_NONE` (default)
- `L10GL_CULL_FRONT`
- `L10GL_CULL_BACK`

Degenerate zero-area triangles are discarded. Lines are not culled.

## Clipping and current limits

X3 clips assembled triangles against the OpenGL homogeneous near plane,
`Z + W >= 0`, before perspective division and culling. Sutherland-Hodgman
clipping produces zero, one, or two triangles from each input triangle. New
vertices interpolate clip coordinates, color, alpha, normal, and UV; a vertex
within a small relative epsilon of the plane is snapped onto it to avoid a
numerical sliver. A clipped quad is triangulated as a fan, preserving its
shared vertices.

X/Y are left to the backend's existing clip rectangle. After viewport
transformation, triangles spanning more than 2047 scanlines are rejected
before dispatch because the ViRGE command field is only 11 bits.

Far-plane clipping is not implemented yet: a triangle crossing `Z - W = 0`
is rejected whole. Lines are not clipped and likewise use conservative
whole-segment depth rejection. These restrictions keep invalid coordinates
away from vintage hardware while leaving the completed triangle near-plane
path useful.

X5 will derive reciprocal eye depth for perspective-correct texture mapping.
Until then, immediate-mode vertices emit `w = 1.0`, so their textures are
affine. Existing screen-space textured drawing remains unchanged and can still
supply explicit W.

## Validation

`make check` runs `test-pipeline` against a capture backend. It verifies exact
screen coordinates and attributes, MODELVIEW connection, triangle grouping,
strip alternation, fan origin, line pairing, incomplete primitives, bound
texture selection, front/back culling, near-plane split/interpolation and
boundary cases, conservative far/line rejection, and the 2047-scanline guard.
The separate swrast suite validates the rasterizer receiving those calls.
