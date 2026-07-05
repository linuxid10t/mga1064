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
    float r, g, b, a; /* Color (0.0 to 1.0) */
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
 * Backend Interface
 *
 * Each hardware driver implements this struct. Fields set to NULL mean
 * the operation is not supported in hardware (and would require software
 * fallback if the frontend implements one).
 * ======================================================================== */

struct l10gl_ctx;  /* forward decl */

struct l10gl_backend {
    const char *name;     /* e.g. "mga1064" */

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
    void (*draw_line)(struct l10gl_ctx *ctx,
                      struct l10gl_vertex v0,
                      struct l10gl_vertex v1);
    void (*fill_rect)(struct l10gl_ctx *ctx,
                      int x, int y, int w, int h, uint32_t color);

    /* --- Synchronization --- */
    void (*wait_engine)(struct l10gl_ctx *ctx);
    void (*wait_vsync)(struct l10gl_ctx *ctx);

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

    /* Cached state */
    enum l10gl_depth_func depth_func_val;
    int depth_test_enabled;
    int depth_writes_enabled;
    int blend_enabled;
};

/* ========================================================================
 * Public API — hardware-agnostic frontend
 *
 * These are thin wrappers that call through backend->func.
 * Future versions can add software fallback, vertex arrays, etc.
 * ======================================================================== */

/*
 * l10gl_create - Create a context with a specified backend.
 * @backend:  The hardware backend vtable (e.g. &mga1064_backend).
 * @width, height, bpp: Screen dimensions.
 *
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

/* Drawing primitives (dispatch to backend triangles/lines) */
void l10gl_draw_triangle(struct l10gl_ctx *ctx,
                          struct l10gl_vertex v0,
                          struct l10gl_vertex v1,
                          struct l10gl_vertex v2);

void l10gl_draw_rect(struct l10gl_ctx *ctx,
                     int x, int y, int w, int h, uint32_t color);

/* Sync */
void l10gl_wait_engine(struct l10gl_ctx *ctx);
void l10gl_wait_vsync(struct l10gl_ctx *ctx);

/* Capabilities query */
int l10gl_has_cap(struct l10gl_ctx *ctx, unsigned int cap);

/* Backend registration (each backend provides one of these) */
extern const struct l10gl_backend mga1064_backend;

#endif /* L10GL_H */
