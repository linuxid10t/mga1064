#include <errno.h>
#include <linux/fb.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>

#include "fbdev.h"

static struct l10gl_color_channel channel_from_fb(struct fb_bitfield field)
{
    return (struct l10gl_color_channel) {
        .offset = (uint8_t)field.offset,
        .length = (uint8_t)field.length,
        .msb_right = (uint8_t)field.msb_right,
    };
}

static struct fb_bitfield channel_to_fb(struct l10gl_color_channel channel)
{
    return (struct fb_bitfield) {
        .offset = channel.offset,
        .length = channel.length,
        .msb_right = channel.msb_right,
    };
}

int l10gl_pixel_format_standard(int bits_per_pixel,
                                struct l10gl_pixel_format *format)
{
    if (!format)
        return -EINVAL;

    memset(format, 0, sizeof(*format));
    format->bits_per_pixel = (uint8_t)bits_per_pixel;
    switch (bits_per_pixel) {
    case 15:
        format->red = (struct l10gl_color_channel){ 10, 5, 0 };
        format->green = (struct l10gl_color_channel){ 5, 5, 0 };
        format->blue = (struct l10gl_color_channel){ 0, 5, 0 };
        return 0;
    case 16:
        format->red = (struct l10gl_color_channel){ 11, 5, 0 };
        format->green = (struct l10gl_color_channel){ 5, 6, 0 };
        format->blue = (struct l10gl_color_channel){ 0, 5, 0 };
        return 0;
    case 24:
        format->red = (struct l10gl_color_channel){ 16, 8, 0 };
        format->green = (struct l10gl_color_channel){ 8, 8, 0 };
        format->blue = (struct l10gl_color_channel){ 0, 8, 0 };
        return 0;
    case 32:
        format->red = (struct l10gl_color_channel){ 16, 8, 0 };
        format->green = (struct l10gl_color_channel){ 8, 8, 0 };
        format->blue = (struct l10gl_color_channel){ 0, 8, 0 };
        return 0;
    default:
        memset(format, 0, sizeof(*format));
        return -ENOTSUP;
    }
}

static int channel_matches(struct fb_bitfield actual,
                           struct l10gl_color_channel required)
{
    if (!required.length)
        return actual.length == 0;
    return actual.offset == required.offset &&
           actual.length == required.length &&
           actual.msb_right == required.msb_right;
}

int l10gl_fbdev_mode_matches(const struct l10gl_fbdev_mode *mode,
                             int width, int height, int bits_per_pixel,
                             const struct l10gl_pixel_format *required_format)
{
    const struct fb_var_screeninfo *var;

    if (!mode)
        return 0;
    var = &mode->var;
    if (var->xres != (uint32_t)width || var->yres != (uint32_t)height ||
        var->bits_per_pixel != (uint32_t)bits_per_pixel ||
        var->xoffset != 0 || var->yoffset != 0)
        return 0;
    if (!required_format)
        return 1;
    return required_format->bits_per_pixel == bits_per_pixel &&
           channel_matches(var->red, required_format->red) &&
           channel_matches(var->green, required_format->green) &&
           channel_matches(var->blue, required_format->blue) &&
           channel_matches(var->transp, required_format->alpha);
}

int l10gl_fbdev_find_pan_pages(const struct l10gl_fbdev_mode *mode,
                               uint32_t *front_yoffset,
                               uint32_t *back_yoffset)
{
    const struct fb_var_screeninfo *var;
    const struct fb_fix_screeninfo *fix;
    uint32_t candidates[2];
    uint64_t visible_end;
    uint64_t x_bytes;

    if (!mode || !front_yoffset || !back_yoffset)
        return 0;
    var = &mode->var;
    fix = &mode->fix;
    candidates[0] = 0;
    candidates[1] = var->yres;
    visible_end = (uint64_t)var->yoffset + var->yres;
    x_bytes = (uint64_t)var->xoffset
            * ((var->bits_per_pixel + 7u) / 8u);

    if (!var->yres || !fix->line_length || visible_end > var->yres_virtual)
        return 0;
    for (size_t i = 0; i < 2; i++) {
        uint64_t candidate_end = (uint64_t)candidates[i] + var->yres;
        uint64_t candidate_bytes = candidate_end * fix->line_length + x_bytes;

        if (candidate_end <= var->yres_virtual &&
            candidate_bytes <= fix->smem_len &&
            (candidate_end <= var->yoffset ||
             candidates[i] >= visible_end)) {
            *front_yoffset = var->yoffset;
            *back_yoffset = candidates[i];
            return 1;
        }
    }
    return 0;
}

static int validate_mode(const char *name, const struct l10gl_fbdev_mode *mode)
{
    const struct fb_fix_screeninfo *fix = &mode->fix;
    const struct fb_var_screeninfo *var = &mode->var;
    uint32_t bytes_per_pixel = (var->bits_per_pixel + 7u) / 8u;
    uint64_t minimum_stride =
        ((uint64_t)var->xoffset + var->xres) * bytes_per_pixel;
    int visual_ok = fix->visual == FB_VISUAL_TRUECOLOR ||
                    fix->visual == FB_VISUAL_DIRECTCOLOR ||
                    ((fix->visual == FB_VISUAL_PSEUDOCOLOR ||
                      fix->visual == FB_VISUAL_STATIC_PSEUDOCOLOR) &&
                     var->bits_per_pixel <= 8);

    if (fix->type != FB_TYPE_PACKED_PIXELS ||
        !visual_ok ||
        !var->xres || !var->yres || !bytes_per_pixel ||
        bytes_per_pixel > 4 || !fix->line_length || !fix->smem_len ||
        fix->line_length < minimum_stride ||
        (uint64_t)fix->line_length *
            ((uint64_t)var->yoffset + var->yres) > fix->smem_len ||
        var->red.length > 8 || var->green.length > 8 ||
        var->blue.length > 8 || var->transp.length > 8) {
        fprintf(stderr, "%s: unusable fbdev mode %ux%u @ %ubpp, "
                        "stride %u (requires a supported packed-pixel visual)\n",
                name, var->xres, var->yres, var->bits_per_pixel,
                fix->line_length);
        return -ENOTSUP;
    }
    return 0;
}

