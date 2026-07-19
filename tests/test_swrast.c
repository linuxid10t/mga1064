/* Pixel-level regression tests for the L10GL software reference backend. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "l10gl.h"

struct rgb {
    uint8_t r, g, b;
};

static struct l10gl_vertex vertex(float x, float y, float z, float w,
                                  float r, float g, float b, float a,
                                  float u, float v)
{
    return (struct l10gl_vertex) {
        .x = x, .y = y, .z = z, .w = w,
        .r = r, .g = g, .b = b, .a = a,
        .u = u, .v = v,
    };
}

static void draw_quad(struct l10gl_ctx *ctx,
                      struct l10gl_vertex a, struct l10gl_vertex b,
                      struct l10gl_vertex c, struct l10gl_vertex d,
                      int textured)
{
    if (textured) {
        l10gl_draw_textured_triangle(ctx, a, b, c);
        l10gl_draw_textured_triangle(ctx, a, c, d);
    } else {
        l10gl_draw_triangle(ctx, a, b, c);
        l10gl_draw_triangle(ctx, a, c, d);
    }
}

static struct rgb *read_ppm(const char *path, int expected_width,
                            int expected_height)
{
    char magic[3];
    int width, height, maxval;
    size_t count = (size_t)expected_width * (size_t)expected_height;
    struct rgb *pixels;
    FILE *file = fopen(path, "rb");

    if (!file) {
        fprintf(stderr, "test-swrast: cannot open %s: %s\n",
                path, strerror(errno));
        return NULL;
    }
    if (fscanf(file, "%2s%d%d%d", magic, &width, &height, &maxval) != 4 ||
        strcmp(magic, "P6") != 0 || width != expected_width ||
        height != expected_height || maxval != 255 || fgetc(file) == EOF) {
        fprintf(stderr, "test-swrast: malformed PPM header in %s\n", path);
        fclose(file);
        return NULL;
    }
    pixels = malloc(count * sizeof(*pixels));
    if (!pixels) {
        fclose(file);
        return NULL;
    }
    if (fread(pixels, sizeof(*pixels), count, file) != count ||
        fgetc(file) != EOF) {
        fprintf(stderr, "test-swrast: malformed PPM pixels in %s\n", path);
        free(pixels);
        fclose(file);
        return NULL;
    }
    fclose(file);
    return pixels;
}

static int expect_pixel(const struct rgb *pixels, int width, int x, int y,
                        int r, int g, int b, const char *label)
{
    const struct rgb *pixel = &pixels[y * width + x];

    /* Plane interpolation and RGB565 expansion can differ by one 8-bit
     * quantization step without changing the rasterization result. */
    if (abs((int)pixel->r - r) <= 1 && abs((int)pixel->g - g) <= 1 &&
        abs((int)pixel->b - b) <= 1)
        return 0;
    fprintf(stderr,
            "test-swrast: %s pixel (%d,%d) is (%u,%u,%u), expected "
            "(%d,%d,%d)\n", label, x, y,
            pixel->r, pixel->g, pixel->b, r, g, b);
    return -1;
}

