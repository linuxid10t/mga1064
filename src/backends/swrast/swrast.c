/*
 * swrast.c - Correctness-first software reference backend for L10GL.
 *
 * This backend deliberately favors straightforward floating-point math over
 * speed.  It is the executable reference for frontend geometry work and can
 * render without graphics hardware into a malloc'd buffer.  Set
 * L10GL_SWRAST_DUMP=frame%04d.ppm to make swap_buffers write PPM frames, or
 * L10GL_SWRAST_FB=/dev/fb0 to render into an existing fbdev mode.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fb.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../../fbdev.h"
#include "../../l10gl.h"

struct swrast_color {
    float r, g, b, a;
};

struct swrast_texture {
    struct swrast_texture *next;
    enum l10gl_tex_format format;
    int width;
    int height;
    int bytes_per_texel;
    uint8_t pixels[];
};

struct swrast_private {
    uint8_t *color;          /* current private render target */
    uint8_t *front;          /* last completed/offscreen or visible fb buffer */
    uint8_t *owned_color[2];
    size_t color_size;
    float *depth;
    size_t stride;

    void *fb_map;
    size_t fb_map_len;
    int fb_fd;
    int fb_pan;
    int vsync_state;        /* 0 unknown, 1 supported, -1 unavailable */
    uint32_t front_yoffset;
    uint32_t back_yoffset;
    struct fb_var_screeninfo pan_var;

    struct fb_bitfield red;
    struct fb_bitfield green;
    struct fb_bitfield blue;
    struct fb_bitfield transp;

    enum l10gl_tex_filter filter;
    enum l10gl_tex_wrap wrap;
    struct swrast_texture *textures;

    char *dump_template;
    int dump_has_frame;
    unsigned int frame;
    int dump_failed;
};

static inline struct swrast_private *SWRAST_PRIV(struct l10gl_ctx *ctx)
{
    return (struct swrast_private *)ctx->backend_data;
}

