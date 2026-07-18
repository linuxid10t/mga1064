# Transform conventions and X1 API

X1 adds hardware-independent matrix and viewport state to every `l10gl_ctx`.
It is the mathematical foundation for the Phase 2 primitive pipeline and the
eventual OpenGL 1.1 compatibility shim.

X1 does not change the existing drawing contract: `l10gl_draw_triangle()` and
`l10gl_draw_line()` still accept screen-space vertices. X2 will consume the
state described here and emit those existing primitives after transformation.

## Matrix convention

Matrices contain 16 `float` values in OpenGL-compatible column-major order and
operate on column vectors. All constructors post-multiply the selected matrix:

```text
current = current * new_transform
clip = projection * modelview * object
```

Consequently, this sequence transforms a point by scale first, then by
translation:

```c
l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
l10gl_load_identity(ctx);
l10gl_translatef(ctx, 1.0f, 2.0f, 3.0f);
l10gl_scalef(ctx, 2.0f, 2.0f, 2.0f);
```

`l10gl_load_matrixf()` and `l10gl_mult_matrixf()` accept column-major arrays.
`l10gl_get_matrix()` copies out either current stack top. The low-level
`l10gl_transform_vec4()` helper permits its input and output arrays to alias.

## Matrix stacks

Select a stack with `l10gl_matrix_mode()`:

- `L10GL_MATRIX_MODELVIEW`: 32 entries
- `L10GL_MATRIX_PROJECTION`: 4 entries

Both begin at an identity matrix when `l10gl_create()` succeeds, with
MODELVIEW selected. `l10gl_push_matrix()` duplicates the current top and
`l10gl_pop_matrix()` restores the previous entry. They return `-EOVERFLOW` and
`-ERANGE` at their respective bounds and leave state unchanged. Invalid matrix
targets and projection parameters return `-EINVAL`.

Available constructors are:

- `l10gl_translatef`, `l10gl_rotatef`, and `l10gl_scalef`
- `l10gl_frustum` and `l10gl_perspective`
- `l10gl_ortho`

Rotation angles are degrees. Rotation axes are normalized internally; a zero
or non-finite axis is rejected.

## Viewport and depth

The default viewport is `(0, 0, ctx->width, ctx->height)` using the actual
raster reported by the backend, not merely the requested context size. The
default depth range is `[0, 1]`.

`l10gl_viewport()` follows OpenGL's lower-left origin. Hardware backends use
top-left framebuffer coordinates, so `l10gl_ndc_to_window()` performs the Y
conversion using `ctx->height`:

```text
x_window = viewport_x + (x_ndc + 1) * viewport_width / 2
y_gl     = viewport_y + (y_ndc + 1) * viewport_height / 2
y_screen = framebuffer_height - y_gl
z_window = near + (z_ndc + 1) * (far - near) / 2
```

Viewport width and height may be zero but not negative. `l10gl_depth_range()`
clamps each endpoint independently to `[0, 1]`, matching legacy OpenGL; a
reversed depth range is valid.

## Pipeline helpers

`l10gl_object_to_clip()` applies the current MODELVIEW and PROJECTION matrices
without dividing by clip W. Keeping clip coordinates intact is important: X3
will clip against the near plane before the perspective divide. After clipping
and division, X2 can pass NDC coordinates to `l10gl_ndc_to_window()`.

`make check` runs `test-xform`, which verifies matrix ordering, load/multiply,
axis normalization, both stack limits, perspective and orthographic results,
invalid-input preservation, viewport Y conversion, depth mapping, and aliasing.
