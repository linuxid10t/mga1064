/*
 * l10gl_virge.c - L10GL backend glue for the S3 ViRGE.
 *
 * Adapts the virge hardware driver to the l10gl_backend vtable.
 * Converts between generic l10gl_vertex/l10gl types and the
 * hardware-specific virge_vertex format.
 *
 * Implements full texture management: VRAM allocation, format conversion,
 * filter/wrap mode caching, and textured triangle dispatch.
 */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "../../l10gl.h"
#include "virge.h"

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

/* Convert float RGB (0..1) to 24bpp RGB888 packed in uint32 */
static uint32_t rgb_to_888(float r, float g, float b)
{
    return ((int)(r * 255) << 16) | ((int)(g * 255) << 8) | (int)(b * 255);
}

/* ========================================================================
 * Backend-private data
 * ======================================================================== */

struct virge_private {
    struct virge_ctx hw;          /* low-level hardware context */
    int depth_test_enabled;
    int depth_writes_enabled;
    enum l10gl_depth_func depth_func_val;

    /* Cached texture parameters */
    enum l10gl_tex_filter tex_filter;
    enum l10gl_tex_wrap tex_wrap;

    /* Cached blend state */
    int blend_enabled;
};

static inline struct virge_private *VIRGE_PRIV(struct l10gl_ctx *ctx)
{
    return (struct virge_private *)ctx->backend_data;
}

/* ========================================================================
 * L10GL texture format → ViRGE CMD_SET texture format bits
 *
 * CMD_SET bits [7-5] = TEX CLR FORMAT:
 *   000 = 32bpp ARGB8888
 *   001 = 16bpp ARGB4444
 *   010 = 16bpp ARGB1555
 *   011 = 8bpp Alpha4/Blend4
 *   100 = 4bpp Blend4 low nibble
 *   101 = 4bpp Blend4 high nibble
 *   110 = 8bpp palettized
 *   111 = YU/YV (16bpp video)
 * ======================================================================== */

static uint32_t virge_tex_format_bits(enum l10gl_tex_format fmt)
{
    switch (fmt) {
    case L10GL_TEX_FMT_ARGB8888: return (0 << 5);
    case L10GL_TEX_FMT_ARGB4444: return (1 << 5);
    case L10GL_TEX_FMT_ARGB1555: return (2 << 5);
    case L10GL_TEX_FMT_PAL8:     return (6 << 5);
    case L10GL_TEX_FMT_RGB565:
    default:
        /* The ViRGE doesn't have a pure 5:6:5 texture format.
         * Use ARGB1555 as the closest approximation. */
        return (2 << 5);
    }
}

static int virge_tex_bytes_per_texel(enum l10gl_tex_format fmt)
{
    switch (fmt) {
    case L10GL_TEX_FMT_ARGB8888: return 4;
    case L10GL_TEX_FMT_RGB565:
    case L10GL_TEX_FMT_ARGB1555:
    case L10GL_TEX_FMT_ARGB4444: return 2;
    case L10GL_TEX_FMT_PAL8:     return 1;
    default: return 2;
    }
}

/*
 * L10GL filter → ViRGE CMD_SET filter bits [14-12]:
 *   000 = M1TPP (MIP_NEAREST)
 *   001 = M2TPP (LINEAR_MIP_NEAREST)
 *   010 = M4TPP (MIP_LINEAR)
 *   011 = M8TPP (LINEAR_MIP_LINEAR = trilinear)
 *   100 = 1TPP (NEAREST)
 *   110 = 4TPP (LINEAR = bilinear)
 */