static float clamp01(float value)
{
    if (!(value > 0.0f))
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

static uint32_t channel_max(const struct fb_bitfield *field)
{
    return field->length ? ((1u << field->length) - 1u) : 0u;
}

static uint32_t pack_channel(float value, const struct fb_bitfield *field)
{
    uint32_t max = channel_max(field);
    uint32_t scaled;

    if (!max)
        return 0;
    scaled = (uint32_t)(clamp01(value) * (float)max + 0.5f);
    return scaled << field->offset;
}

static float unpack_channel(uint32_t pixel, const struct fb_bitfield *field,
                            float absent)
{
    uint32_t max = channel_max(field);

    if (!max)
        return absent;
    return (float)((pixel >> field->offset) & max) / (float)max;
}

static uint32_t load_pixel_from(const struct l10gl_ctx *ctx,
                                const struct swrast_private *priv,
                                const uint8_t *color, int x, int y)
{
    const uint8_t *src = color + (size_t)y * priv->stride
                       + (size_t)x * (size_t)ctx->bpp;
    uint32_t pixel = 0;

    memcpy(&pixel, src, (size_t)ctx->bpp);
    return pixel;
}

static void store_pixel(const struct l10gl_ctx *ctx,
                        struct swrast_private *priv, int x, int y,
                        struct swrast_color color)
{
    uint8_t *dest = priv->color + (size_t)y * priv->stride
                  + (size_t)x * (size_t)ctx->bpp;
    uint32_t pixel = pack_channel(color.r, &priv->red)
                   | pack_channel(color.g, &priv->green)
                   | pack_channel(color.b, &priv->blue)
                   | pack_channel(color.a, &priv->transp);

    memcpy(dest, &pixel, (size_t)ctx->bpp);
}

static struct swrast_color read_pixel(const struct l10gl_ctx *ctx,
                                      const struct swrast_private *priv,
                                      int x, int y)
{
    uint32_t pixel = load_pixel_from(ctx, priv, priv->color, x, y);
    struct swrast_color color = {
        unpack_channel(pixel, &priv->red, 0.0f),
        unpack_channel(pixel, &priv->green, 0.0f),
        unpack_channel(pixel, &priv->blue, 0.0f),
        unpack_channel(pixel, &priv->transp, 1.0f),
    };

    return color;
}

static struct swrast_color read_pixel_from(const struct l10gl_ctx *ctx,
                                           const struct swrast_private *priv,
                                           const uint8_t *pixels,
                                           int x, int y)
{
    uint32_t pixel = load_pixel_from(ctx, priv, pixels, x, y);
    struct swrast_color color = {
        unpack_channel(pixel, &priv->red, 0.0f),
        unpack_channel(pixel, &priv->green, 0.0f),
        unpack_channel(pixel, &priv->blue, 0.0f),
        unpack_channel(pixel, &priv->transp, 1.0f),
    };

    return color;
}

static float blend_factor(enum l10gl_blend_func factor,
                          struct swrast_color src,
                          struct swrast_color dst, int channel)
{
    float src_c = channel == 0 ? src.r : channel == 1 ? src.g : src.b;
    float dst_c = channel == 0 ? dst.r : channel == 1 ? dst.g : dst.b;

    switch (factor) {
    case L10GL_ZERO:                return 0.0f;
    case L10GL_ONE:                 return 1.0f;
    case L10GL_SRC_COLOR:           return src_c;
    case L10GL_ONE_MINUS_SRC_COLOR: return 1.0f - src_c;
    case L10GL_SRC_ALPHA:           return src.a;
    case L10GL_ONE_MINUS_SRC_ALPHA: return 1.0f - src.a;
    case L10GL_DST_COLOR:           return dst_c;
    case L10GL_ONE_MINUS_DST_COLOR: return 1.0f - dst_c;
    }
    return 1.0f;
}

static struct swrast_color blend(const struct l10gl_ctx *ctx,
                                 struct swrast_color src,
                                 struct swrast_color dst)
{
    struct swrast_color out;
    float sf, df;

    sf = blend_factor(ctx->blend_sfactor, src, dst, 0);
    df = blend_factor(ctx->blend_dfactor, src, dst, 0);
    out.r = clamp01(src.r * sf + dst.r * df);
    sf = blend_factor(ctx->blend_sfactor, src, dst, 1);
    df = blend_factor(ctx->blend_dfactor, src, dst, 1);
    out.g = clamp01(src.g * sf + dst.g * df);
    sf = blend_factor(ctx->blend_sfactor, src, dst, 2);
    df = blend_factor(ctx->blend_dfactor, src, dst, 2);
    out.b = clamp01(src.b * sf + dst.b * df);
    out.a = clamp01(src.a + dst.a * (1.0f - src.a));
    return out;
}

static int depth_passes(enum l10gl_depth_func func, float src, float dst)
{
    switch (func) {
    case L10GL_NEVER:    return 0;
    case L10GL_LESS:     return src < dst;
    case L10GL_EQUAL:    return src == dst;
    case L10GL_LEQUAL:   return src <= dst;
    case L10GL_GREATER:  return src > dst;
    case L10GL_NOTEQUAL: return src != dst;
    case L10GL_GEQUAL:   return src >= dst;
    case L10GL_ALWAYS:   return 1;
    }
    return 0;
}

static void draw_fragment(struct l10gl_ctx *ctx, int x, int y, float z,
                          struct swrast_color color)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    size_t depth_index;

    if (x < 0 || y < 0 || x >= ctx->width || y >= ctx->height)
        return;

    depth_index = (size_t)y * (size_t)ctx->width + (size_t)x;
    if (ctx->depth_test_enabled &&
        !depth_passes(ctx->depth_func_val, z, priv->depth[depth_index]))
        return;

    if (ctx->blend_enabled)
        color = blend(ctx, color, read_pixel(ctx, priv, x, y));
    store_pixel(ctx, priv, x, y, color);

    /* OpenGL depth writes are inactive when the depth test is disabled. */
    if (ctx->depth_test_enabled && ctx->depth_writes_enabled)
        priv->depth[depth_index] = z;
}

static int texture_bytes_per_texel(enum l10gl_tex_format format)
{
    switch (format) {
    case L10GL_TEX_FMT_ARGB8888: return 4;
    case L10GL_TEX_FMT_RGB565:
    case L10GL_TEX_FMT_ARGB1555:
    case L10GL_TEX_FMT_ARGB4444: return 2;
    case L10GL_TEX_FMT_PAL8:     return 1;
    case L10GL_TEX_FMT_NONE:     return 0;
    }
    return 0;
}

static struct swrast_color texture_texel(const struct swrast_texture *texture,
                                         int x, int y)
{
    const uint8_t *src = texture->pixels
                       + ((size_t)y * (size_t)texture->width + (size_t)x)
                       * (size_t)texture->bytes_per_texel;
    struct swrast_color color = { 0.0f, 0.0f, 0.0f, 1.0f };
    uint32_t pixel = 0;

    memcpy(&pixel, src, (size_t)texture->bytes_per_texel);
    switch (texture->format) {
    case L10GL_TEX_FMT_ARGB8888:
        color.a = (float)((pixel >> 24) & 0xff) / 255.0f;
        color.r = (float)((pixel >> 16) & 0xff) / 255.0f;
        color.g = (float)((pixel >> 8) & 0xff) / 255.0f;
        color.b = (float)(pixel & 0xff) / 255.0f;
        break;
    case L10GL_TEX_FMT_RGB565:
        color.r = (float)((pixel >> 11) & 0x1f) / 31.0f;
        color.g = (float)((pixel >> 5) & 0x3f) / 63.0f;
        color.b = (float)(pixel & 0x1f) / 31.0f;
        break;
    case L10GL_TEX_FMT_ARGB1555:
        color.a = (pixel & 0x8000) ? 1.0f : 0.0f;
        color.r = (float)((pixel >> 10) & 0x1f) / 31.0f;
        color.g = (float)((pixel >> 5) & 0x1f) / 31.0f;
        color.b = (float)(pixel & 0x1f) / 31.0f;
        break;
    case L10GL_TEX_FMT_ARGB4444:
        color.a = (float)((pixel >> 12) & 0xf) / 15.0f;
        color.r = (float)((pixel >> 8) & 0xf) / 15.0f;
        color.g = (float)((pixel >> 4) & 0xf) / 15.0f;
        color.b = (float)(pixel & 0xf) / 15.0f;
        break;
    case L10GL_TEX_FMT_PAL8:
        /* No palette object exists in the current API; use a deterministic
         * grayscale interpretation so PAL8 remains usable by the reference. */
        color.r = color.g = color.b = (float)(pixel & 0xff) / 255.0f;
        break;
    case L10GL_TEX_FMT_NONE:
        break;
    }
    return color;
}

