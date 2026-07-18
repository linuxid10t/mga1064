/* Unit tests for the Phase P1 requested/actual mode contract. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "fbdev.h"
#include "l10gl.h"

static int failed;
static int cleanup_calls;

#define EXPECT(condition, label) do {                                      \
    if (!(condition)) {                                                    \
        fprintf(stderr, "test-mode: FAIL: %s\n", (label));               \
        failed = 1;                                                        \
    }                                                                      \
} while (0)

static void copy_format_to_var(struct fb_var_screeninfo *var,
                               const struct l10gl_pixel_format *format)
{
    var->bits_per_pixel = format->bits_per_pixel;
    var->red = (struct fb_bitfield) {
        format->red.offset, format->red.length, format->red.msb_right,
    };
    var->green = (struct fb_bitfield) {
        format->green.offset, format->green.length, format->green.msb_right,
    };
    var->blue = (struct fb_bitfield) {
        format->blue.offset, format->blue.length, format->blue.msb_right,
    };
    var->transp = (struct fb_bitfield) {
        format->alpha.offset, format->alpha.length, format->alpha.msb_right,
    };
}

static void test_formats_and_matching(void)
{
    struct l10gl_pixel_format rgb555, rgb565, rgb888, xrgb8888;
    struct l10gl_fbdev_mode mode;

    EXPECT(l10gl_pixel_format_standard(15, &rgb555) == 0,
           "construct RGB555");
    EXPECT(rgb555.red.offset == 10 && rgb555.green.length == 5 &&
           rgb555.alpha.length == 0,
           "RGB555 fields");
    EXPECT(l10gl_pixel_format_standard(16, &rgb565) == 0,
           "construct RGB565");
    EXPECT(rgb565.red.offset == 11 && rgb565.green.length == 6 &&
           rgb565.blue.length == 5, "RGB565 fields");
    EXPECT(l10gl_pixel_format_standard(24, &rgb888) == 0 &&
           rgb888.red.offset == 16 && rgb888.green.offset == 8,
           "construct RGB888");
    EXPECT(l10gl_pixel_format_standard(32, &xrgb8888) == 0 &&
           xrgb8888.alpha.length == 0, "construct XRGB8888");
    EXPECT(l10gl_pixel_format_standard(12, &xrgb8888) == -ENOTSUP,
           "reject unknown packed format");

    memset(&mode, 0, sizeof(mode));
    mode.var.xres = 800;
    mode.var.yres = 600;
    mode.fix.line_length = 2048; /* Padding must not affect mode matching. */
    copy_format_to_var(&mode.var, &rgb555);
    EXPECT(l10gl_fbdev_mode_matches(&mode, 800, 600, 15, &rgb555),
           "exact fixed-format match");
    EXPECT(!l10gl_fbdev_mode_matches(&mode, 640, 480, 15, &rgb555),
           "geometry mismatch");
    EXPECT(!l10gl_fbdev_mode_matches(&mode, 800, 600, 16, NULL),
           "depth mismatch");
    mode.var.green.length = 6;
    EXPECT(!l10gl_fbdev_mode_matches(&mode, 800, 600, 15, &rgb555),
           "channel mismatch");
    EXPECT(l10gl_fbdev_mode_matches(&mode, 800, 600, 15, NULL),
           "format-agnostic match");
    mode.var.yoffset = 1;
    EXPECT(!l10gl_fbdev_mode_matches(&mode, 800, 600, 15, NULL),
           "nonzero scanout offset mismatch");
}