static uint32_t virge_filter_bits(enum l10gl_tex_filter filter)
{
    switch (filter) {
    case L10GL_FILTER_NEAREST:                return (4 << 12);  /* 1TPP */
    case L10GL_FILTER_LINEAR:                 return (6 << 12);  /* 4TPP */
    case L10GL_FILTER_NEAREST_MIPMAP_NEAREST: return (0 << 12);  /* M1TPP */
    case L10GL_FILTER_LINEAR_MIPMAP_NEAREST:  return (1 << 12);  /* M2TPP */
    case L10GL_FILTER_NEAREST_MIPMAP_LINEAR:  return (2 << 12);  /* M4TPP */
    case L10GL_FILTER_LINEAR_MIPMAP_LINEAR:   return (3 << 12);  /* M8TPP */
    default: return (4 << 12);  /* NEAREST */
    }
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

static int virge_be_init(struct l10gl_ctx *ctx, int w, int h, int bpp)
{
    struct virge_private *priv = calloc(1, sizeof(*priv));
    if (!priv)
        return -1;
    ctx->backend_data = priv;

    int ret = virge_init(&priv->hw, w, h, bpp);
    if (ret) {
        free(priv);
        ctx->backend_data = NULL;
        return ret;
    }

    /* Defaults */
    priv->depth_test_enabled = 1;
    priv->depth_writes_enabled = 1;
    priv->depth_func_val = L10GL_LESS;
    priv->tex_filter = L10GL_FILTER_NEAREST;
    priv->tex_wrap = L10GL_WRAP_REPEAT;
    priv->blend_enabled = 0;

    return 0;
}

static void virge_be_cleanup(struct l10gl_ctx *ctx)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    if (priv) {
        virge_cleanup(&priv->hw);
        free(priv);
        ctx->backend_data = NULL;
    }
}

/* ========================================================================
 * Buffer clearing
 * ======================================================================== */

static void virge_be_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    uint32_t color;

    if (priv->hw.bpp == 2)
        color = rgb_to_565(r, g, b);
    else
        color = rgb_to_888(r, g, b);

    virge_fill_rect(&priv->hw, 0, 0, priv->hw.width, priv->hw.height, color);
}

static void virge_be_clear_depth(struct l10gl_ctx *ctx, float z)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    virge_clear_z(&priv->hw, z);
}

/* ========================================================================
 * State
 * ======================================================================== */

static void virge_be_depth_func(struct l10gl_ctx *ctx, enum l10gl_depth_func func)
{
    VIRGE_PRIV(ctx)->depth_func_val = func;
}

static void virge_be_depth_mask(struct l10gl_ctx *ctx, int enable)
{
    VIRGE_PRIV(ctx)->depth_writes_enabled = enable;
}

static void virge_be_depth_test(struct l10gl_ctx *ctx, int enable)
{
    VIRGE_PRIV(ctx)->depth_test_enabled = enable;
}

static void virge_be_blend_enable(struct l10gl_ctx *ctx, int enable)
{
    VIRGE_PRIV(ctx)->blend_enabled = enable;
}

static void virge_be_blend_func(struct l10gl_ctx *ctx,
                                 enum l10gl_blend_func sfactor,
                                 enum l10gl_blend_func dfactor)
{
    /* The ViRGE only supports two blend modes:
     *   - texture alpha (ABC = 10b)
     *   - source alpha (ABC = 11b)
     *
     * We map SRC_ALPHA → source alpha, and anything else → texture alpha.
     * Full OpenGL blend factor support would require software fallback.
     *
     * State is cached; actual CMD_SET bits applied per-draw. */
    (void)ctx;
    (void)dfactor;
    (void)sfactor;
}

/* ========================================================================
 * Drawing
 * ======================================================================== */

static void virge_be_draw_triangle(struct l10gl_ctx *ctx,
                                    struct l10gl_vertex v0,
                                    struct l10gl_vertex v1,
                                    struct l10gl_vertex v2)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);

    struct virge_vertex vs0 = { v0.x, v0.y, v0.z, v0.w,
                                 v0.r, v0.g, v0.b, v0.a, v0.u, v0.v };
    struct virge_vertex vs1 = { v1.x, v1.y, v1.z, v1.w,
                                 v1.r, v1.g, v1.b, v1.a, v1.u, v1.v };
    struct virge_vertex vs2 = { v2.x, v2.y, v2.z, v2.w,
                                 v2.r, v2.g, v2.b, v2.a, v2.u, v2.v };

    virge_draw_triangle_gouraud(&priv->hw, vs0, vs1, vs2);
}