static int wrap_index(int value, int size, enum l10gl_tex_wrap wrap)
{
    if (wrap == L10GL_WRAP_REPEAT) {
        value %= size;
        return value < 0 ? value + size : value;
    }
    if (value < 0)
        return 0;
    if (value >= size)
        return size - 1;
    return value;
}

static struct swrast_color texture_sample(const struct swrast_private *priv,
                                          const struct swrast_texture *texture,
                                          float u, float v)
{
    int linear = priv->filter == L10GL_FILTER_LINEAR ||
                 priv->filter == L10GL_FILTER_LINEAR_MIPMAP_NEAREST ||
                 priv->filter == L10GL_FILTER_LINEAR_MIPMAP_LINEAR;

    if (!linear) {
        int x = wrap_index((int)floorf(u * texture->width), texture->width,
                           priv->wrap);
        int y = wrap_index((int)floorf(v * texture->height), texture->height,
                           priv->wrap);
        return texture_texel(texture, x, y);
    } else {
        float tx = u * texture->width - 0.5f;
        float ty = v * texture->height - 0.5f;
        int x0 = (int)floorf(tx);
        int y0 = (int)floorf(ty);
        float fx = tx - floorf(tx);
        float fy = ty - floorf(ty);
        struct swrast_color c00, c10, c01, c11, result;

        c00 = texture_texel(texture,
                            wrap_index(x0, texture->width, priv->wrap),
                            wrap_index(y0, texture->height, priv->wrap));
        c10 = texture_texel(texture,
                            wrap_index(x0 + 1, texture->width, priv->wrap),
                            wrap_index(y0, texture->height, priv->wrap));
        c01 = texture_texel(texture,
                            wrap_index(x0, texture->width, priv->wrap),
                            wrap_index(y0 + 1, texture->height, priv->wrap));
        c11 = texture_texel(texture,
                            wrap_index(x0 + 1, texture->width, priv->wrap),
                            wrap_index(y0 + 1, texture->height, priv->wrap));

#define BILERP(member) \
        ((c00.member * (1.0f - fx) + c10.member * fx) * (1.0f - fy) + \
         (c01.member * (1.0f - fx) + c11.member * fx) * fy)
        result.r = BILERP(r);
        result.g = BILERP(g);
        result.b = BILERP(b);
        result.a = BILERP(a);
#undef BILERP
        return result;
    }
}

static double orient2d(const struct l10gl_vertex *a,
                       const struct l10gl_vertex *b, double x, double y)
{
    return ((double)b->x - a->x) * (y - a->y)
         - ((double)b->y - a->y) * (x - a->x);
}

/* With y increasing down-screen and clockwise triangles normalized to
 * positive area, inclusive top/left edges travel upward or horizontally
 * right.  The opposite direction is exclusive, so shared edges have exactly
 * one owner regardless of primitive order. */
static int is_top_left(const struct l10gl_vertex *a,
                       const struct l10gl_vertex *b)
{
    float dy = b->y - a->y;
    float dx = b->x - a->x;
    return dy < 0.0f || (dy == 0.0f && dx > 0.0f);
}

static int edge_inside(double edge, int top_left)
{
    return edge > 0.0 || (edge == 0.0 && top_left);
}