static void test_mode_publication(void)
{
    struct l10gl_ctx ctx;
    struct l10gl_fbdev_mode mode;
    struct l10gl_pixel_format rgb555;

    memset(&ctx, 0, sizeof(ctx));
    memset(&mode, 0, sizeof(mode));
    l10gl_pixel_format_standard(15, &rgb555);
    mode.var.xres = 800;
    mode.var.yres = 600;
    mode.fix.line_length = 2048;
    copy_format_to_var(&mode.var, &rgb555);
    l10gl_mode_from_fbdev(&ctx, &mode);
    EXPECT(ctx.width == 800 && ctx.height == 600 && ctx.bpp == 2,
           "fbdev actual geometry/storage");
    EXPECT(ctx.stride == 2048 && ctx.pixel_format.bits_per_pixel == 15,
           "fbdev actual stride/depth");
    EXPECT(ctx.pixel_format.alpha.length == 0, "fbdev actual alpha field");

    memset(&ctx, 0, sizeof(ctx));
    l10gl_mode_set_linear(&ctx, 320, 200, 16, 672, NULL);
    EXPECT(ctx.width == 320 && ctx.height == 200 && ctx.bpp == 2 &&
           ctx.stride == 672, "linear padded mode");
    EXPECT(ctx.pixel_format.green.length == 6, "linear default format");
}

static void test_pan_pages(void)
{
    struct l10gl_fbdev_mode mode;
    uint32_t front = UINT32_MAX;
    uint32_t back = UINT32_MAX;

    memset(&mode, 0, sizeof(mode));
    mode.var.xres = mode.var.xres_virtual = 800;
    mode.var.yres = 600;
    mode.var.yres_virtual = 1200;
    mode.var.bits_per_pixel = 32;
    mode.fix.line_length = 3200;
    mode.fix.smem_len = 3200 * 1200;
    EXPECT(l10gl_fbdev_find_pan_pages(&mode, &front, &back) &&
           front == 0 && back == 600,
           "find second vertical fbdev page");

    mode.var.yoffset = 600;
    EXPECT(l10gl_fbdev_find_pan_pages(&mode, &front, &back) &&
           front == 600 && back == 0,
           "find first page behind nonzero scanout");

    mode.var.yoffset = 0;
    mode.fix.smem_len = 3200 * 600;
    EXPECT(!l10gl_fbdev_find_pan_pages(&mode, &front, &back),
           "reject virtual page outside mapped memory");
    mode.fix.smem_len = 3200 * 1200;
    mode.var.yres_virtual = 600;
    EXPECT(!l10gl_fbdev_find_pan_pages(&mode, &front, &back),
           "reject single-height virtual raster");
}

static int valid_backend_init(struct l10gl_ctx *ctx, int width, int height,
                              int bpp)
{
    (void)width;
    (void)height;
    (void)bpp;
    l10gl_mode_set_linear(ctx, 800, 600, 16, 2048, NULL);
    return 0;
}

static int invalid_backend_init(struct l10gl_ctx *ctx, int width, int height,
                                int bpp)
{
    (void)width;
    (void)height;
    (void)bpp;
    l10gl_mode_set_linear(ctx, 800, 600, 16, 100, NULL);
    return 0;
}

static void fake_cleanup(struct l10gl_ctx *ctx)
{
    (void)ctx;
    cleanup_calls++;
}

static void test_frontend_contract(void)
{
    struct l10gl_ctx ctx;
    const struct l10gl_backend valid = {
        .name = "mode-test-valid",
        .init = valid_backend_init,
    };
    const struct l10gl_backend invalid = {
        .name = "mode-test-invalid",
        .init = invalid_backend_init,
        .cleanup = fake_cleanup,
    };

    EXPECT(l10gl_create(&ctx, &valid, 640, 480, 2) == 0,
           "frontend accepts reported actual mode");
    EXPECT(ctx.width == 800 && ctx.height == 600 && ctx.stride == 2048,
           "frontend retains reported actual mode");
    l10gl_destroy(&ctx);

    cleanup_calls = 0;
    EXPECT(l10gl_create(&ctx, &invalid, 640, 480, 2) == -EINVAL,
           "frontend rejects impossible stride");
    EXPECT(cleanup_calls == 1, "frontend cleans invalid backend mode");
}

int main(void)
{
    test_formats_and_matching();
    test_mode_publication();
    test_pan_pages();
    test_frontend_contract();
    if (failed)
        return 1;
    printf("test-mode: PASS (formats, matching, actual mode, panning, "
           "validation)\n");
    return 0;
}
