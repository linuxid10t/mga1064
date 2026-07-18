/*
 * l10gl.h - L10GL: Lightweight Legacy OpenGL Driver Framework
 *
 * A common interface for writing userspace OpenGL-ish drivers for vintage
 * fixed-function graphics hardware. Each hardware backend implements this
 * API; the application code (demos, games) links against l10gl and is
 * hardware-agnostic.
 *
 * The design mirrors the classic Mesa dd_function_table pattern: the
 * "device driver" is a struct of function pointers, and the higher-level
 * code calls through them without knowing what chip is underneath.
 */

#ifndef L10GL_H
#define L10GL_H

#include <stdint.h>

/* A scanout channel as reported by fbdev. Offsets count from the least
 * significant bit of a packed pixel, matching struct fb_bitfield. */
struct l10gl_color_channel {
    uint8_t offset;
    uint8_t length;
    uint8_t msb_right;
};

/* Actual scanout storage layout. bits_per_pixel is kept separate from bpp:
 * RGB555 commonly reports 15 bits while occupying two bytes per pixel. */
struct l10gl_pixel_format {
    uint8_t bits_per_pixel;
    struct l10gl_color_channel red;
    struct l10gl_color_channel green;
    struct l10gl_color_channel blue;
    struct l10gl_color_channel alpha;
};

/* ========================================================================
 * Vertex and Primitive Types
 * ======================================================================== */

struct l10gl_vertex {
    float x, y;       /* Screen coordinates (pixels) */
    float z;          /* Depth value (0.0 = near, 1.0 = far) */
    float w;          /* 1/clip_W; equals 1/positive eye depth for perspective */
    float r, g, b, a; /* Color (0.0 to 1.0) */
    float u, v;       /* Texture coordinates (0.0 to 1.0) */
};

/* Primitive types */
enum l10gl_primitive {
    L10GL_TRIANGLES,     /* Every 3 vertices = 1 triangle */
    L10GL_TRIANGLE_STRIP,/* Strip of triangles sharing edges */
    L10GL_TRIANGLE_FAN,  /* Fan of triangles sharing first vertex */
    L10GL_QUADS,         /* Every 4 vertices = 2 triangles */
    L10GL_QUAD_STRIP,    /* Paired vertices form connected quads */
    L10GL_LINES,         /* Independent line segments */
    L10GL_LINE_STRIP,    /* Connected line segments */
    L10GL_POINTS,
};

/* Matrix targets. Matrices use OpenGL-compatible column-major storage and
 * post-multiplication: current = current * supplied_transform. */
enum l10gl_matrix_mode {
    L10GL_MATRIX_MODELVIEW,
    L10GL_MATRIX_PROJECTION,
};

/* Front faces are counter-clockwise after projection, matching OpenGL. */
enum l10gl_cull_mode {
    L10GL_CULL_NONE,
    L10GL_CULL_FRONT,
    L10GL_CULL_BACK,
};

#define L10GL_MODELVIEW_STACK_DEPTH 32
#define L10GL_PROJECTION_STACK_DEPTH 4

/* Vertex captured by the immediate-mode frontend before transformation. */
struct l10gl_immediate_vertex {
    float x, y, z;
    float r, g, b, a;
    float nx, ny, nz;
    float u, v;
};

/* Depth comparison functions (match OpenGL ordering) */
enum l10gl_depth_func {
    L10GL_NEVER,
    L10GL_LESS,
    L10GL_EQUAL,
    L10GL_LEQUAL,
    L10GL_GREATER,
    L10GL_NOTEQUAL,
    L10GL_GEQUAL,
    L10GL_ALWAYS,
};

/* ========================================================================
 * Blending
 *
 * Alpha blending on this class of fixed-function hardware is limited.
 * On the primary backend (S3 ViRGE, DB019-B sec.15.4.8.5) the blend unit
 * is fixed-function src*A + dst*(1-A): the blend factor is the source
 * (vertex) alpha or the texture alpha, and nothing else. Therefore the
 * sfactor/dfactor passed to l10gl_blend_func are advisory -- effectively
 * only (SRC_ALPHA, ONE_MINUS_SRC_ALPHA) is implemented, and any other
 * factor pair degrades to that. Backends advertise L10GL_CAP_BLEND only
 * when at least this alpha blend is available in hardware.
 *
 * Application-side rule for correct transparency WITH depth buffering
 * (DB019-B sec.15.4.8.5; the standard OpenGL rule): draw all opaque
 * geometry first with depth writes enabled, then draw transparent
 * geometry back-to-front with depth WRITES off (l10gl_depth_mask(ctx, 0))
 * while leaving the depth TEST enabled. Transparent fragments then still
 * fail the depth test against closer opaque geometry, but no transparent
 * fragment writes Z, so transparent objects do not occlude one another.
 * ======================================================================== */