static void swrast_draw_triangle_common(struct l10gl_ctx *ctx,
                                        struct l10gl_vertex v0,
                                        struct l10gl_vertex v1,
                                        struct l10gl_vertex v2,
                                        int textured)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    const struct swrast_texture *texture = NULL;
    double area = orient2d(&v0, &v1, v2.x, v2.y);
    int min_x, max_x, min_y, max_y;
    int tl0, tl1, tl2;

    if (area == 0.0)
        return;
    if (area < 0.0) {
        struct l10gl_vertex tmp = v1;
        v1 = v2;
        v2 = tmp;
        area = -area;
    }

    if (textured && ctx->current_texture)
        texture = (const struct swrast_texture *)ctx->current_texture->backend_data;
    if (textured && !texture)
        textured = 0;

    min_x = (int)floorf(fminf(v0.x, fminf(v1.x, v2.x)));
    max_x = (int)ceilf(fmaxf(v0.x, fmaxf(v1.x, v2.x))) - 1;
    min_y = (int)floorf(fminf(v0.y, fminf(v1.y, v2.y)));
    max_y = (int)ceilf(fmaxf(v0.y, fmaxf(v1.y, v2.y))) - 1;
    if (min_x < 0) min_x = 0;
    if (min_y < 0) min_y = 0;
    if (max_x >= ctx->width) max_x = ctx->width - 1;
    if (max_y >= ctx->height) max_y = ctx->height - 1;

    tl0 = is_top_left(&v1, &v2);
    tl1 = is_top_left(&v2, &v0);
    tl2 = is_top_left(&v0, &v1);

    for (int y = min_y; y <= max_y; y++) {
        for (int x = min_x; x <= max_x; x++) {
            double px = x + 0.5;
            double py = y + 0.5;
            double e0 = orient2d(&v1, &v2, px, py);
            double e1 = orient2d(&v2, &v0, px, py);
            double e2 = orient2d(&v0, &v1, px, py);
            float w0, w1, w2, z;
            struct swrast_color color;

            if (!edge_inside(e0, tl0) || !edge_inside(e1, tl1) ||
                !edge_inside(e2, tl2))
                continue;

            w0 = (float)(e0 / area);
            w1 = (float)(e1 / area);
            w2 = (float)(e2 / area);
            z = w0 * v0.z + w1 * v1.z + w2 * v2.z;
            color.r = w0 * v0.r + w1 * v1.r + w2 * v2.r;
            color.g = w0 * v0.g + w1 * v1.g + w2 * v2.g;
            color.b = w0 * v0.b + w1 * v1.b + w2 * v2.b;
            color.a = w0 * v0.a + w1 * v1.a + w2 * v2.a;

            if (textured) {
                float rw0 = v0.w != 0.0f ? v0.w : 1.0f;
                float rw1 = v1.w != 0.0f ? v1.w : 1.0f;
                float rw2 = v2.w != 0.0f ? v2.w : 1.0f;
                float denom = w0 * rw0 + w1 * rw1 + w2 * rw2;
                float u, v;
                struct swrast_color texel;

                if (fabsf(denom) < 1.0e-20f)
                    continue;
                u = (w0 * v0.u * rw0 + w1 * v1.u * rw1 +
                     w2 * v2.u * rw2) / denom;
                v = (w0 * v0.v * rw0 + w1 * v1.v * rw1 +
                     w2 * v2.v * rw2) / denom;
                texel = texture_sample(priv, texture, u, v);
                color.r *= texel.r;
                color.g *= texel.g;
                color.b *= texel.b;
                color.a *= texel.a;
            }

            draw_fragment(ctx, x, y, z, color);
        }
    }
}

static int validate_dump_template(const char *text, int *has_frame)
{
    int conversions = 0;

    for (size_t i = 0; text[i]; i++) {
        if (text[i] != '%')
            continue;
        i++;
        if (text[i] == '%')
            continue;
        if (text[i] == '0') {
            i++;
            if (text[i] < '1' || text[i] > '9')
                return -EINVAL;
            while (text[i] >= '0' && text[i] <= '9')
                i++;
        }
        if (text[i] != 'd' || ++conversions > 1)
            return -EINVAL;
    }
    *has_frame = conversions != 0;
    return 0;
}

static int swrast_dump_frame(struct l10gl_ctx *ctx, const uint8_t *pixels)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    char path[PATH_MAX];
    uint8_t *row;
    FILE *file;
    int length;

    if (!priv->dump_template || priv->dump_failed)
        return 0;
    if (priv->dump_has_frame)
        length = snprintf(path, sizeof(path), priv->dump_template, priv->frame);
    else
        length = snprintf(path, sizeof(path), "%s", priv->dump_template);
    if (length < 0 || (size_t)length >= sizeof(path)) {
        fprintf(stderr, "swrast: dump path is too long\n");
        priv->dump_failed = 1;
        return -ENAMETOOLONG;
    }

    file = fopen(path, "wb");
    if (!file) {
        int error = errno;
        fprintf(stderr, "swrast: cannot write '%s': %s\n",
                path, strerror(error));
        priv->dump_failed = 1;
        return -error;
    }
    row = malloc((size_t)ctx->width * 3u);
    if (!row) {
        fclose(file);
        priv->dump_failed = 1;
        return -ENOMEM;
    }

    fprintf(file, "P6\n%d %d\n255\n", ctx->width, ctx->height);
    for (int y = 0; y < ctx->height; y++) {
        for (int x = 0; x < ctx->width; x++) {
            struct swrast_color color = read_pixel_from(ctx, priv, pixels,
                                                        x, y);
            row[x * 3 + 0] = (uint8_t)(clamp01(color.r) * 255.0f + 0.5f);
            row[x * 3 + 1] = (uint8_t)(clamp01(color.g) * 255.0f + 0.5f);
            row[x * 3 + 2] = (uint8_t)(clamp01(color.b) * 255.0f + 0.5f);
        }
        if (fwrite(row, (size_t)ctx->width * 3u, 1, file) != 1) {
            fprintf(stderr, "swrast: write failed for '%s'\n", path);
            priv->dump_failed = 1;
            break;
        }
    }
    free(row);
    if (fclose(file) != 0)
        priv->dump_failed = 1;
    if (!priv->dump_failed && (priv->frame == 0 || priv->frame % 60 == 0))
        printf("swrast: dumped frame %u to %s\n", priv->frame, path);
    priv->frame++;
    return priv->dump_failed ? -EIO : 0;
}

