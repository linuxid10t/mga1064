/*
 * l10gl_mga1064.c - L10GL backend glue for the Matrox MGA-1064SG.
 *
 * This file adapts the mga1064 hardware driver to the l10gl_backend
 * vtable. It wraps the existing mga1064_* functions, converting between
 * the generic l10gl_vertex/l10gl types and the hardware-specific formats.
 *
 * The actual register programming lives in mga1064.c.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "../../l10gl.h"
#include "mga1064.h"

/* ========================================================================
 * Color conversion helpers
 * ======================================================================== */

/* Convert float RGB (0..1) to 16bpp 5:6:5 */
static uint16_t rgb_to_565(float r, float g, float b)
{
    int ri = (int)(r * 31.0f + 0.5f) & 0x1F;
    int gi = (int)(g * 63.0f + 0.5f) & 0x3F;
    int bi = (int)(b * 31.0f + 0.5f) & 0x1F;
    return (ri << 11) | (gi << 5) | bi;
}

/* ========================================================================
 * Backend-private data
 *
 * We store the mga1064_ctx (hardware state) and cached state that
 * affects how DWGCTL is programmed for each draw call.
 * ======================================================================== */

struct mga1064_private {
    struct mga1064_ctx hw;    /* low-level hardware context */
    int depth_test_enabled;
    int depth_writes_enabled;
    enum l10gl_depth_func depth_func_val;
};

/* Get the private data from the l10gl context */
static inline struct mga1064_private *MGA_PRIV(struct l10gl_ctx *ctx)
{
    return (struct mga1064_private *)ctx->backend_data;
}

/* ========================================================================
 * Depth function mapping
 *
 * l10gl uses OpenGL-style depth functions. The 1064 DWGCTL zmode field
 * supports: NOZCMP, ZE (=), ZNE (≠), ZLT (<), ZLTE (<=), ZGT (>), ZGTE (>=).
 *
 * When depth test is disabled, we use NOZCMP (always write).
 * When depth writes are disabled, we use atype=I instead of atype=ZI
 * (I = Gouraud with depth compare but Z not updated).
 * ======================================================================== */

/* L10GL depth func → MGA zmode mapping.
 * Used by future draw calls that respect cached depth state. */
__attribute__((unused))
static uint32_t mga_zmode_for_func(enum l10gl_depth_func func)
{
    switch (func) {
    case L10GL_NEVER:    return 0;                        /* skip all */
    case L10GL_LESS:     return MGA_ZMODE_ZLT;
    case L10GL_EQUAL:    return MGA_ZMODE_ZE;
    case L10GL_LEQUAL:   return MGA_ZMODE_ZLTE;
    case L10GL_GREATER:  return MGA_ZMODE_ZGT;
    case L10GL_GEQUAL:   return MGA_ZMODE_ZGTE;
    case L10GL_NOTEQUAL: return MGA_ZMODE_ZNE;
    case L10GL_ALWAYS:   return MGA_ZMODE_NOZCMP;
    }
    return MGA_ZMODE_NOZCMP;
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

static int mga_be_init(struct l10gl_ctx *ctx, int w, int h, int bpp)
{
    struct mga1064_private *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return -1;
    ctx->backend_data = priv;

    int ret = mga1064_init(&priv->hw, w, h, bpp);
    if (ret) {
        free(priv);
        ctx->backend_data = NULL;
        return ret;
    }

    /* Defaults */
    priv->depth_test_enabled = 1;
    priv->depth_writes_enabled = 1;
    priv->depth_func_val = L10GL_LESS;

    return 0;
}

static void mga_be_cleanup(struct l10gl_ctx *ctx)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);
    if (priv) {
        mga1064_cleanup(&priv->hw);
        free(priv);
        ctx->backend_data = NULL;
    }
}

/* ========================================================================
 * Buffer clearing
 * ======================================================================== */

static void mga_be_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);
    uint32_t color;

    if (priv->hw.bpp == 2)
        color = rgb_to_565(r, g, b);
    else
        color = ((int)(r * 255) << 16) | ((int)(g * 255) << 8) | (int)(b * 255);

    mga1064_fill_rect(&priv->hw, 0, 0, priv->hw.width, priv->hw.height, color);
}