/* Blend factors (subset for now) */
enum l10gl_blend_func {
    L10GL_ZERO,
    L10GL_ONE,
    L10GL_SRC_COLOR,
    L10GL_ONE_MINUS_SRC_COLOR,
    L10GL_SRC_ALPHA,
    L10GL_ONE_MINUS_SRC_ALPHA,
    L10GL_DST_COLOR,
    L10GL_ONE_MINUS_DST_COLOR,
};

/* ========================================================================
 * Texture Types
 * ======================================================================== */

/* Texture pixel formats */
enum l10gl_tex_format {
    L10GL_TEX_FMT_NONE = 0,
    L10GL_TEX_FMT_ARGB8888,  /* 32 bits/pixel */
    L10GL_TEX_FMT_RGB565,    /* 16 bits/pixel, no alpha */
    L10GL_TEX_FMT_ARGB1555,  /* 16 bits/pixel, 1-bit alpha */
    L10GL_TEX_FMT_ARGB4444,  /* 16 bits/pixel, 4-bit alpha */
    L10GL_TEX_FMT_PAL8,      /* 8 bits/pixel, palettized */
};

/* Texture minification/magnification filters */
enum l10gl_tex_filter {
    L10GL_FILTER_NEAREST,
    L10GL_FILTER_LINEAR,                /* bilinear */
    L10GL_FILTER_NEAREST_MIPMAP_NEAREST,
    L10GL_FILTER_LINEAR_MIPMAP_NEAREST,
    L10GL_FILTER_NEAREST_MIPMAP_LINEAR,
    L10GL_FILTER_LINEAR_MIPMAP_LINEAR,  /* trilinear */
};

/* Texture coordinate wrapping */
enum l10gl_tex_wrap {
    L10GL_WRAP_REPEAT,
    L10GL_WRAP_CLAMP,
};

/*
 * Texture object. Allocated by the application, filled by the backend.
 * The backend stores its private data (VRAM address, hardware format)
 * in backend_data.
 */
struct l10gl_texture {
    int width;
    int height;
    enum l10gl_tex_format format;
    int bytes_per_texel;
    void *backend_data;   /* backend-private (e.g. VRAM byte offset) */
};

/* ========================================================================
 * Backend Interface
 *
 * Each hardware or software driver implements this struct. Fields set to NULL
 * mean the operation is not supported by that backend (and would require a
 * frontend fallback if one exists).
 * ======================================================================== */

struct l10gl_ctx;  /* forward decl */

struct l10gl_backend {
    const char *name;     /* e.g. "mga1064", "virge" */

    /* Optional fbdev target for shared P2 console ownership. fbdev_env is
     * checked first; an unset/empty value falls back to fbdev_path. Backends
     * with neither field are offscreen/non-console and never touch a VT. */
    const char *fbdev_path;
    const char *fbdev_env;

    /* --- Lifecycle --- */
    int  (*probe)(void);  /* >0 if this backend's hardware is present */
    int  (*init)(struct l10gl_ctx *ctx, int width, int height, int bpp);
    void (*cleanup)(struct l10gl_ctx *ctx);

    /* --- Buffer clearing --- */
    void (*clear_color)(struct l10gl_ctx *ctx, float r, float g, float b);
    void (*clear_depth)(struct l10gl_ctx *ctx, float z);
    void (*clear)(struct l10gl_ctx *ctx);  /* clear color+depth with current values */

    /* --- State --- */
    void (*depth_func)(struct l10gl_ctx *ctx, enum l10gl_depth_func func);
    void (*depth_mask)(struct l10gl_ctx *ctx, int enable); /* enable/disable Z writes */
    void (*depth_test)(struct l10gl_ctx *ctx, int enable);
    void (*blend_func)(struct l10gl_ctx *ctx,
                       enum l10gl_blend_func sfactor,
                       enum l10gl_blend_func dfactor);
    void (*blend_enable)(struct l10gl_ctx *ctx, int enable);