static void set_offscreen_format(struct swrast_private *priv, int bpp)
{
    if (bpp == 2) {
        priv->red = (struct fb_bitfield){ .offset = 11, .length = 5 };
        priv->green = (struct fb_bitfield){ .offset = 5, .length = 6 };
        priv->blue = (struct fb_bitfield){ .offset = 0, .length = 5 };
    } else {
        priv->red = (struct fb_bitfield){ .offset = 16, .length = 8 };
        priv->green = (struct fb_bitfield){ .offset = 8, .length = 8 };
        priv->blue = (struct fb_bitfield){ .offset = 0, .length = 8 };
    }
}

static int init_offscreen(struct l10gl_ctx *ctx, struct swrast_private *priv,
                          int width, int height, int bpp)
{
    size_t color_size;

    if (width <= 0 || height <= 0 || (bpp != 2 && bpp != 3)) {
        fprintf(stderr, "swrast: offscreen output requires positive geometry "
                        "and 16bpp or 24bpp\n");
        return -EINVAL;
    }
    if ((size_t)width > SIZE_MAX / (size_t)bpp ||
        (size_t)height > SIZE_MAX / ((size_t)width * (size_t)bpp) ||
        (size_t)height > SIZE_MAX / ((size_t)width * sizeof(float)))
        return -EOVERFLOW;

    priv->stride = (size_t)width * (size_t)bpp;
    color_size = priv->stride * (size_t)height;
    priv->owned_color[0] = calloc(1, color_size);
    priv->owned_color[1] = calloc(1, color_size);
    if (!priv->owned_color[0] || !priv->owned_color[1])
        return -ENOMEM;
    priv->color = priv->owned_color[0];
    priv->front = priv->owned_color[1];
    priv->color_size = color_size;
    set_offscreen_format(priv, bpp);
    l10gl_mode_set_linear(ctx, width, height, bpp * 8,
                          (uint32_t)priv->stride, NULL);
    printf("swrast: using double-buffered offscreen %dx%d %dbpp buffer\n",
           width, height, bpp * 8);
    return 0;
}

static int prepare_fbdev_pan(struct swrast_private *priv,
                             struct l10gl_fbdev_mode *mode,
                             uint32_t *front_yoffset,
                             uint32_t *back_yoffset)
{
    struct fb_var_screeninfo requested;
    uint64_t virtual_height;
    int ret;

    if (l10gl_fbdev_find_pan_pages(mode, front_yoffset, back_yoffset))
        return 1;

    virtual_height = (uint64_t)mode->var.yres * 2u;
    if (virtual_height > UINT32_MAX)
        return 0;
    requested = mode->var;
    requested.yres_virtual = (uint32_t)virtual_height;
    requested.yoffset = 0;
    requested.activate = FB_ACTIVATE_NOW;
    if (ioctl(priv->fb_fd, FBIOPUT_VSCREENINFO, &requested) < 0)
        return 0;

    ret = l10gl_fbdev_read_mode(priv->fb_fd, "swrast", mode);
    if (ret)
        return ret;
    return l10gl_fbdev_find_pan_pages(mode, front_yoffset, back_yoffset);
}