static void mga_be_clear_depth(struct l10gl_ctx *ctx, float z)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);
    mga1064_clear_z(&priv->hw, z);
}

/* ========================================================================
 * State
 * ======================================================================== */

static void mga_be_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func)
{
    MGA_PRIV(ctx)->depth_func_val = func;
}

static void mga_be_depth_mask(struct l10gl_ctx *ctx, int enable)
{
    MGA_PRIV(ctx)->depth_writes_enabled = enable;
}

static void mga_be_depth_test(struct l10gl_ctx *ctx, int enable)
{
    MGA_PRIV(ctx)->depth_test_enabled = enable;
}

/* ========================================================================
 * Drawing
 *
 * The existing mga1064_draw_triangle_gouraud takes mga_vertex structs.
 * l10gl_vertex has the same layout (x, y, z, r, g, b) plus alpha.
 * We convert and call the existing driver.
 * ======================================================================== */

static void mga_be_draw_triangle(struct l10gl_ctx *ctx,
                                  struct l10gl_vertex v0,
                                  struct l10gl_vertex v1,
                                  struct l10gl_vertex v2)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);

    struct mga_vertex m0 = { v0.x, v0.y, v0.z, v0.w, v0.r, v0.g, v0.b, v0.u, v0.v };
    struct mga_vertex m1 = { v1.x, v1.y, v1.z, v1.w, v1.r, v1.g, v1.b, v1.u, v1.v };
    struct mga_vertex m2 = { v2.x, v2.y, v2.z, v2.w, v2.r, v2.g, v2.b, v2.u, v2.v };

    /* If depth test is disabled, fall through to the existing function
     * which uses ZLTE. A proper implementation would program DWGCTL
     * with NOZCMP + RPL here. For now we still pass through Z. */

    /* TODO: use cached depth_func and depth_writes to select DWGCTL
     * atype/zmode in draw_gouraud_trap. This requires extending the
     * backend interface slightly. */

    mga1064_draw_triangle_gouraud(&priv->hw, m0, m1, m2);
}

static void mga_be_draw_line(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);
    uint32_t color;

    /* Use v0's color for the line. Average v0/v1 colors for Gouraud lines
     * would be better but the 1064 line engine does flat color only. */
    if (priv->hw.bpp == 2)
        color = rgb_to_565(v0.r, v0.g, v0.b);
    else
        color = ((int)(v0.r * 255) << 16) | ((int)(v0.g * 255) << 8) | (int)(v0.b * 255);

    mga1064_draw_line(&priv->hw,
                      (int)v0.x, (int)v0.y, (int)v1.x, (int)v1.y, color);
}

static void mga_be_fill_rect(struct l10gl_ctx *ctx,
                             int x, int y, int w, int h, uint32_t color)
{
    struct mga1064_private *priv = MGA_PRIV(ctx);
    mga1064_fill_rect(&priv->hw, x, y, w, h, color);
}

/* ========================================================================
 * Sync
 * ======================================================================== */

static void mga_be_wait_engine(struct l10gl_ctx *ctx)
{
    mga1064_wait_engine(&MGA_PRIV(ctx)->hw);
}

static void mga_be_wait_vsync(struct l10gl_ctx *ctx)
{
    mga1064_wait_vsync(&MGA_PRIV(ctx)->hw);
}

/* ========================================================================
 * Backend Vtable
 * ======================================================================== */

const struct l10gl_backend mga1064_backend = {
    .name         = "mga1064",
    .init         = mga_be_init,
    .cleanup      = mga_be_cleanup,
    .clear_color  = mga_be_clear_color,
    .clear_depth  = mga_be_clear_depth,
    .clear        = NULL,  /* frontend handles color+depth dispatch */
    .depth_func   = mga_be_depth_func,
    .depth_mask   = mga_be_depth_mask,
    .depth_test   = mga_be_depth_test,
    .blend_func   = NULL,  /* not yet supported by 1064 hardware */
    .blend_enable = NULL,
    .draw_triangle = mga_be_draw_triangle,
    .draw_line     = mga_be_draw_line,
    .fill_rect     = mga_be_fill_rect,
    .wait_engine   = mga_be_wait_engine,
    .wait_vsync    = mga_be_wait_vsync,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER | L10GL_CAP_LINES,
};