static int test_reference_frame(const char *path)
{
    static const uint32_t perspective_texels[4] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff,
    };
    static const uint32_t linear_texels[2] = {
        0xffff0000, 0xff0000ff,
    };
    struct l10gl_texture perspective_texture = {0};
    struct l10gl_texture linear_texture = {0};
    struct l10gl_ctx ctx;
    struct rgb *pixels = NULL;
    int failed = 0;

    if (setenv("L10GL_SWRAST_DUMP", path, 1) < 0)
        return -1;
    if (l10gl_create(&ctx, &swrast_backend, 20, 16, 3) < 0)
        return -1;
    if (ctx.width != 20 || ctx.height != 16 || ctx.bpp != 3 ||
        ctx.stride != 60 || ctx.pixel_format.bits_per_pixel != 24 ||
        ctx.pixel_format.red.offset != 16 ||
        ctx.pixel_format.green.offset != 8 ||
        ctx.pixel_format.blue.offset != 0) {
        fprintf(stderr, "test-swrast: incorrect published RGB888 mode\n");
        failed = 1;
        goto done;
    }
    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_clear(&ctx);

    /* Two half-alpha triangles share the diagonal.  Every pixel in the
     * 6x6 quad must be blended exactly once: a gap is black and double
     * ownership is 75% red instead of 50%. */
    l10gl_enable_depth_test(&ctx, 0);
    l10gl_enable_blend(&ctx, 1);
    l10gl_blend_func(&ctx, L10GL_SRC_ALPHA, L10GL_ONE_MINUS_SRC_ALPHA);
    draw_quad(&ctx,
              vertex(1, 1, 0, 1, 1, 0, 0, 0.5f, 0, 0),
              vertex(7, 1, 0, 1, 1, 0, 0, 0.5f, 0, 0),
              vertex(7, 7, 0, 1, 1, 0, 0, 0.5f, 0, 0),
              vertex(1, 7, 0, 1, 1, 0, 0, 0.5f, 0, 0), 0);

    /* Near green must replace far red; later mid-depth blue must fail. */
    l10gl_enable_blend(&ctx, 0);
    l10gl_enable_depth_test(&ctx, 1);
    l10gl_depth_func(&ctx, L10GL_LESS);
    draw_quad(&ctx,
              vertex(9, 1, .75f, 1, 1, 0, 0, 1, 0, 0),
              vertex(15, 1, .75f, 1, 1, 0, 0, 1, 0, 0),
              vertex(15, 7, .75f, 1, 1, 0, 0, 1, 0, 0),
              vertex(9, 7, .75f, 1, 1, 0, 0, 1, 0, 0), 0);
    draw_quad(&ctx,
              vertex(9, 1, .25f, 1, 0, 1, 0, 1, 0, 0),
              vertex(15, 1, .25f, 1, 0, 1, 0, 1, 0, 0),
              vertex(15, 7, .25f, 1, 0, 1, 0, 1, 0, 0),
              vertex(9, 7, .25f, 1, 0, 1, 0, 1, 0, 0), 0);
    draw_quad(&ctx,
              vertex(9, 1, .50f, 1, 0, 0, 1, 1, 0, 0),
              vertex(15, 1, .50f, 1, 0, 0, 1, 1, 0, 0),
              vertex(15, 7, .50f, 1, 0, 0, 1, 1, 0, 0),
              vertex(9, 7, .50f, 1, 0, 0, 1, 1, 0, 0), 0);

    /* At (4.5,10.5), affine U is .583 (blue texel), but interpolating
     * U*W/W with the right vertex's W=.25 gives U=.259 (green texel). */
    l10gl_enable_depth_test(&ctx, 0);
    if (l10gl_tex_image_2d(&ctx, &perspective_texture, 4, 1,
                           L10GL_TEX_FMT_ARGB8888,
                           perspective_texels) < 0) {
        failed = 1;
        goto done;
    }
    l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);
    l10gl_bind_texture(&ctx, &perspective_texture);
    l10gl_draw_textured_triangle(
        &ctx,
        vertex(1, 9, 0, 1.00f, 1, 1, 1, 1, 0, 0.5f),
        vertex(7, 9, 0, 0.25f, 1, 1, 1, 1, 1, 0.5f),
        vertex(1, 15, 0, 1.00f, 1, 1, 1, 1, 0, 0.5f));

    /* Constant U=.5 lies halfway between a red and blue texel under the
     * OpenGL-style u*width-.5 bilinear sampling convention. */
    if (l10gl_tex_image_2d(&ctx, &linear_texture, 2, 1,
                           L10GL_TEX_FMT_ARGB8888, linear_texels) < 0) {
        failed = 1;
        goto done;
    }
    l10gl_tex_parameter(&ctx, L10GL_FILTER_LINEAR, L10GL_WRAP_CLAMP);
    l10gl_bind_texture(&ctx, &linear_texture);
    draw_quad(&ctx,
              vertex(9, 9, 0, 1, 1, 1, 1, 1, .5f, .5f),
              vertex(15, 9, 0, 1, 1, 1, 1, 1, .5f, .5f),
              vertex(15, 15, 0, 1, 1, 1, 1, 1, .5f, .5f),
              vertex(9, 15, 0, 1, 1, 1, 1, 1, .5f, .5f), 1);

    l10gl_swap_buffers(&ctx);
    pixels = read_ppm(path, 20, 16);
    if (!pixels) {
        failed = 1;
        goto done;
    }
    failed |= expect_pixel(pixels, 20, 0, 0, 0, 0, 0, "background");
    for (int y = 1; y < 7; y++)
        for (int x = 1; x < 7; x++)
            failed |= expect_pixel(pixels, 20, x, y, 128, 0, 0,
                                   "top-left ownership");
    failed |= expect_pixel(pixels, 20, 11, 3, 0, 255, 0, "depth");
    failed |= expect_pixel(pixels, 20, 4, 10, 0, 255, 0, "perspective");
    failed |= expect_pixel(pixels, 20, 11, 11, 128, 0, 128, "bilinear");