static int init_fbdev(struct l10gl_ctx *ctx, struct swrast_private *priv,
                      const char *path, int width, int height, int bpp)
{
    struct l10gl_fbdev_mode mode;
    const struct fb_fix_screeninfo *fix = &mode.fix;
    const struct fb_var_screeninfo *var = &mode.var;
    uint32_t front_yoffset = 0, back_yoffset = 0;
    size_t front_offset, back_offset = 0;
    int pan;
    int ret;

    priv->fb_fd = open(path, O_RDWR);
    if (priv->fb_fd < 0)
        return -errno;
    ret = l10gl_fbdev_negotiate(priv->fb_fd, "swrast", width, height,
                                bpp * 8, NULL, &mode);
    if (ret)
        return ret;
    if (var->bits_per_pixel != 16 && var->bits_per_pixel != 24 &&
        var->bits_per_pixel != 32) {
        fprintf(stderr, "swrast: fbdev requires packed true/direct color "
                        "at 16, 24, or 32bpp\n");
        return -ENOTSUP;
    }

    pan = prepare_fbdev_pan(priv, &mode, &front_yoffset, &back_yoffset);
    if (pan < 0)
        return pan;
    if (var->bits_per_pixel != 16 && var->bits_per_pixel != 24 &&
        var->bits_per_pixel != 32) {
        fprintf(stderr, "swrast: fbdev virtual-page request changed to an "
                        "unsupported %ubpp mode\n", var->bits_per_pixel);
        return -ENOTSUP;
    }

    priv->fb_map_len = fix->smem_len;
    priv->fb_map = mmap(NULL, priv->fb_map_len, PROT_READ | PROT_WRITE,
                        MAP_SHARED, priv->fb_fd, 0);
    if (priv->fb_map == MAP_FAILED) {
        priv->fb_map = NULL;
        return -errno;
    }
    front_offset = (size_t)front_yoffset * fix->line_length
                 + (size_t)var->xoffset
                 * ((var->bits_per_pixel + 7u) / 8u);
    if (pan)
        back_offset = (size_t)back_yoffset * fix->line_length
                    + (size_t)var->xoffset
                    * ((var->bits_per_pixel + 7u) / 8u);
    if (front_offset >= priv->fb_map_len ||
        (size_t)var->yres >
            (priv->fb_map_len - front_offset) / fix->line_length ||
        (pan && (back_offset >= priv->fb_map_len ||
         (size_t)var->yres >
            (priv->fb_map_len - back_offset) / fix->line_length))) {
        fprintf(stderr, "swrast: visible fbdev raster exceeds its mapping\n");
        return -EOVERFLOW;
    }

    priv->stride = fix->line_length;
    priv->red = var->red;
    priv->green = var->green;
    priv->blue = var->blue;
    priv->transp = var->transp;
    l10gl_mode_from_fbdev(ctx, &mode);
    priv->color_size = priv->stride * (size_t)ctx->height;
    priv->front = (uint8_t *)priv->fb_map + front_offset;
    if (pan) {
        priv->color = (uint8_t *)priv->fb_map + back_offset;
        priv->fb_pan = 1;
        priv->front_yoffset = front_yoffset;
        priv->back_yoffset = back_yoffset;
        priv->pan_var = *var;
        printf("swrast: using %s current mode %dx%d %dbpp, stride %zu "
               "(fbdev page flip y=%u/%u)\n",
               path, ctx->width, ctx->height, ctx->bpp * 8, priv->stride,
               front_yoffset, back_yoffset);
    } else {
        priv->color = priv->front;
        printf("swrast: using %s current mode %dx%d %dbpp, stride %zu "
               "(direct single buffer; fbdev panning unavailable)\n",
               path, ctx->width, ctx->height, ctx->bpp * 8, priv->stride);
    }
    return 0;
}

static int swrast_probe(void)
{
    /* Always available; registry order keeps this behind real hardware. */
    return 1;
}

static int swrast_init(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    struct swrast_private *priv = calloc(1, sizeof(*priv));
    const char *fb_path = getenv("L10GL_SWRAST_FB");
    const char *dump = getenv("L10GL_SWRAST_DUMP");
    int ret;

    if (!priv)
        return -ENOMEM;
    priv->fb_fd = -1;
    priv->filter = L10GL_FILTER_NEAREST;
    priv->wrap = L10GL_WRAP_REPEAT;
    ctx->backend_data = priv;

    ret = fb_path && fb_path[0]
        ? init_fbdev(ctx, priv, fb_path, width, height, bpp)
        : init_offscreen(ctx, priv, width, height, bpp);
    if (ret)
        goto fail;

    if ((size_t)ctx->height > SIZE_MAX /
        ((size_t)ctx->width * sizeof(*priv->depth))) {
        ret = -EOVERFLOW;
        goto fail;
    }
    priv->depth = malloc((size_t)ctx->width * (size_t)ctx->height
                         * sizeof(*priv->depth));
    if (!priv->depth) {
        ret = -ENOMEM;
        goto fail;
    }

    if (dump && dump[0]) {
        ret = validate_dump_template(dump, &priv->dump_has_frame);
        if (ret) {
            fprintf(stderr, "swrast: L10GL_SWRAST_DUMP accepts one %%d or "
                            "%%0Nd conversion\n");
            goto fail;
        }
        priv->dump_template = strdup(dump);
        if (!priv->dump_template) {
            ret = -ENOMEM;
            goto fail;
        }
    } else if (!fb_path || !fb_path[0]) {
        printf("swrast: offscreen output is not visible; set "
               "L10GL_SWRAST_DUMP to write PPM frames\n");
    }
    return 0;

fail:
    if (priv->fb_map)
        munmap(priv->fb_map, priv->fb_map_len);
    free(priv->owned_color[0]);
    free(priv->owned_color[1]);
    if (priv->fb_fd >= 0)
        close(priv->fb_fd);
    free(priv->depth);
    free(priv->dump_template);
    free(priv);
    ctx->backend_data = NULL;
    return ret;
}