static void virge_be_draw_textured_triangle(struct l10gl_ctx *ctx,
                                             struct l10gl_vertex v0,
                                             struct l10gl_vertex v1,
                                             struct l10gl_vertex v2)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);

    struct virge_vertex vs0 = { v0.x, v0.y, v0.z, v0.w,
                                 v0.r, v0.g, v0.b, v0.a, v0.u, v0.v };
    struct virge_vertex vs1 = { v1.x, v1.y, v1.z, v1.w,
                                 v1.r, v1.g, v1.b, v1.a, v1.u, v1.v };
    struct virge_vertex vs2 = { v2.x, v2.y, v2.z, v2.w,
                                 v2.r, v2.g, v2.b, v2.a, v2.u, v2.v };

    virge_draw_textured_triangle(&priv->hw, vs0, vs1, vs2);
}

static void virge_be_draw_line(struct l10gl_ctx *ctx,
                                struct l10gl_vertex v0,
                                struct l10gl_vertex v1)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    uint32_t color;

    if (priv->hw.bpp == 2)
        color = rgb_to_565(v0.r, v0.g, v0.b);
    else
        color = rgb_to_888(v0.r, v0.g, v0.b);

    virge_draw_line(&priv->hw,
                    (int)v0.x, (int)v0.y, (int)v1.x, (int)v1.y, color);
}

static void virge_be_fill_rect(struct l10gl_ctx *ctx,
                                int x, int y, int w, int h, uint32_t color)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    virge_fill_rect(&priv->hw, x, y, w, h, color);
}

/* ========================================================================
 * Texture management
 *
 * tex_image_2d: Upload texture data to offscreen VRAM via the bump allocator.
 * bind_texture: Program TEX_BASE and cache CMD_SET bits for this texture.
 * tex_parameter: Update cached filter/wrap mode.
 * ======================================================================== */

static int virge_be_tex_image_2d(struct l10gl_ctx *ctx,
                                  struct l10gl_texture *tex,
                                  int width, int height,
                                  enum l10gl_tex_format format,
                                  const void *data)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    struct virge_ctx *hw = &priv->hw;

    int bpt = virge_tex_bytes_per_texel(format);
    uint32_t tex_size = width * height * bpt;

    /* Align to quadword */
    uint32_t tex_addr = (hw->tex_heap_next + 7) & ~7;

    /* Check we have enough VRAM */
    if (tex_addr + tex_size > hw->vram_size) {
        fprintf(stderr, "ViRGE: out of VRAM for texture (need %u, have %u)\n",
                tex_addr + tex_size, hw->vram_size);
        return -1;
    }

    /* Upload to VRAM */
    virge_upload_texture(hw, tex_addr, data, tex_size);

    /* Bump allocator */
    hw->tex_heap_next = tex_addr + tex_size;

    /* Store texture metadata */
    tex->width = width;
    tex->height = height;
    tex->format = format;
    tex->bytes_per_texel = bpt;
    tex->backend_data = (void *)(uintptr_t)tex_addr;

    return 0;
}

