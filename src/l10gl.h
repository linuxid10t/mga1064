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

/* ========================================================================
 * Vertex and Primitive Types
 * ======================================================================== */

struct l10gl_vertex {
    float x, y;       /* Screen coordinates (pixels) */
    float z;          /* Depth value (0.0 = near, 1.0 = far) */
    float w;          /* 1/Z_eye for perspective correction (1.0 = disable) */
    float r, g, b, a; /* Color (0.0 to 1.0) */
    float u, v;       /* Texture coordinates (0.0 to 1.0) */
};

/* Primitive types */
enum l10gl_primitive {
    L10GL_TRIANGLES,     /* Every 3 vertices = 1 triangle */
    L10GL_TRIANGLE_STRIP,/* Strip of triangles sharing edges */
    L10GL_TRIANGLE_FAN,  /* Fan of triangles sharing first vertex */
    L10GL_LINES,         /* Independent line segments */
    L10GL_LINE_STRIP,    /* Connected line segments */
    L10GL_POINTS,
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
 * Each hardware driver implements this struct. Fields set to NULL mean
 * the operation is not supported in hardware (and would require software
 * fallback if the frontend implements one).
 * ======================================================================== */

struct l10gl_ctx;  /* forward decl */

struct l10gl_backend {
    const char *name;     /* e.g. "mga1064", "virge" */

    /* --- Lifecycle --- */
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
#define L10GL_CAP_GOURAUD     (1 << 0)  /* Hardware Gouraud shading */
#define L10GL_CAP_ZBUFFER     (1 << 1)  /* Hardware Z-buffer */
#define L10GL_CAP_LINES       (1 << 2)  /* Hardware line drawing */
#define L10GL_CAP_TEXTURE     (1 << 3)  /* Hardware texture mapping */
#define L10GL_CAP_BLEND       (1 << 4)  /* Hardware alpha blending */
#define L10GL_CAP_DITHER      (1 << 5)  /* Hardware color dithering */
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

    /* Screen geometry */
    int width;
    int height;
    int bpp;  /* bytes per pixel */

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
extern const struct l10gl_backend virge_backend;

#endif /* L10GL_H */