static void swrast_cleanup(struct l10gl_ctx *ctx)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    struct swrast_texture *texture, *next;

    if (!priv)
        return;
    if (priv->dump_template && priv->frame == 0)
        swrast_dump_frame(ctx, priv->color);
    for (texture = priv->textures; texture; texture = next) {
        next = texture->next;
        free(texture);
    }
    if (priv->fb_map)
        munmap(priv->fb_map, priv->fb_map_len);
    free(priv->owned_color[0]);
    free(priv->owned_color[1]);
    if (priv->fb_fd >= 0)
        close(priv->fb_fd);
    free(priv->depth);
    free(priv->dump_template);
    free(priv);
    ctx->backend_data = NULL;
}

static void swrast_clear_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    struct swrast_color color = { r, g, b, 1.0f };

    for (int y = 0; y < ctx->height; y++)
        for (int x = 0; x < ctx->width; x++)
            store_pixel(ctx, priv, x, y, color);
}

static void swrast_clear_depth(struct l10gl_ctx *ctx, float z)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    size_t count = (size_t)ctx->width * (size_t)ctx->height;

    for (size_t i = 0; i < count; i++)
        priv->depth[i] = z;
}

static void swrast_state_noop(struct l10gl_ctx *ctx, int value)
{
    (void)ctx;
    (void)value;
}

static void swrast_depth_func(struct l10gl_ctx *ctx,
                              enum l10gl_depth_func func)
{
    (void)ctx;
    (void)func;
}

static void swrast_blend_func(struct l10gl_ctx *ctx,
                              enum l10gl_blend_func src,
                              enum l10gl_blend_func dst)
{
    (void)ctx;
    (void)src;
    (void)dst;
}

static void swrast_draw_triangle(struct l10gl_ctx *ctx,
                                 struct l10gl_vertex v0,
                                 struct l10gl_vertex v1,
                                 struct l10gl_vertex v2)
{
    swrast_draw_triangle_common(ctx, v0, v1, v2, 0);
}

static void swrast_draw_textured_triangle(struct l10gl_ctx *ctx,
                                          struct l10gl_vertex v0,
                                          struct l10gl_vertex v1,
                                          struct l10gl_vertex v2)
{
    swrast_draw_triangle_common(ctx, v0, v1, v2, 1);
}

static void swrast_draw_line(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1)
{
    float dx = v1.x - v0.x;
    float dy = v1.y - v0.y;
    int steps = (int)ceilf(fmaxf(fabsf(dx), fabsf(dy)));

    if (steps == 0) {
        struct swrast_color color = { v0.r, v0.g, v0.b, v0.a };
        draw_fragment(ctx, (int)floorf(v0.x + 0.5f),
                      (int)floorf(v0.y + 0.5f), v0.z, color);
        return;
    }
    for (int i = 0; i <= steps; i++) {
        float t = (float)i / (float)steps;
        struct swrast_color color = {
            v0.r + (v1.r - v0.r) * t,
            v0.g + (v1.g - v0.g) * t,
            v0.b + (v1.b - v0.b) * t,
            v0.a + (v1.a - v0.a) * t,
        };
        draw_fragment(ctx,
                      (int)floorf(v0.x + dx * t + 0.5f),
                      (int)floorf(v0.y + dy * t + 0.5f),
                      v0.z + (v1.z - v0.z) * t, color);
    }
}

static void swrast_fill_rect(struct l10gl_ctx *ctx,
                             int x, int y, int width, int height,
                             uint32_t color)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    long long x1 = (long long)x + width;
    long long y1 = (long long)y + height;
    int left, top, right, bottom;

    if (width <= 0 || height <= 0)
        return;
    left = x < 0 ? 0 : x;
    top = y < 0 ? 0 : y;
    right = x1 > ctx->width ? ctx->width : (int)x1;
    bottom = y1 > ctx->height ? ctx->height : (int)y1;
    for (int py = top; py < bottom; py++) {
        for (int px = left; px < right; px++) {
            uint8_t *dest = priv->color + (size_t)py * priv->stride
                          + (size_t)px * (size_t)ctx->bpp;
            memcpy(dest, &color, (size_t)ctx->bpp);
        }
    }
}