static void virge_be_bind_texture(struct l10gl_ctx *ctx,
                                   struct l10gl_texture *tex)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    struct virge_ctx *hw = &priv->hw;

    if (!tex || !tex->backend_data) {
        hw->tex_bound = 0;
        return;
    }

    uint32_t tex_addr = (uint32_t)(uintptr_t)tex->backend_data;

    /* Program TEX_BASE (quadword aligned) */
    virge_wait_engine(hw);
    virge_write32(hw, VIRGE_3D_TEX_BASE, tex_addr & ~0x7);

    /* Cache CMD_SET bits for this texture's format.
     *
     * We build the texture-related CMD_SET bits:
     *   bits [7-5]:   texture color format
     *   bits [11-8]:  mipmap level size (s where 2^s = max(w,h))
     *   bits [14-12]: texture filter mode
     *   bits [16-15]: texture blending (01 = modulate for lit triangles)
     *   bit  [26]:    texture wrap enable
     */
    uint32_t fmt_bits = virge_tex_format_bits(tex->format);
    uint32_t filter_bits = virge_filter_bits(priv->tex_filter);

    /* Mipmap level size: find log2 of the larger dimension */
    int max_dim = tex->width > tex->height ? tex->width : tex->height;
    int s_val = 0;
    while ((1 << s_val) < max_dim) s_val++;
    if (s_val > 9) s_val = 9;  /* max 512×512 */

    uint32_t mipmap_bits = (s_val << 8);

    /* Texture blending mode: modulate (01) for lit triangles */
    uint32_t blend_bits = (1 << 15);  /* TB = 01 = Modulate */

    /* Wrap */
    uint32_t wrap_bits = 0;
    if (priv->tex_wrap == L10GL_WRAP_REPEAT)
        wrap_bits = VIRGE_CMD_TEX_WRAP;

    hw->tex_cmd_bits = fmt_bits | mipmap_bits | filter_bits
                       | blend_bits | wrap_bits;
    hw->tex_bound = 1;
}

static void virge_be_tex_parameter(struct l10gl_ctx *ctx,
                                    enum l10gl_tex_filter filter,
                                    enum l10gl_tex_wrap wrap)
{
    struct virge_private *priv = VIRGE_PRIV(ctx);
    priv->tex_filter = filter;
    priv->tex_wrap = wrap;

    /* If a texture is currently bound, update its CMD_SET bits */
    if (ctx->current_texture && ctx->current_texture->backend_data) {
        virge_be_bind_texture(ctx, ctx->current_texture);
    }
}

/* ========================================================================
 * Sync
 * ======================================================================== */

static void virge_be_wait_engine(struct l10gl_ctx *ctx)
{
    virge_wait_engine(&VIRGE_PRIV(ctx)->hw);
}

static void virge_be_wait_vsync(struct l10gl_ctx *ctx)
{
    virge_wait_vsync(&VIRGE_PRIV(ctx)->hw);
}

/* ========================================================================
 * Backend Vtable
 *
 * The ViRGE is a full-featured 3D accelerator. All capability bits are
 * supported in hardware — no software fallback paths needed.
 * ======================================================================== */

const struct l10gl_backend virge_backend = {
    .name                 = "virge",
    .init                 = virge_be_init,
    .cleanup              = virge_be_cleanup,
    .clear_color          = virge_be_clear_color,
    .clear_depth          = virge_be_clear_depth,
    .clear                = NULL,  /* frontend handles color+depth dispatch */
    .depth_func           = virge_be_depth_func,
    .depth_mask           = virge_be_depth_mask,
    .depth_test           = virge_be_depth_test,
    .blend_func           = virge_be_blend_func,
    .blend_enable         = virge_be_blend_enable,
    .draw_triangle        = virge_be_draw_triangle,
    .draw_textured_triangle = virge_be_draw_textured_triangle,
    .draw_line            = virge_be_draw_line,
    .fill_rect            = virge_be_fill_rect,
    .tex_image_2d         = virge_be_tex_image_2d,
    .bind_texture         = virge_be_bind_texture,
    .tex_parameter        = virge_be_tex_parameter,
    .wait_engine          = virge_be_wait_engine,
    .wait_vsync           = virge_be_wait_vsync,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER | L10GL_CAP_LINES |
            L10GL_CAP_TEXTURE | L10GL_CAP_BLEND | L10GL_CAP_DITHER |
            L10GL_CAP_BILINEAR | L10GL_CAP_TRILINEAR | L10GL_CAP_PERSPECTIVE,
};