    /* --- Drawing --- */
    void (*draw_triangle)(struct l10gl_ctx *ctx,
                          struct l10gl_vertex v0,
                          struct l10gl_vertex v1,
                          struct l10gl_vertex v2);
    void (*draw_textured_triangle)(struct l10gl_ctx *ctx,
                                    struct l10gl_vertex v0,
                                    struct l10gl_vertex v1,
                                    struct l10gl_vertex v2);
    void (*draw_line)(struct l10gl_ctx *ctx,
                      struct l10gl_vertex v0,
                      struct l10gl_vertex v1);
    void (*fill_rect)(struct l10gl_ctx *ctx,
                      int x, int y, int w, int h, uint32_t color);

    /* --- Texture management --- */
    int  (*tex_image_2d)(struct l10gl_ctx *ctx, struct l10gl_texture *tex,
                         int width, int height,
                         enum l10gl_tex_format format,
                         const void *data);
    void (*bind_texture)(struct l10gl_ctx *ctx, struct l10gl_texture *tex);
    void (*tex_parameter)(struct l10gl_ctx *ctx,
                          enum l10gl_tex_filter filter,
                          enum l10gl_tex_wrap wrap);

    /* --- Synchronization --- */
    void (*wait_engine)(struct l10gl_ctx *ctx);
    void (*wait_vsync)(struct l10gl_ctx *ctx);
    void (*swap_buffers)(struct l10gl_ctx *ctx);  /* page-flip back buffer to scanout (double-buffer) */

    /* --- Capabilities (queried at init) --- */
    unsigned int caps;   /* bitmask of L10GL_CAP_* bits */
};

/* Capability bits */
#define L10GL_CAP_GOURAUD     (1 << 0)  /* Gouraud shading */
#define L10GL_CAP_ZBUFFER     (1 << 1)  /* Z-buffer */
#define L10GL_CAP_LINES       (1 << 2)  /* Line drawing */
#define L10GL_CAP_TEXTURE     (1 << 3)  /* Texture mapping */
#define L10GL_CAP_BLEND       (1 << 4)  /* Alpha blending */
#define L10GL_CAP_DITHER      (1 << 5)  /* Color dithering */
#define L10GL_CAP_TRILINEAR   (1 << 6)  /* Trilinear texture filtering */
#define L10GL_CAP_BILINEAR    (1 << 7)  /* Bilinear texture filtering */
#define L10GL_CAP_PERSPECTIVE (1 << 8)  /* Perspective-correct texture mapping */

/* ========================================================================
 * Context
 *
 * The context holds backend-specific data (via an opaque pointer) and
 * cached render state. Applications allocate one context and pass it to
 * all L10GL functions.
 * ======================================================================== */

struct l10gl_ctx {
    const struct l10gl_backend *backend;  /* const backend vtable */
    void *backend_data;                   /* backend-private state (e.g. mga1064_ctx) */
    void *console_data;                   /* shared console.c ownership state */

    /* Actual screen geometry selected/adopted by the backend. The create
     * arguments are requests; these fields are authoritative after init. */
    int width;
    int height;
    int bpp;              /* storage bytes per pixel */
    uint32_t stride;      /* bytes per scanline, including padding */
    struct l10gl_pixel_format pixel_format;

    /* Cached clear values */
    float clear_r, clear_g, clear_b;
    float clear_z;

    /* Cached depth state */
    enum l10gl_depth_func depth_func_val;
    int depth_test_enabled;
    int depth_writes_enabled;

    /* Cached blend state */
    int blend_enabled;
    enum l10gl_blend_func blend_sfactor;
    enum l10gl_blend_func blend_dfactor;

    /* Current texture (NULL = no texture bound) */
    struct l10gl_texture *current_texture;

    /* Frontend transform state. Viewport Y follows OpenGL's lower-left
     * convention; l10gl_ndc_to_window converts it to the backends' top-left
     * framebuffer coordinates. */
    float modelview_stack[L10GL_MODELVIEW_STACK_DEPTH][16];
    float projection_stack[L10GL_PROJECTION_STACK_DEPTH][16];
    int modelview_top;
    int projection_top;
    enum l10gl_matrix_mode matrix_mode_val;
    int viewport_x, viewport_y, viewport_width, viewport_height;
    float depth_range_near, depth_range_far;