static int swrast_tex_image_2d(struct l10gl_ctx *ctx,
                               struct l10gl_texture *tex,
                               int width, int height,
                               enum l10gl_tex_format format,
                               const void *data)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    struct swrast_texture *texture;
    int bytes_per_texel = texture_bytes_per_texel(format);
    size_t size;

    if (!tex || !data || width <= 0 || height <= 0 || !bytes_per_texel)
        return -EINVAL;
    if ((size_t)width > SIZE_MAX / (size_t)height ||
        (size_t)width * (size_t)height > SIZE_MAX / (size_t)bytes_per_texel)
        return -EOVERFLOW;
    size = (size_t)width * (size_t)height * (size_t)bytes_per_texel;
    if (size > SIZE_MAX - sizeof(*texture))
        return -EOVERFLOW;

    texture = malloc(sizeof(*texture) + size);
    if (!texture)
        return -ENOMEM;
    texture->next = priv->textures;
    texture->format = format;
    texture->width = width;
    texture->height = height;
    texture->bytes_per_texel = bytes_per_texel;
    memcpy(texture->pixels, data, size);
    priv->textures = texture;

    tex->width = width;
    tex->height = height;
    tex->format = format;
    tex->bytes_per_texel = bytes_per_texel;
    tex->backend_data = texture;
    return 0;
}

static void swrast_bind_texture(struct l10gl_ctx *ctx,
                                struct l10gl_texture *texture)
{
    (void)ctx;
    (void)texture;
}

static void swrast_tex_parameter(struct l10gl_ctx *ctx,
                                 enum l10gl_tex_filter filter,
                                 enum l10gl_tex_wrap wrap)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);

    priv->filter = filter;
    priv->wrap = wrap;
}

static void swrast_wait(struct l10gl_ctx *ctx)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    unsigned int crtc = 0;

    if (priv->fb_fd < 0 || priv->vsync_state < 0)
        return;
    if (ioctl(priv->fb_fd, FBIO_WAITFORVSYNC, &crtc) == 0) {
        priv->vsync_state = 1;
        return;
    }
    if (priv->vsync_state == 0)
        fprintf(stderr, "swrast: fbdev vsync wait unavailable: %s; "
                        "publishing without synchronization\n",
                strerror(errno));
    priv->vsync_state = -1;
}

static void swrast_swap_buffers(struct l10gl_ctx *ctx)
{
    struct swrast_private *priv = SWRAST_PRIV(ctx);
    const uint8_t *completed = priv->color;

    if (priv->fb_pan) {
        struct fb_var_screeninfo pan = priv->pan_var;
        uint8_t *old_front = priv->front;
        uint32_t old_front_yoffset = priv->front_yoffset;

        pan.yoffset = priv->back_yoffset;
        pan.activate = FB_ACTIVATE_VBL;
        if (ioctl(priv->fb_fd, FBIOPAN_DISPLAY, &pan) == 0) {
            priv->front = priv->color;
            priv->color = old_front;
            priv->front_yoffset = priv->back_yoffset;
            priv->back_yoffset = old_front_yoffset;
            priv->pan_var.yoffset = priv->front_yoffset;
        } else {
            fprintf(stderr, "swrast: FBIOPAN_DISPLAY failed: %s; "
                            "falling back to direct single buffering\n",
                    strerror(errno));
            priv->fb_pan = 0;
            priv->color = priv->front;
        }
    } else if (!priv->fb_map) {
        priv->front = priv->color;
        priv->color = priv->color == priv->owned_color[0]
                    ? priv->owned_color[1] : priv->owned_color[0];
    }
    swrast_dump_frame(ctx, completed);
}

const struct l10gl_backend swrast_backend = {
    .name                   = "swrast",
    .fbdev_env              = "L10GL_SWRAST_FB",
    .probe                  = swrast_probe,
    .init                   = swrast_init,
    .cleanup                = swrast_cleanup,
    .clear_color            = swrast_clear_color,
    .clear_depth            = swrast_clear_depth,
    .clear                  = NULL,
    .depth_func             = swrast_depth_func,
    .depth_mask             = swrast_state_noop,
    .depth_test             = swrast_state_noop,
    .blend_func             = swrast_blend_func,
    .blend_enable           = swrast_state_noop,
    .draw_triangle          = swrast_draw_triangle,
    .draw_textured_triangle = swrast_draw_textured_triangle,
    .draw_line              = swrast_draw_line,
    .fill_rect              = swrast_fill_rect,
    .tex_image_2d           = swrast_tex_image_2d,
    .bind_texture           = swrast_bind_texture,
    .tex_parameter          = swrast_tex_parameter,
    .wait_engine            = swrast_wait,
    .wait_vsync             = swrast_wait,
    .swap_buffers           = swrast_swap_buffers,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER | L10GL_CAP_LINES |
            L10GL_CAP_TEXTURE | L10GL_CAP_BLEND | L10GL_CAP_BILINEAR |
            L10GL_CAP_PERSPECTIVE,
};
