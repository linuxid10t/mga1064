/*
 * l10gl.c - L10GL frontend: thin dispatch layer over backend vtable.
 *
 * Applications link against this and a backend (e.g. mga1064).
 * This layer handles state caching and dispatch.
 */

#include <string.h>
#include <stdio.h>

#include "l10gl.h"

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

int l10gl_create(struct l10gl_ctx *ctx,
                 const struct l10gl_backend *backend,
                 int width, int height, int bpp)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->backend = backend;

    /* Set default state before init so backend sees it */
    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;

    ctx->clear_r = 0.0f;
    ctx->clear_g = 0.0f;
    ctx->clear_b = 0.0f;
    ctx->clear_z = 1.0f;

    ctx->depth_func_val = L10GL_LESS;
    ctx->depth_test_enabled = 1;
    ctx->depth_writes_enabled = 1;
    ctx->blend_enabled = 0;

    if (backend->init) {
        int ret = backend->init(ctx, width, height, bpp);
        if (ret)
            return ret;
    }

    printf("L10GL: Backend '%s' initialized (%dx%d @ %dbpp)\n",
           backend->name, width, height, bpp * 8);
    printf("  Caps: %s%s%s%s%s%s\n",
           ctx->backend->caps & L10GL_CAP_GOURAUD  ? "Gouraud " : "",
           ctx->backend->caps & L10GL_CAP_ZBUFFER  ? "Z-buffer " : "",
           ctx->backend->caps & L10GL_CAP_LINES    ? "Lines " : "",
           ctx->backend->caps & L10GL_CAP_TEXTURE  ? "Texture " : "",
           ctx->backend->caps & L10GL_CAP_BLEND    ? "Blend " : "",
           ctx->backend->caps & L10GL_CAP_DITHER   ? "Dither " : "");

    return 0;
}

void l10gl_destroy(struct l10gl_ctx *ctx)
{
    if (ctx->backend && ctx->backend->cleanup)
        ctx->backend->cleanup(ctx);
}

/* ========================================================================
 * Clearing
 * ======================================================================== */

void l10gl_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    ctx->clear_r = r;
    ctx->clear_g = g;
    ctx->clear_b = b;
}

void l10gl_clear_depth(struct l10gl_ctx *ctx, float z)
{
    ctx->clear_z = z;
}

void l10gl_clear(struct l10gl_ctx *ctx)
{
    /* Clear depth buffer if backend supports it */
    if (ctx->backend->clear_depth && (ctx->backend->caps & L10GL_CAP_ZBUFFER))
        ctx->backend->clear_depth(ctx, ctx->clear_z);

    /* Clear color buffer */
    if (ctx->backend->clear_color)
        ctx->backend->clear_color(ctx, ctx->clear_r, ctx->clear_g, ctx->clear_b);
}

/* ========================================================================
 * State
 * ======================================================================== */

void l10gl_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func)
{
    ctx->depth_func_val = func;
    if (ctx->backend->depth_func)
        ctx->backend->depth_func(ctx, func);
}

void l10gl_depth_mask(struct l10gl_ctx *ctx, int enable)
{
    ctx->depth_writes_enabled = enable;
    if (ctx->backend->depth_mask)
        ctx->backend->depth_mask(ctx, enable);
}

void l10gl_enable_depth_test(struct l10gl_ctx *ctx, int enable)
{
    ctx->depth_test_enabled = enable;
    if (ctx->backend->depth_test)
        ctx->backend->depth_test(ctx, enable);
}

void l10gl_enable_blend(struct l10gl_ctx *ctx, int enable)
{
    ctx->blend_enabled = enable;
    if (ctx->backend->blend_enable)
        ctx->backend->blend_enable(ctx, enable);
}

void l10gl_blend_func(struct l10gl_ctx *ctx,
                      enum l10gl_blend_func sfactor,
                      enum l10gl_blend_func dfactor)
{
    if (ctx->backend->blend_func)
        ctx->backend->blend_func(ctx, sfactor, dfactor);
}

/* ========================================================================
 * Drawing
 * ======================================================================== */

void l10gl_draw_triangle(struct l10gl_ctx *ctx,
                          struct l10gl_vertex v0,
                          struct l10gl_vertex v1,
                          struct l10gl_vertex v2)
{
    if (ctx->backend->draw_triangle)
        ctx->backend->draw_triangle(ctx, v0, v1, v2);
}

void l10gl_draw_rect(struct l10gl_ctx *ctx,
                     int x, int y, int w, int h, uint32_t color)
{
    if (ctx->backend->fill_rect)
        ctx->backend->fill_rect(ctx, x, y, w, h, color);
}

/* ========================================================================
 * Sync
 * ======================================================================== */

void l10gl_wait_engine(struct l10gl_ctx *ctx)
{
    if (ctx->backend->wait_engine)
        ctx->backend->wait_engine(ctx);
}

void l10gl_wait_vsync(struct l10gl_ctx *ctx)
{
    if (ctx->backend->wait_vsync)
        ctx->backend->wait_vsync(ctx);
}

/* ========================================================================
 * Capabilities
 * ======================================================================== */

int l10gl_has_cap(struct l10gl_ctx *ctx, unsigned int cap)
{
    return (ctx->backend->caps & cap) ? 1 : 0;
}