    /* Immediate-mode current attributes and streaming assembly state. */
    float current_r, current_g, current_b, current_a;
    float current_nx, current_ny, current_nz;
    float current_u, current_v;
    enum l10gl_cull_mode cull_mode_val;
    int flat_shading;

    /* X4 frontend lighting. The directional vector is the eye-space
     * direction in which light rays travel; normals point toward the light
     * when their dot product with the negated direction is positive. */
    int lighting_enabled;
    float light_dir_x, light_dir_y, light_dir_z;
    float light_r, light_g, light_b;
    float ambient_r, ambient_g, ambient_b;
    float material_r, material_g, material_b, material_a;

    int immediate_active;
    enum l10gl_primitive immediate_primitive;
    unsigned long immediate_vertex_count;
    struct l10gl_immediate_vertex immediate_vertices[4];
};

/* ========================================================================
 * Public API — hardware-agnostic frontend
 *
 * These are thin wrappers that call through backend->func.
 * ======================================================================== */

/*
 * l10gl_create - Create a context with a specified backend.
 * Returns 0 on success, negative on failure.
 */
int l10gl_create(struct l10gl_ctx *ctx,
                 const struct l10gl_backend *backend,
                 int width, int height, int bpp);

/* Select the first detected backend (ViRGE first), unless overridden by
 * L10GL_BACKEND=<name>. A forced backend is returned without probing so its
 * init path can report the precise reason it cannot start. */
const struct l10gl_backend *l10gl_autodetect(void);
int l10gl_create_auto(struct l10gl_ctx *ctx,
                      int width, int height, int bpp);

void l10gl_destroy(struct l10gl_ctx *ctx);

/* Clearing */
void l10gl_clear_color(struct l10gl_ctx *ctx, float r, float g, float b);
void l10gl_clear_depth(struct l10gl_ctx *ctx, float z);
void l10gl_clear(struct l10gl_ctx *ctx);

/* State */
void l10gl_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func);
void l10gl_depth_mask(struct l10gl_ctx *ctx, int enable);
void l10gl_enable_depth_test(struct l10gl_ctx *ctx, int enable);
void l10gl_enable_blend(struct l10gl_ctx *ctx, int enable);
void l10gl_blend_func(struct l10gl_ctx *ctx,
                      enum l10gl_blend_func sfactor,
                      enum l10gl_blend_func dfactor);

/* Transform state. Stack operations and projection constructors return zero
 * on success or a negative errno-style value for invalid input/stack bounds. */
int l10gl_matrix_mode(struct l10gl_ctx *ctx, enum l10gl_matrix_mode mode);
void l10gl_load_identity(struct l10gl_ctx *ctx);
void l10gl_load_matrixf(struct l10gl_ctx *ctx, const float matrix[16]);
void l10gl_mult_matrixf(struct l10gl_ctx *ctx, const float matrix[16]);
int l10gl_get_matrix(const struct l10gl_ctx *ctx,
                     enum l10gl_matrix_mode mode, float matrix[16]);
int l10gl_push_matrix(struct l10gl_ctx *ctx);
int l10gl_pop_matrix(struct l10gl_ctx *ctx);
void l10gl_translatef(struct l10gl_ctx *ctx, float x, float y, float z);
int l10gl_rotatef(struct l10gl_ctx *ctx, float angle_degrees,
                  float x, float y, float z);
void l10gl_scalef(struct l10gl_ctx *ctx, float x, float y, float z);
int l10gl_frustum(struct l10gl_ctx *ctx,
                  float left, float right, float bottom, float top,
                  float z_near, float z_far);
int l10gl_perspective(struct l10gl_ctx *ctx, float fovy_degrees,
                      float aspect, float z_near, float z_far);
int l10gl_ortho(struct l10gl_ctx *ctx,
                float left, float right, float bottom, float top,
                float z_near, float z_far);
int l10gl_viewport(struct l10gl_ctx *ctx, int x, int y,
                   int width, int height);
void l10gl_depth_range(struct l10gl_ctx *ctx, float z_near, float z_far);

/* Reusable X1 math helpers for the Phase 2 primitive pipeline. Inputs and
 * outputs may alias. object_to_clip applies PROJECTION * MODELVIEW. */