done:
    free(pixels);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    return failed ? -1 : 0;
}

static int test_rgb565_dump(const char *path)
{
    struct l10gl_ctx ctx;
    struct rgb *pixels;
    int failed;

    if (setenv("L10GL_SWRAST_DUMP", path, 1) < 0)
        return -1;
    if (l10gl_create(&ctx, &swrast_backend, 2, 1, 2) < 0)
        return -1;
    if (ctx.stride != 4 || ctx.pixel_format.bits_per_pixel != 16 ||
        ctx.pixel_format.red.offset != 11 ||
        ctx.pixel_format.green.length != 6) {
        fprintf(stderr, "test-swrast: incorrect published RGB565 mode\n");
        l10gl_destroy(&ctx);
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10gl_clear_color(&ctx, 1.0f, 0.5f, 1.0f);
    l10gl_clear(&ctx);
    l10gl_swap_buffers(&ctx);
    pixels = read_ppm(path, 2, 1);
    failed = !pixels || expect_pixel(pixels, 2, 0, 0, 255, 130, 255,
                                     "RGB565 conversion");
    free(pixels);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    return failed ? -1 : 0;
}

static int test_double_buffered_swaps(const char *directory)
{
    char dump_template[256];
    char first_path[256];
    char second_path[256];
    struct l10gl_ctx ctx;
    struct rgb *first = NULL;
    struct rgb *second = NULL;
    int failed = 0;

    snprintf(dump_template, sizeof(dump_template), "%s/swap%%02d.ppm",
             directory);
    snprintf(first_path, sizeof(first_path), "%s/swap00.ppm", directory);
    snprintf(second_path, sizeof(second_path), "%s/swap01.ppm", directory);
    if (setenv("L10GL_SWRAST_DUMP", dump_template, 1) < 0)
        return -1;
    if (l10gl_create(&ctx, &swrast_backend, 3, 2, 3) < 0)
        return -1;

    l10gl_clear_color(&ctx, 1.0f, 0.0f, 0.0f);
    l10gl_clear(&ctx);
    if (access(first_path, F_OK) == 0) {
        fprintf(stderr, "test-swrast: frame became visible before swap\n");
        failed = 1;
        goto done;
    }
    l10gl_swap_buffers(&ctx);

    l10gl_clear_color(&ctx, 0.0f, 1.0f, 0.0f);
    l10gl_clear(&ctx);
    l10gl_swap_buffers(&ctx);

    first = read_ppm(first_path, 3, 2);
    second = read_ppm(second_path, 3, 2);
    if (!first || !second) {
        failed = 1;
        goto done;
    }
    failed |= expect_pixel(first, 3, 1, 1, 255, 0, 0,
                           "first completed back buffer");
    failed |= expect_pixel(second, 3, 1, 1, 0, 255, 0,
                           "second completed back buffer");

done:
    free(first);
    free(second);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    unlink(first_path);
    unlink(second_path);
    return failed ? -1 : 0;
}

/* A convex pentagon submitted through the immediate-mode frontend, drawn
 * identically as either a polygon or a triangle fan. White vertex color so
 * the texture is sampled unmodified; texcoords span the 2x2 texture. */
static void draw_textured_pentagon(struct l10gl_ctx *ctx,
                                   enum l10gl_primitive prim)
{
    l10gl_begin(ctx, prim);
    l10gl_color4f(ctx, 1, 1, 1, 1); l10gl_texcoord2f(ctx, .5f, 1);
    l10gl_vertex3f(ctx,  0.0f,  0.7f, 0);
    l10gl_color4f(ctx, 1, 1, 1, 1); l10gl_texcoord2f(ctx, 1, .5f);
    l10gl_vertex3f(ctx,  0.6f,  0.2f, 0);
    l10gl_color4f(ctx, 1, 1, 1, 1); l10gl_texcoord2f(ctx, .8f, 0);
    l10gl_vertex3f(ctx,  0.4f, -0.7f, 0);
    l10gl_color4f(ctx, 1, 1, 1, 1); l10gl_texcoord2f(ctx, .2f, 0);
    l10gl_vertex3f(ctx, -0.4f, -0.7f, 0);
    l10gl_color4f(ctx, 1, 1, 1, 1); l10gl_texcoord2f(ctx, 0, .5f);
    l10gl_vertex3f(ctx, -0.6f,  0.2f, 0);
    l10gl_end(ctx);
}

static int render_pentagon_dump(const char *path, enum l10gl_primitive prim)
{
    static const uint32_t texels[4] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff,
    };
    struct l10gl_texture texture = {0};
    struct l10gl_ctx ctx;

    if (setenv("L10GL_SWRAST_DUMP", path, 1) < 0)
        return -1;
    if (l10gl_create(&ctx, &swrast_backend, 24, 16, 3) < 0) {
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10gl_clear_color(&ctx, 0, 0, 0);
    l10gl_clear_depth(&ctx, 1);
    l10gl_clear(&ctx);
    l10gl_enable_depth_test(&ctx, 0);
    l10gl_enable_blend(&ctx, 0);
    l10gl_cull_face(&ctx, L10GL_CULL_NONE);
    l10gl_matrix_mode(&ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(&ctx);
    l10gl_matrix_mode(&ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(&ctx);
    l10gl_viewport(&ctx, 0, 0, 24, 16);
    if (l10gl_tex_image_2d(&ctx, &texture, 2, 2, L10GL_TEX_FMT_ARGB8888,
                           texels) < 0) {
        l10gl_destroy(&ctx);
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_REPEAT);
    l10gl_bind_texture(&ctx, &texture);
    draw_textured_pentagon(&ctx, prim);
    l10gl_swap_buffers(&ctx);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    return 0;
}

/* A textured convex polygon and the equivalent triangle fan must rasterize
 * to byte-identical pixels: the polygon's fan decomposition emits the same
 * triangles in the same order. */
static int test_polygon_matches_fan(const char *directory)
{
    char poly_path[256];
    char fan_path[256];
    struct rgb *poly = NULL;
    struct rgb *fan = NULL;
    size_t count = (size_t)24 * 16, i;
    int failed = 0, painted = 0;

    snprintf(poly_path, sizeof(poly_path), "%s/poly.ppm", directory);
    snprintf(fan_path, sizeof(fan_path), "%s/fan.ppm", directory);
    if (render_pentagon_dump(poly_path, L10GL_POLYGON) < 0 ||
        render_pentagon_dump(fan_path, L10GL_TRIANGLE_FAN) < 0) {
        fprintf(stderr, "test-swrast: polygon/fan dump failed\n");
        return -1;
    }
    poly = read_ppm(poly_path, 24, 16);
    fan = read_ppm(fan_path, 24, 16);
    if (!poly || !fan) {
        failed = 1;
        goto done;
    }
    if (memcmp(poly, fan, count * sizeof(*poly)) != 0) {
        fprintf(stderr, "test-swrast: polygon frame differs from fan frame\n");
        for (i = 0; i < count; i++)
            if (poly[i].r != fan[i].r || poly[i].g != fan[i].g ||
                poly[i].b != fan[i].b) {
                fprintf(stderr, "test-swrast: first diff at (%zu,%zu): "
                        "poly=(%u,%u,%u) fan=(%u,%u,%u)\n",
                        i % 24, i / 24,
                        poly[i].r, poly[i].g, poly[i].b,
                        fan[i].r, fan[i].g, fan[i].b);
                break;
            }
        failed = 1;
    }
    /* Reject a vacuous pass: the polygon must have painted something, so the
     * equality above is comparing real rasterization, not two empty frames. */
    for (i = 0; i < count; i++)
        if (poly[i].r || poly[i].g || poly[i].b)
            painted++;
    if (painted == 0) {
        fprintf(stderr, "test-swrast: polygon painted no pixels\n");
        failed = 1;
    }

done:
    free(poly);
    free(fan);
    unlink(poly_path);
    unlink(fan_path);
    return failed ? -1 : 0;
}

/* Render a full-screen NEAREST textured quad with UV (0,0)-(u1,v1) over an
 * fbw x fbh RGB888 buffer, dumping one frame. Used to probe per-axis texture
 * addressing on rectangular images. */
static int render_rect_texture(const char *path, int texw, int texh,
                               const uint32_t *texels,
                               enum l10gl_tex_wrap wrap,
                               float u1, float v1, int fbw, int fbh)
{
    struct l10gl_texture texture = {0};
    struct l10gl_ctx ctx;

    if (setenv("L10GL_SWRAST_DUMP", path, 1) < 0)
        return -1;
    if (l10gl_create(&ctx, &swrast_backend, fbw, fbh, 3) < 0) {
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10gl_clear_color(&ctx, 0, 0, 0);
    l10gl_clear_depth(&ctx, 1);
    l10gl_clear(&ctx);
    l10gl_enable_depth_test(&ctx, 0);
    l10gl_enable_blend(&ctx, 0);
    l10gl_cull_face(&ctx, L10GL_CULL_NONE);
    l10gl_matrix_mode(&ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(&ctx);
    l10gl_matrix_mode(&ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(&ctx);
    l10gl_viewport(&ctx, 0, 0, fbw, fbh);
    if (l10gl_tex_image_2d(&ctx, &texture, texw, texh,
                           L10GL_TEX_FMT_ARGB8888, texels) < 0) {
        l10gl_destroy(&ctx);
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, wrap);
    l10gl_bind_texture(&ctx, &texture);
    draw_quad(&ctx,
              vertex(0, 0, 0, 1, 1, 1, 1, 1, 0, 0),
              vertex(fbw, 0, 0, 1, 1, 1, 1, 1, u1, 0),
              vertex(fbw, fbh, 0, 1, 1, 1, 1, 1, u1, v1),
              vertex(0, fbh, 0, 1, 1, 1, 1, 1, 0, v1), 1);
    l10gl_swap_buffers(&ctx);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    return 0;
}

/* Rectangular textures must address each axis independently (Q3). A wide
 * image spreads its colors horizontally and is uniform vertically; a tall
 * image does the inverse. REPEAT tiles each axis; CLAMP stretches it. */
static int test_rectangular_textures(const char *directory)
{
    static const uint32_t four[4] = {
        0xffff0000, 0xff00ff00, 0xff0000ff, 0xffffffff,
    };
    static const uint32_t two[2] = { 0xffff0000, 0xff0000ff };
    char path[256];
    struct rgb *px;
    int failed = 0;

    /* Wide 4x1, CLAMP: four horizontal bands, uniform down the single row. */
    snprintf(path, sizeof(path), "%s/wide_clamp.ppm", directory);
    if (render_rect_texture(path, 4, 1, four, L10GL_WRAP_CLAMP, 1, 1, 16, 4))
        return -1;
    px = read_ppm(path, 16, 4);
    if (!px) { unlink(path); return -1; }
    failed |= expect_pixel(px, 16, 1, 0, 255, 0, 0, "wide clamp band 0");
    failed |= expect_pixel(px, 16, 5, 0, 0, 255, 0, "wide clamp band 1");
    failed |= expect_pixel(px, 16, 9, 0, 0, 0, 255, "wide clamp band 2");
    failed |= expect_pixel(px, 16, 13, 0, 255, 255, 255, "wide clamp band 3");
    failed |= expect_pixel(px, 16, 1, 3, 255, 0, 0, "wide clamp V-uniform");
    free(px);
    unlink(path);

    /* Tall 1x4, CLAMP: four vertical bands, uniform across the single col. */
    snprintf(path, sizeof(path), "%s/tall_clamp.ppm", directory);
    if (render_rect_texture(path, 1, 4, four, L10GL_WRAP_CLAMP, 1, 1, 16, 4))
        return -1;
    px = read_ppm(path, 16, 4);
    if (!px) { unlink(path); return -1; }
    failed |= expect_pixel(px, 16, 0, 0, 255, 0, 0, "tall clamp band 0");
    failed |= expect_pixel(px, 16, 0, 1, 0, 255, 0, "tall clamp band 1");
    failed |= expect_pixel(px, 16, 0, 2, 0, 0, 255, "tall clamp band 2");
    failed |= expect_pixel(px, 16, 0, 3, 255, 255, 255, "tall clamp band 3");
    failed |= expect_pixel(px, 16, 8, 1, 0, 255, 0, "tall clamp U-uniform");
    free(px);
    unlink(path);

    /* Wide 2x1, REPEAT over U=0..2: the two texels tile twice across. */
    snprintf(path, sizeof(path), "%s/wide_repeat.ppm", directory);
    if (render_rect_texture(path, 2, 1, two, L10GL_WRAP_REPEAT, 2, 1, 16, 4))
        return -1;
    px = read_ppm(path, 16, 4);
    if (!px) { unlink(path); return -1; }
    failed |= expect_pixel(px, 16, 1, 0, 255, 0, 0, "wide repeat u0 red");
    failed |= expect_pixel(px, 16, 5, 0, 0, 0, 255, "wide repeat half blue");
    failed |= expect_pixel(px, 16, 9, 0, 255, 0, 0, "wide repeat wrap red");
    failed |= expect_pixel(px, 16, 13, 0, 0, 0, 255, "wide repeat wrap blue");
    free(px);
    unlink(path);

    /* Tall 1x2, REPEAT over V=0..2: the two texels tile twice down. */
    snprintf(path, sizeof(path), "%s/tall_repeat.ppm", directory);
    if (render_rect_texture(path, 1, 2, two, L10GL_WRAP_REPEAT, 1, 2, 4, 16))
        return -1;
    px = read_ppm(path, 4, 16);
    if (!px) { unlink(path); return -1; }
    failed |= expect_pixel(px, 4, 0, 1, 255, 0, 0, "tall repeat v0 red");
    failed |= expect_pixel(px, 4, 0, 5, 0, 0, 255, "tall repeat half blue");
    failed |= expect_pixel(px, 4, 0, 9, 255, 0, 0, "tall repeat wrap red");
    failed |= expect_pixel(px, 4, 0, 13, 0, 0, 255, "tall repeat wrap blue");
    free(px);
    unlink(path);

    return failed ? -1 : 0;
}

int main(void)
{
    char directory[] = "/tmp/l10gl-swrast-test.XXXXXX";
    char reference_path[256];
    char rgb565_path[256];
    int failed = 0;

    /* Test output must stay offscreen even when the invoking shell normally
     * uses swrast for a live framebuffer. */
    unsetenv("L10GL_SWRAST_FB");

    if (!mkdtemp(directory)) {
        perror("test-swrast: mkdtemp");
        return 1;
    }
    snprintf(reference_path, sizeof(reference_path), "%s/reference.ppm",
             directory);
    snprintf(rgb565_path, sizeof(rgb565_path), "%s/rgb565.ppm", directory);

    failed |= test_reference_frame(reference_path);
    failed |= test_rgb565_dump(rgb565_path);
    failed |= test_double_buffered_swaps(directory);
    failed |= test_polygon_matches_fan(directory);
    failed |= test_rectangular_textures(directory);

    unlink(reference_path);
    unlink(rgb565_path);
    rmdir(directory);
    if (failed) {
        fprintf(stderr, "test-swrast: FAILED\n");
        return 1;
    }
    printf("test-swrast: PASS (coverage, blend, depth, perspective, "
           "bilinear, RGB565, PPM, double buffering, polygon==fan, "
           "rectangular textures)\n");
    return 0;
}