int l10gl_fbdev_read_mode(int fd, const char *name,
                          struct l10gl_fbdev_mode *mode)
{
    if (fd < 0 || !mode)
        return -EINVAL;
    memset(mode, 0, sizeof(*mode));
    if (ioctl(fd, FBIOGET_FSCREENINFO, &mode->fix) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &mode->var) < 0) {
        int error = errno;
        fprintf(stderr, "%s: cannot read fbdev mode: %s\n",
                name, strerror(error));
        return -error;
    }
    return validate_mode(name, mode);
}

static void set_requested_format(struct fb_var_screeninfo *var,
                                 int bits_per_pixel,
                                 const struct l10gl_pixel_format *required)
{
    struct l10gl_pixel_format standard;
    const struct l10gl_pixel_format *format = required;

    var->bits_per_pixel = (uint32_t)bits_per_pixel;
    if (!format && l10gl_pixel_format_standard(bits_per_pixel, &standard) == 0)
        format = &standard;
    if (!format)
        return;
    var->red = channel_to_fb(format->red);
    var->green = channel_to_fb(format->green);
    var->blue = channel_to_fb(format->blue);
    var->transp = channel_to_fb(format->alpha);
}

int l10gl_fbdev_negotiate(int fd, const char *name,
                          int width, int height, int bits_per_pixel,
                          const struct l10gl_pixel_format *required_format,
                          struct l10gl_fbdev_mode *mode)
{
    struct fb_var_screeninfo request;
    int ret;

    if (width <= 0 || height <= 0 || bits_per_pixel <= 0 ||
        bits_per_pixel > 32)
        return -EINVAL;
    ret = l10gl_fbdev_read_mode(fd, name, mode);
    if (ret)
        return ret;
    if (l10gl_fbdev_mode_matches(mode, width, height, bits_per_pixel,
                                 required_format))
        return 0;

    printf("%s: requesting %dx%d @ %dbpp from current %ux%u @ %ubpp\n",
           name, width, height, bits_per_pixel, mode->var.xres,
           mode->var.yres, mode->var.bits_per_pixel);
    request = mode->var;
    request.xres = (uint32_t)width;
    request.yres = (uint32_t)height;
    request.xres_virtual = (uint32_t)width;
    request.yres_virtual = (uint32_t)height;
    request.xoffset = 0;
    request.yoffset = 0;
    request.activate = FB_ACTIVATE_NOW;
    set_requested_format(&request, bits_per_pixel, required_format);

    if (ioctl(fd, FBIOPUT_VSCREENINFO, &request) < 0)
        ret = -errno;
    else
        ret = 0;

    /* A driver may clamp or silently ignore a request. Always re-read both
     * structures because line_length can change even when PUT succeeds. */
    if (l10gl_fbdev_read_mode(fd, name, mode) < 0)
        return ret ? ret : -EIO;
    if (l10gl_fbdev_mode_matches(mode, width, height, bits_per_pixel,
                                 required_format))
        return 0;

    fprintf(stderr,
            "%s: requested %dx%d @ %dbpp, but fbdev remains %ux%u @ %ubpp "
            "(stride %u).\n"
            "  Set a matching mode first (for example: fbset -xres %d "
            "-yres %d -depth %d), or use a framebuffer driver that "
            "supports FBIOPUT_VSCREENINFO.\n",
            name, width, height, bits_per_pixel, mode->var.xres,
            mode->var.yres, mode->var.bits_per_pixel, mode->fix.line_length,
            width, height, bits_per_pixel);
    return ret ? ret : -ENOTSUP;
}

void l10gl_mode_from_fbdev(struct l10gl_ctx *ctx,
                           const struct l10gl_fbdev_mode *mode)
{
    ctx->width = (int)mode->var.xres;
    ctx->height = (int)mode->var.yres;
    ctx->bpp = (int)((mode->var.bits_per_pixel + 7u) / 8u);
    ctx->stride = mode->fix.line_length;
    ctx->pixel_format.bits_per_pixel = (uint8_t)mode->var.bits_per_pixel;
    ctx->pixel_format.red = channel_from_fb(mode->var.red);
    ctx->pixel_format.green = channel_from_fb(mode->var.green);
    ctx->pixel_format.blue = channel_from_fb(mode->var.blue);
    ctx->pixel_format.alpha = channel_from_fb(mode->var.transp);
}

void l10gl_mode_set_linear(struct l10gl_ctx *ctx, int width, int height,
                           int bits_per_pixel, uint32_t stride,
                           const struct l10gl_pixel_format *format)
{
    ctx->width = width;
    ctx->height = height;
    ctx->bpp = (bits_per_pixel + 7) / 8;
    ctx->stride = stride;
    if (format)
        ctx->pixel_format = *format;
    else if (l10gl_pixel_format_standard(bits_per_pixel,
                                         &ctx->pixel_format) < 0) {
        memset(&ctx->pixel_format, 0, sizeof(ctx->pixel_format));
        ctx->pixel_format.bits_per_pixel = (uint8_t)bits_per_pixel;
    }
}