void l10gl_transform_vec4(const float matrix[16],
                          const float input[4], float output[4]);
void l10gl_object_to_clip(const struct l10gl_ctx *ctx,
                          const float object[4], float clip[4]);
void l10gl_ndc_to_window(const struct l10gl_ctx *ctx,
                         const float ndc[3], float window[3]);

/* Immediate-mode model-space submission. Attributes are current state and are
 * captured by each vertex. begin/end/vertex return negative errno-style
 * values for invalid nesting, unsupported primitives, or out-of-block use.
 * Incomplete primitives at end are ignored, matching legacy OpenGL. */
int l10gl_begin(struct l10gl_ctx *ctx, enum l10gl_primitive primitive);
int l10gl_end(struct l10gl_ctx *ctx);
int l10gl_vertex3f(struct l10gl_ctx *ctx, float x, float y, float z);
void l10gl_color4f(struct l10gl_ctx *ctx,
                   float r, float g, float b, float a);
void l10gl_normal3f(struct l10gl_ctx *ctx, float x, float y, float z);
void l10gl_texcoord2f(struct l10gl_ctx *ctx, float u, float v);
int l10gl_cull_face(struct l10gl_ctx *ctx, enum l10gl_cull_mode mode);
void l10gl_shade_flat(struct l10gl_ctx *ctx, int enable);

/* X4 directional diffuse + ambient lighting. Lighting is disabled by
 * default. light_dir is an eye-space light-ray direction and normalizes its
 * input; a zero or non-finite direction returns -EINVAL without changing
 * state. Normals are transformed by inverse-transpose MODELVIEW at vertex
 * submission. Material supplies RGB reflectance and output alpha. */
void l10gl_enable_lighting(struct l10gl_ctx *ctx, int enable);
int l10gl_light_dir(struct l10gl_ctx *ctx, float x, float y, float z);
void l10gl_light_color(struct l10gl_ctx *ctx, float r, float g, float b);
void l10gl_light_ambient(struct l10gl_ctx *ctx, float r, float g, float b);
void l10gl_material(struct l10gl_ctx *ctx,
                    float r, float g, float b, float a);

/* Drawing primitives */
void l10gl_draw_triangle(struct l10gl_ctx *ctx,
                          struct l10gl_vertex v0,
                          struct l10gl_vertex v1,
                          struct l10gl_vertex v2);

void l10gl_draw_textured_triangle(struct l10gl_ctx *ctx,
                                   struct l10gl_vertex v0,
                                   struct l10gl_vertex v1,
                                   struct l10gl_vertex v2);

void l10gl_draw_line(struct l10gl_ctx *ctx,
                      struct l10gl_vertex v0,
                      struct l10gl_vertex v1);

void l10gl_draw_rect(struct l10gl_ctx *ctx,
                     int x, int y, int w, int h, uint32_t color);

/* Texture management */
int l10gl_tex_image_2d(struct l10gl_ctx *ctx, struct l10gl_texture *tex,
                        int width, int height,
                        enum l10gl_tex_format format,
                        const void *data);
void l10gl_bind_texture(struct l10gl_ctx *ctx, struct l10gl_texture *tex);
void l10gl_tex_parameter(struct l10gl_ctx *ctx,
                         enum l10gl_tex_filter filter,
                         enum l10gl_tex_wrap wrap);

/* Sync */
void l10gl_wait_engine(struct l10gl_ctx *ctx);
void l10gl_wait_vsync(struct l10gl_ctx *ctx);

/* Double-buffering: publish the back buffer (the current render target)
 * to the scanout via a vsync-synchronized page flip, then make the former
 * front buffer the new render target. EGL/OpenGL double-buffer model:
 * render a frame, then call this once. Backends that don't double-buffer
 * leave the slot NULL and this is a no-op (single-buffer passthrough). */
void l10gl_swap_buffers(struct l10gl_ctx *ctx);

/* Capabilities query */
int l10gl_has_cap(struct l10gl_ctx *ctx, unsigned int cap);

/* Backend registration (each backend provides one of these) */
extern const struct l10gl_backend mga1064_backend;
extern const struct l10gl_backend swrast_backend;
extern const struct l10gl_backend virge_backend;

#endif /* L10GL_H */
