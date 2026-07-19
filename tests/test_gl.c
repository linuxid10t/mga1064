/* Unit tests for the Phase 4 OpenGL compatibility entry points. */

#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <GL/gl.h>

#include "fbdev.h"
#include "l10gl.h"

#define EPSILON 1.0e-5f

struct capture_state {
    struct l10gl_vertex triangle[3];
    uint32_t texture_pixels[4];
    struct l10gl_texture *bound_texture;
    int triangles;
    int textured_triangles;
    int texture_uploads;
    int texture_binds;
    int texture_parameters;
    int texture_width;
    int texture_height;
    enum l10gl_tex_format texture_format;
    enum l10gl_tex_filter texture_filter;
    enum l10gl_tex_wrap texture_wrap;
    int color_clears;
    int depth_clears;
    int waits;
    int swaps;
};

static struct capture_state capture;
static int failures;

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-gl: %s is %d, expected %d\n",
            label, actual, expected);
    failures++;
}

static void expect_float(const char *label, float actual, float expected)
{
    if (fabsf(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-gl: %s is %.9g, expected %.9g\n",
            label, actual, expected);
    failures++;
}

static int capture_init(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    l10gl_mode_set_linear(ctx, width, height, bpp * 8,
                          (uint32_t)width * (uint32_t)bpp, NULL);
    return 0;
}

static void capture_triangle(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1,
                             struct l10gl_vertex v2)
{
    (void)ctx;
    capture.triangle[0] = v0;
    capture.triangle[1] = v1;
    capture.triangle[2] = v2;
    capture.triangles++;
}

static void capture_textured_triangle(struct l10gl_ctx *ctx,
                                      struct l10gl_vertex v0,
                                      struct l10gl_vertex v1,
                                      struct l10gl_vertex v2)
{
    capture_triangle(ctx, v0, v1, v2);
    capture.textured_triangles++;
}

static int capture_texture_upload(struct l10gl_ctx *ctx,
                                  struct l10gl_texture *texture,
                                  int width, int height,
                                  enum l10gl_tex_format format,
                                  const void *data)
{
    size_t count = (size_t)width * (size_t)height;
    (void)ctx;

    if (count > 4)
        count = 4;
    memset(capture.texture_pixels, 0, sizeof(capture.texture_pixels));
    memcpy(capture.texture_pixels, data, count * sizeof(uint32_t));
    capture.texture_width = width;
    capture.texture_height = height;
    capture.texture_format = format;
    capture.texture_uploads++;
    texture->width = width;
    texture->height = height;
    texture->format = format;
    texture->bytes_per_texel = 4;
    texture->backend_data = texture;
    return 0;
}

static void capture_texture_bind(struct l10gl_ctx *ctx,
                                 struct l10gl_texture *texture)
{
    (void)ctx;
    capture.bound_texture = texture;
    capture.texture_binds++;
}

static void capture_texture_parameter(struct l10gl_ctx *ctx,
                                      enum l10gl_tex_filter filter,
                                      enum l10gl_tex_wrap wrap)
{
    (void)ctx;
    capture.texture_filter = filter;
    capture.texture_wrap = wrap;
    capture.texture_parameters++;
}

static void capture_clear_color(struct l10gl_ctx *ctx,
                                float red, float green, float blue)
{
    (void)ctx;
    expect_float("clear callback red", red, 1.0f);
    expect_float("clear callback green", green, 0.0f);
    expect_float("clear callback blue", blue, 0.25f);
    capture.color_clears++;
}

static void capture_clear_depth(struct l10gl_ctx *ctx, float depth)
{
    (void)ctx;
    expect_float("clear callback depth", depth, 0.0f);
    capture.depth_clears++;
}

static void capture_wait(struct l10gl_ctx *ctx)
{
    (void)ctx;
    capture.waits++;
}

static void capture_swap(struct l10gl_ctx *ctx)
{
    (void)ctx;
    capture.swaps++;
}

static const struct l10gl_backend capture_backend = {
    .name = "gl-capture",
    .init = capture_init,
    .clear_color = capture_clear_color,
    .clear_depth = capture_clear_depth,
    .draw_triangle = capture_triangle,
    .draw_textured_triangle = capture_textured_triangle,
    .tex_image_2d = capture_texture_upload,
    .bind_texture = capture_texture_bind,
    .tex_parameter = capture_texture_parameter,
    .wait_engine = capture_wait,
    .swap_buffers = capture_swap,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_ZBUFFER | L10GL_CAP_TEXTURE,
};

static void test_no_context(void)
{
    glVertex3f(0, 0, 0);
    expect_int("no-context error", glGetError(), GL_INVALID_OPERATION);
    expect_int("error latch clears", glGetError(), GL_NO_ERROR);
}

static void test_immediate_and_matrices(struct l10gl_ctx *ctx)
{
    memset(&capture, 0, sizeof(capture));
    glViewport(0, 0, 100, 80);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glTranslatef(0.25f, 0, 0);

    glBegin(GL_TRIANGLES);
    glColor4f(0.1f, 0.2f, 0.3f, 0.4f);
    glNormal3f(0, 0, 1);
    glTexCoord2f(0.6f, 0.7f);
    glVertex3f(-0.5f, -0.5f, 0);
    glVertex3f(0.5f, -0.5f, 0);
    glVertex2f(0, 0.5f);
    glEnd();

    expect_int("one GL triangle", capture.triangles, 1);
    expect_float("translated left X", capture.triangle[0].x, 37.5f);
    expect_float("right X", capture.triangle[1].x, 87.5f);
    expect_float("top Y", capture.triangle[2].y, 20.0f);
    expect_float("captured red", capture.triangle[0].r, 0.1f);
    expect_float("captured alpha", capture.triangle[1].a, 0.4f);
    expect_float("captured U", capture.triangle[2].u, 0.6f);
    expect_int("immediate-mode error", glGetError(), GL_NO_ERROR);

    memset(&capture, 0, sizeof(capture));
    glLoadIdentity();
    glBegin(GL_QUADS);
    glColor3f(0.1f, 0, 0); glVertex2f(-.5f, -.5f);
    glColor3f(0.2f, 0, 0); glVertex2f( .5f, -.5f);
    glColor3f(0.3f, 0, 0); glVertex2f( .5f,  .5f);
    glColor3f(0.4f, 0, 0); glVertex2f(-.5f,  .5f);
    glEnd();
    expect_int("GL quad triangle count", capture.triangles, 2);
    expect_float("GL quad second triangle v0", capture.triangle[0].r, .1f);
    expect_float("GL quad second triangle v1", capture.triangle[1].r, .3f);
    expect_float("GL quad second triangle v2", capture.triangle[2].r, .4f);

    memset(&capture, 0, sizeof(capture));
    glBegin(GL_QUAD_STRIP);
    glVertex2f(-.5f, -.5f); glVertex2f(-.5f, .5f);
    glVertex2f(0, -.5f);    glVertex2f(0, .5f);
    glVertex2f(.5f, -.5f); glVertex2f(.5f, .5f);
    glEnd();
    expect_int("GL quad strip triangle count", capture.triangles, 4);

    glBegin(GL_POINTS);
    expect_int("unsupported points", glGetError(), GL_INVALID_ENUM);
    glEnd();
    expect_int("end after rejected begin", glGetError(), GL_INVALID_OPERATION);

    glMatrixMode(0xdead);
    expect_int("bad matrix target", glGetError(), GL_INVALID_ENUM);

    (void)ctx;
}

static void test_state(struct l10gl_ctx *ctx)
{
    glEnable(GL_DEPTH_TEST);
    expect_int("depth enabled", ctx->depth_test_enabled, 1);
    glDisable(GL_DEPTH_TEST);
    expect_int("depth disabled", ctx->depth_test_enabled, 0);

    glDepthFunc(GL_GEQUAL);
    expect_int("depth function", ctx->depth_func_val, L10GL_GEQUAL);
    glDepthMask(GL_FALSE);
    expect_int("depth writes disabled", ctx->depth_writes_enabled, 0);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    expect_int("source blend factor", ctx->blend_sfactor, L10GL_SRC_ALPHA);
    expect_int("destination blend factor", ctx->blend_dfactor,
               L10GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_BLEND);
    expect_int("blend enabled", ctx->blend_enabled, 1);

    glCullFace(GL_FRONT);
    glEnable(GL_CULL_FACE);
    expect_int("front culling", ctx->cull_mode_val, L10GL_CULL_FRONT);
    glDisable(GL_CULL_FACE);
    expect_int("culling disabled", ctx->cull_mode_val, L10GL_CULL_NONE);

    glEnable(GL_LIGHTING);
    expect_int("light0 still gates lighting", ctx->lighting_enabled, 0);
    glEnable(GL_LIGHT0);
    expect_int("lighting enabled", ctx->lighting_enabled, 1);
    glDisable(GL_LIGHT0);
    expect_int("lighting disabled with light0", ctx->lighting_enabled, 0);

    glEnable(0xbeef);
    expect_int("bad capability", glGetError(), GL_INVALID_ENUM);
    glDepthFunc(0xbeef);
    glBlendFunc(GL_SRC_ALPHA, 0xbeef);
    expect_int("first error remains latched", glGetError(), GL_INVALID_ENUM);
    expect_int("second error consumed with latch", glGetError(), GL_NO_ERROR);
}

static void test_lighting_material(struct l10gl_ctx *ctx)
{
    const GLfloat position[4] = { 0, 0, 2, 0 };
    const GLfloat ambient[4] = { .1f, .1f, .1f, 1 };
    const GLfloat diffuse[4] = { .6f, .6f, .6f, 1 };
    const GLfloat material[4] = { .5f, .25f, 1, .75f };
    const GLfloat positional[4] = { 0, 0, 2, 1 };

    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glLightfv(GL_LIGHT0, GL_POSITION, position);
    expect_float("GL light ray direction Z", ctx->light_dir_z, -1);
    glLightfv(GL_LIGHT0, GL_AMBIENT, ambient);
    glLightfv(GL_LIGHT0, GL_DIFFUSE, diffuse);
    glMaterialfv(GL_FRONT, GL_AMBIENT_AND_DIFFUSE, material);
    expect_float("GL light ambient", ctx->ambient_r, .1f);
    expect_float("GL light diffuse", ctx->light_r, .6f);
    expect_float("GL material red", ctx->material_r, .5f);
    expect_float("GL material alpha", ctx->material_a, .75f);

    glEnable(GL_LIGHT0);
    glEnable(GL_LIGHTING);
    glNormal3f(0, 0, 1);
    memset(&capture, 0, sizeof(capture));
    glBegin(GL_TRIANGLES);
    glVertex2f(-.25f, -.25f);
    glVertex2f(.25f, -.25f);
    glVertex2f(0, .25f);
    glEnd();
    expect_int("GL lit triangle", capture.triangles, 1);
    expect_float("GL lit material red", capture.triangle[0].r, .35f);
    expect_float("GL lit material green", capture.triangle[0].g, .175f);
    expect_float("GL lit material blue", capture.triangle[0].b, .7f);
    expect_float("GL lit material alpha", capture.triangle[0].a, .75f);

    glLightfv(GL_LIGHT0, GL_POSITION, positional);
    expect_int("positional light rejected", glGetError(), GL_INVALID_VALUE);
    glMaterialfv(GL_BACK, GL_AMBIENT_AND_DIFFUSE, material);
    expect_int("back-only material rejected", glGetError(), GL_INVALID_ENUM);
    glDisable(GL_LIGHTING);
}

static void test_shade_model(struct l10gl_ctx *ctx)
{
    glShadeModel(GL_FLAT);
    expect_int("flat shading enabled", ctx->flat_shading, 1);
    memset(&capture, 0, sizeof(capture));
    glBegin(GL_TRIANGLES);
    glColor3f(.1f, 0, 0); glVertex2f(-.25f, -.25f);
    glColor3f(.2f, 0, 0); glVertex2f( .25f, -.25f);
    glColor3f(.3f, 0, 0); glVertex2f(0, .25f);
    glEnd();
    expect_float("flat provoking color v0", capture.triangle[0].r, .3f);
    expect_float("flat provoking color v1", capture.triangle[1].r, .3f);
    expect_float("flat provoking color v2", capture.triangle[2].r, .3f);

    glShadeModel(GL_SMOOTH);
    expect_int("smooth shading restored", ctx->flat_shading, 0);
    glShadeModel(0xbeef);
    expect_int("bad shade model", glGetError(), GL_INVALID_ENUM);
}

static void test_texture_objects(struct l10gl_ctx *ctx)
{
    const GLubyte rgba[16] = {
        255, 0, 0, 255,  0, 255, 0, 128,
        0, 0, 255, 64,   255, 255, 255, 0,
    };
    /* Default GL_UNPACK_ALIGNMENT=4: two RGB texels occupy six bytes and
     * each source row has two padding bytes. */
    const GLubyte rgb[16] = {
        1, 2, 3, 4, 5, 6, 0, 0,
        7, 8, 9, 10, 11, 12, 0, 0,
    };
    GLuint textures[2] = { 0, 0 };
    int plain_before;

    memset(&capture, 0, sizeof(capture));
    glGenTextures(2, textures);
    expect_int("generated texture 0 nonzero", textures[0] != 0, 1);
    expect_int("generated texture names differ", textures[0] != textures[1],
               1);
    expect_int("reserved name is not texture", glIsTexture(textures[0]),
               GL_FALSE);

    glBindTexture(GL_TEXTURE_2D, textures[0]);
    expect_int("bound name becomes texture", glIsTexture(textures[0]),
               GL_TRUE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    expect_int("RGBA upload count", capture.texture_uploads, 1);
    expect_int("RGBA upload width", capture.texture_width, 2);
    expect_int("RGBA backend format", capture.texture_format,
               L10GL_TEX_FMT_ARGB8888);
    expect_int("RGBA red conversion", (int)capture.texture_pixels[0],
               (int)0xffff0000u);
    expect_int("RGBA alpha conversion", (int)capture.texture_pixels[1],
               (int)0x8000ff00u);
    expect_int("disabled texture remains unbound", ctx->current_texture == NULL,
               1);

    glEnable(GL_TEXTURE_2D);
    expect_int("enabled texture bound", ctx->current_texture != NULL, 1);
    expect_int("linear filter applied", capture.texture_filter,
               L10GL_FILTER_LINEAR);
    expect_int("repeat applied", capture.texture_wrap, L10GL_WRAP_REPEAT);

    glColor3f(1, 1, 1);
    glBegin(GL_TRIANGLES);
    glTexCoord2f(0, 0); glVertex2f(-.25f, -.25f);
    glTexCoord2f(1, 0); glVertex2f( .25f, -.25f);
    glTexCoord2f(0, 1); glVertex2f(0, .25f);
    glEnd();
    expect_int("textured GL dispatch", capture.textured_triangles, 1);

    glDisable(GL_TEXTURE_2D);
    expect_int("disable unbinds backend texture", ctx->current_texture == NULL,
               1);
    plain_before = capture.triangles;
    glBegin(GL_TRIANGLES);
    glVertex2f(-.25f, -.25f);
    glVertex2f( .25f, -.25f);
    glVertex2f(0, .25f);
    glEnd();
    expect_int("disabled texture uses plain path", capture.triangles,
               plain_before + 1);
    expect_int("no extra textured dispatch", capture.textured_triangles, 1);

    glBindTexture(GL_TEXTURE_2D, textures[1]);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, 2, 2, 0,
                 GL_RGB, GL_UNSIGNED_BYTE, rgb);
    expect_int("RGB upload count", capture.texture_uploads, 2);
    expect_int("RGB row 0 texel 0", (int)capture.texture_pixels[0],
               (int)0xff010203u);
    expect_int("RGB row 0 texel 1", (int)capture.texture_pixels[1],
               (int)0xff040506u);
    expect_int("RGB padded row 1 texel 0", (int)capture.texture_pixels[2],
               (int)0xff070809u);
    expect_int("RGB padded row 1 texel 1", (int)capture.texture_pixels[3],
               (int)0xff0a0b0cu);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
    glEnable(GL_TEXTURE_2D);
    expect_int("second texture bound", ctx->current_texture != NULL, 1);
    expect_int("nearest filter applied", capture.texture_filter,
               L10GL_FILTER_NEAREST);
    expect_int("clamp applied", capture.texture_wrap, L10GL_WRAP_CLAMP);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 3, 3, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    expect_int("non-power-of-two rejected", glGetError(), GL_INVALID_VALUE);

    /* Rectangular power-of-two textures are accepted per-axis (Q3): each
     * side independently POT and <= 512. Rejected sizes return before the
     * pixel data is read, so a small buffer is safe for them. */
    {
        GLubyte rect_pixels[4 * 2 * 4];

        memset(rect_pixels, 0, sizeof(rect_pixels));
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rect_pixels);
        expect_int("4x2 rectangle accepted", glGetError(), GL_NO_ERROR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 4, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rect_pixels);
        expect_int("2x4 rectangle accepted", glGetError(), GL_NO_ERROR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 6, 2, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rect_pixels);
        expect_int("non-POT rectangle rejected", glGetError(),
                   GL_INVALID_VALUE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1024, 512, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, rect_pixels);
        expect_int("oversize rectangle rejected", glGetError(),
                   GL_INVALID_VALUE);
    }
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                    GL_LINEAR_MIPMAP_LINEAR);
    expect_int("mipmap magnification rejected", glGetError(), GL_INVALID_ENUM);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 3);
    expect_int("bad unpack alignment", glGetError(), GL_INVALID_VALUE);

    glDeleteTextures(1, &textures[1]);
    expect_int("deleted bound texture unbound", ctx->current_texture == NULL,
               1);
    expect_int("deleted texture name gone", glIsTexture(textures[1]),
               GL_FALSE);
    glDeleteTextures(2, textures);
    glDisable(GL_TEXTURE_2D);
}

static void test_clear_and_sync(struct l10gl_ctx *ctx)
{
    memset(&capture, 0, sizeof(capture));
    glClearColor(2.0f, -1.0f, 0.25f, 0.5f);
    glClearDepth(-4.0);
    expect_float("clamped clear red", ctx->clear_r, 1.0f);
    expect_float("clamped clear green", ctx->clear_g, 0.0f);
    expect_float("clamped clear depth", ctx->clear_z, 0.0f);

    glClear(GL_COLOR_BUFFER_BIT);
    expect_int("color-only clear", capture.color_clears, 1);
    expect_int("no depth clear", capture.depth_clears, 0);
    glClear(GL_DEPTH_BUFFER_BIT);
    expect_int("depth-only clear", capture.depth_clears, 1);

    glFinish();
    l10glSwapBuffers();
    expect_int("finish waits", capture.waits, 1);
    expect_int("swap dispatch", capture.swaps, 1);
}

static void test_stack_errors(void)
{
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glPopMatrix();
    expect_int("projection underflow", glGetError(), GL_STACK_UNDERFLOW);
    glPushMatrix();
    glPushMatrix();
    glPushMatrix();
    glPushMatrix();
    expect_int("projection overflow", glGetError(), GL_STACK_OVERFLOW);
}

static void expect_rgb(const char *label, const unsigned char *pixel,
                       int red, int green, int blue)
{
    if (pixel[0] == red && pixel[1] == green && pixel[2] == blue)
        return;
    fprintf(stderr, "test-gl: %s is (%u,%u,%u), expected (%d,%d,%d)\n",
            label, pixel[0], pixel[1], pixel[2], red, green, blue);
    failures++;
}

static void test_swrast_texture_pixels(void)
{
    const GLubyte pixels[16] = {
        255, 0, 0, 255,    0, 255, 0, 255,
        0, 0, 255, 255,    255, 255, 255, 255,
    };
    unsigned char image[4 * 4 * 3];
    char directory[] = "/tmp/l10gl-gl-test.XXXXXX";
    char path[PATH_MAX];
    char magic[3];
    struct l10gl_ctx ctx;
    GLuint texture;
    FILE *file;
    int width, height, maximum;

    if (!mkdtemp(directory)) {
        fprintf(stderr, "test-gl: cannot create swrast temp directory\n");
        failures++;
        return;
    }
    snprintf(path, sizeof(path), "%s/frame.ppm", directory);
    setenv("L10GL_SWRAST_DUMP", path, 1);
    if (l10gl_create(&ctx, &swrast_backend, 4, 4, 3) != 0) {
        fprintf(stderr, "test-gl: cannot create swrast texture context\n");
        failures++;
        unsetenv("L10GL_SWRAST_DUMP");
        rmdir(directory);
        return;
    }
    l10glMakeCurrent(&ctx);
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f( 1, -1);
    glTexCoord2f(1, 1); glVertex2f( 1,  1);
    glTexCoord2f(0, 1); glVertex2f(-1,  1);
    glEnd();
    l10glSwapBuffers();
    expect_int("swrast GL texture error", glGetError(), GL_NO_ERROR);

    l10glMakeCurrent(NULL);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");

    file = fopen(path, "rb");
    if (!file || fscanf(file, "%2s%d%d%d", magic, &width, &height,
                        &maximum) != 4 || strcmp(magic, "P6") != 0 ||
        width != 4 || height != 4 || maximum != 255 || fgetc(file) == EOF ||
        fread(image, sizeof(image), 1, file) != 1) {
        fprintf(stderr, "test-gl: cannot read swrast GL texture frame\n");
        failures++;
    } else {
        expect_rgb("swrast textured top-left", &image[(0 * 4 + 0) * 3],
                   0, 0, 255);
        expect_rgb("swrast textured top-right", &image[(0 * 4 + 3) * 3],
                   255, 255, 255);
        expect_rgb("swrast textured bottom-left", &image[(3 * 4 + 0) * 3],
                   255, 0, 0);
        expect_rgb("swrast textured bottom-right", &image[(3 * 4 + 3) * 3],
                   0, 255, 0);
    }
    if (file)
        fclose(file);
    unlink(path);
    rmdir(directory);
}

/* Read a 4x4 P6 PPM into out (48 bytes). Returns 0 on success. */
static int read_ppm_4x4(const char *path, unsigned char *out)
{
    char magic[3];
    int w, h, maxval;
    FILE *f = fopen(path, "rb");

    if (!f)
        return -1;
    if (fscanf(f, "%2s%d%d%d", magic, &w, &h, &maxval) != 4 ||
        strcmp(magic, "P6") != 0 || w != 4 || h != 4 || maxval != 255 ||
        fgetc(f) == EOF || fread(out, 4 * 4 * 3, 1, f) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return 0;
}

static void draw_full_textured_quad(void)
{
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glColor3f(1, 1, 1);
    glBegin(GL_QUADS);
    glTexCoord2f(0, 0); glVertex2f(-1, -1);
    glTexCoord2f(1, 0); glVertex2f( 1, -1);
    glTexCoord2f(1, 1); glVertex2f( 1,  1);
    glTexCoord2f(0, 1); glVertex2f(-1,  1);
    glEnd();
    l10glSwapBuffers();
}

/* Render a full-screen quad textured with a 4x4 image F. When via_subimage
 * is set, F is installed by uploading solid white and then rebuilding it
 * with glTexSubImage2D (two half-image subrects + one single-texel interior
 * subrect); otherwise F is uploaded whole. Both must rasterize identically. */
static int render_subimage_scene(const char *path, int via_subimage)
{
    GLubyte F[4 * 4 * 4];
    GLubyte init[4 * 4 * 4];
    GLuint tex;
    struct l10gl_ctx ctx;
    int x, y;

    for (y = 0; y < 4; y++)
        for (x = 0; x < 4; x++) {
            GLubyte *t = &F[(y * 4 + x) * 4];
            t[0] = (GLubyte)(x * 60);
            t[1] = (GLubyte)(y * 60);
            t[2] = (GLubyte)((x + y) * 30);
            t[3] = 255;
        }
    memset(init, 0xff, sizeof(init));

    setenv("L10GL_SWRAST_DUMP", path, 1);
    if (l10gl_create(&ctx, &swrast_backend, 4, 4, 3) != 0) {
        unsetenv("L10GL_SWRAST_DUMP");
        return -1;
    }
    l10glMakeCurrent(&ctx);
    glViewport(0, 0, 4, 4);
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();

    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    if (!via_subimage) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, F);
    } else {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 4, 4, 0,
                     GL_RGBA, GL_UNSIGNED_BYTE, init);
        /* Rebuild F via subrectangles covering the whole image. */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 4, 2,
                        GL_RGBA, GL_UNSIGNED_BYTE, F);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 2, 4, 2,
                        GL_RGBA, GL_UNSIGNED_BYTE, F + 2 * 4 * 4);
        /* An interior single-texel subrect (idempotent here) exercises the
         * 1x1 path without changing the result. */
        glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 1, 1, 1,
                        GL_RGBA, GL_UNSIGNED_BYTE, &F[(1 * 4 + 1) * 4]);
    }
    draw_full_textured_quad();

    glDeleteTextures(1, &tex);
    l10glMakeCurrent(NULL);
    l10gl_destroy(&ctx);
    unsetenv("L10GL_SWRAST_DUMP");
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static void test_swrast_subimage_equivalence(void)
{
    char directory[] = "/tmp/l10gl-gl-sub.XXXXXX";
    char whole_path[PATH_MAX];
    char sub_path[PATH_MAX];
    unsigned char whole[4 * 4 * 3];
    unsigned char sub[4 * 4 * 3];

    if (!mkdtemp(directory)) {
        fprintf(stderr, "test-gl: cannot create subimage temp directory\n");
        failures++;
        return;
    }
    snprintf(whole_path, sizeof(whole_path), "%s/whole.ppm", directory);
    snprintf(sub_path, sizeof(sub_path), "%s/sub.ppm", directory);

    if (render_subimage_scene(whole_path, 0) ||
        render_subimage_scene(sub_path, 1)) {
        fprintf(stderr, "test-gl: subimage equivalence render failed\n");
        failures++;
    } else if (read_ppm_4x4(whole_path, whole) ||
               read_ppm_4x4(sub_path, sub)) {
        fprintf(stderr, "test-gl: cannot read subimage equivalence frames\n");
        failures++;
    } else if (memcmp(whole, sub, sizeof(whole)) != 0) {
        fprintf(stderr,
                "test-gl: subimage scene differs from whole-upload scene\n");
        failures++;
    }

    unlink(whole_path);
    unlink(sub_path);
    rmdir(directory);
}

static void test_quake_entry_points(struct l10gl_ctx *ctx)
{
    const GLubyte *s;
    GLfloat m[16];
    GLuint tex;
    const GLubyte bytecolor[3] = {255, 0, 128};

    (void)ctx;
    memset(&capture, 0, sizeof(capture));

    /* glGetString identifies backend and honest pre-1.1 tier. */
    s = glGetString(GL_VENDOR);
    expect_int("vendor string present", s != NULL, 1);
    s = glGetString(GL_RENDERER);
    expect_int("renderer names backend",
               strstr((const char *)s, "gl-capture") != NULL, 1);
    s = glGetString(GL_VERSION);
    expect_int("version reports 1.0", strstr((const char *)s, "1.0") != NULL,
               1);
    expect_int("version does not claim 1.1",
               strstr((const char *)s, "1.1") == NULL, 1);
    s = glGetString(GL_EXTENSIONS);
    expect_int("extensions empty", s[0] == '\0', 1);
    glGetString(0x1234);
    expect_int("bad string name", glGetError(), GL_INVALID_ENUM);

    /* glGetFloatv serves the modelview capture GLQuake needs. */
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    expect_float("identity m[0]", m[0], 1.0f);
    expect_float("identity m[12]", m[12], 0.0f);
    glTranslatef(1.0f, 2.0f, 3.0f);
    glGetFloatv(GL_MODELVIEW_MATRIX, m);
    expect_float("translation m[12]", m[12], 1.0f);
    expect_float("translation m[13]", m[13], 2.0f);
    expect_float("translation m[14]", m[14], 3.0f);
    glGetFloatv(GL_LIGHT0, m);
    expect_int("unsupported query pname", glGetError(), GL_INVALID_ENUM);

    /* glHint accepts the perspective target and rejects the rest. */
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, GL_FASTEST);
    expect_int("hint accepted", glGetError(), GL_NO_ERROR);
    glHint(0x1234, GL_NICEST);
    expect_int("bad hint target", glGetError(), GL_INVALID_ENUM);
    glHint(GL_PERSPECTIVE_CORRECTION_HINT, 0x1234);
    expect_int("bad hint mode", glGetError(), GL_INVALID_ENUM);

    /* glPolygonMode accepts only GL_FILL. */
    glPolygonMode(GL_FRONT_AND_BACK, GL_FILL);
    expect_int("polygon fill accepted", glGetError(), GL_NO_ERROR);
    glPolygonMode(GL_FRONT_AND_BACK, GL_LINE);
    expect_int("polygon line unsupported", glGetError(), GL_INVALID_ENUM);

    /* glDrawBuffer accepts GL_BACK only. */
    glDrawBuffer(GL_BACK);
    expect_int("draw back accepted", glGetError(), GL_NO_ERROR);
    glDrawBuffer(GL_FRONT);
    expect_int("draw front unsupported", glGetError(),
               GL_INVALID_OPERATION);

    /* glReadBuffer accepts GL_FRONT/GL_BACK. */
    glReadBuffer(GL_BACK);
    expect_int("read back accepted", glGetError(), GL_NO_ERROR);
    glReadBuffer(0x1234);
    expect_int("bad read buffer", glGetError(), GL_INVALID_ENUM);

    /* glReadPixels is a documented stub. */
    glReadPixels(0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, m);
    expect_int("readpixels unsupported", glGetError(), GL_INVALID_OPERATION);

    /* glAlphaFunc records state; GL default (ALWAYS,0) is a no-op pass. */
    glAlphaFunc(GL_GREATER, 0.666f);
    expect_int("alpha func accepted", glGetError(), GL_NO_ERROR);
    glAlphaFunc(0x1234, 0.5f);
    expect_int("bad alpha func", glGetError(), GL_INVALID_ENUM);

    /* glTexEnvf records env mode for the Q6 modes; BLEND env is deferred. */
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    expect_int("texenv replace accepted", glGetError(), GL_NO_ERROR);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_BLEND);
    expect_int("texenv blend deferred", glGetError(), GL_INVALID_ENUM);
    glTexEnvf(0x1234, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    expect_int("bad texenv target", glGetError(), GL_INVALID_ENUM);

    /* glColor3ubv converts bytes to float in [0,1] (particles). */
    glDisable(GL_TEXTURE_2D);
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    memset(&capture, 0, sizeof(capture));
    glColor3ubv(bytecolor);
    glBegin(GL_TRIANGLES);
    glVertex2f(-.25f, -.25f);
    glVertex2f(.25f, -.25f);
    glVertex2f(0.0f, .25f);
    glEnd();
    expect_int("byte-color triangle drawn", capture.triangles, 1);
    expect_float("byte-color red", capture.triangle[0].r, 1.0f);
    expect_float("byte-color green", capture.triangle[0].g, 0.0f);
    expect_float("byte-color blue", capture.triangle[0].b, 128.0f / 255.0f);
    glColor3ubv(NULL);
    expect_int("null byte color", glGetError(), GL_INVALID_VALUE);

    /* glTexParameterf delegates to the integer path and reaches the backend. */
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE,
                 (const GLubyte[16]){0});
    glEnable(GL_TEXTURE_2D);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    expect_int("TexParameterf delegates", glGetError(), GL_NO_ERROR);
    expect_int("TexParameterf applies linear",
               capture.texture_filter == L10GL_FILTER_LINEAR, 1);
    glDisable(GL_TEXTURE_2D);
    glDeleteTextures(1, &tex);
}

static void test_texture_subimage(struct l10gl_ctx *ctx)
{
    GLuint tex;
    int uploads;
    /* Base 2x2 RGBA image: A B / C D (grays). */
    static const uint8_t base[16] = {
        0x11, 0x11, 0x11, 0xff,  0x22, 0x22, 0x22, 0xff,
        0x33, 0x33, 0x33, 0xff,  0x44, 0x44, 0x44, 0xff,
    };
    /* A 1x1 red subrectangle (RGB source) to drop at (1,0) -> index 1. */
    static const uint8_t red[3] = { 255, 0, 0 };

    (void)ctx;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 2, 2, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, base);
    expect_int("subimage base upload ok", glGetError(), GL_NO_ERROR);
    expect_int("subimage base texel0", (int)capture.texture_pixels[0],
               (int)0xff111111u);
    expect_int("subimage base texel1", (int)capture.texture_pixels[1],
               (int)0xff222222u);
    expect_int("subimage base texel2", (int)capture.texture_pixels[2],
               (int)0xff333333u);
    expect_int("subimage base texel3", (int)capture.texture_pixels[3],
               (int)0xff444444u);

    /* Interior 1x1 subrect: one re-upload, only the touched texel changes. */
    uploads = capture.texture_uploads;
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, red);
    expect_int("subimage ok", glGetError(), GL_NO_ERROR);
    expect_int("subimage re-uploads once", capture.texture_uploads, uploads + 1);
    expect_int("subimage updated texel", (int)capture.texture_pixels[1],
               (int)0xffff0000u);
    expect_int("subimage left texel0", (int)capture.texture_pixels[0],
               (int)0xff111111u);
    expect_int("subimage left texel3", (int)capture.texture_pixels[3],
               (int)0xff444444u);

    /* Edge subrect covering the whole bottom row (y=1). */
    {
        static const uint8_t row[6] = { 5, 6, 7, 8, 9, 10 };

        uploads = capture.texture_uploads;
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 1, 2, 1, GL_RGB, GL_UNSIGNED_BYTE,
                        row);
        expect_int("edge subimage ok", glGetError(), GL_NO_ERROR);
        expect_int("edge re-uploads once", capture.texture_uploads, uploads + 1);
        expect_int("edge updated texel2", (int)capture.texture_pixels[2],
                   (int)0xff050607u);
        expect_int("edge updated texel3", (int)capture.texture_pixels[3],
                   (int)0xff08090au);
    }

    /* Unpack alignment: a 2x2 RGB subrect whose 6-byte rows pad to 8. The
     * second source row must begin at byte offset 8, not 6. */
    {
        static const uint8_t padded[16] = {
            10, 20, 30, 40, 50, 60,  0xff, 0xff,
            70, 80, 90, 100, 110, 120, 0, 0,
        };

        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 2, 2, GL_RGB, GL_UNSIGNED_BYTE,
                        padded);
        expect_int("aligned subimage ok", glGetError(), GL_NO_ERROR);
        expect_int("aligned row0 texel0", (int)capture.texture_pixels[0],
                   (int)0xff0a141eu);
        expect_int("aligned row0 texel1", (int)capture.texture_pixels[1],
                   (int)0xff28323cu);
        expect_int("aligned row1 texel0", (int)capture.texture_pixels[2],
                   (int)0xff46505au);
        expect_int("aligned row1 texel1", (int)capture.texture_pixels[3],
                   (int)0xff646e78u);
    }

    /* Error cases. GL_TEXTURE_1D (0x0DE0) is not in the shim's enum set. */
    glTexSubImage2D((GLenum)0x0DE0, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, red);
    expect_int("subimage bad target", glGetError(), GL_INVALID_ENUM);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_LUMINANCE,
                    GL_UNSIGNED_BYTE, red);
    expect_int("subimage bad format", glGetError(), GL_INVALID_ENUM);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB, GL_FLOAT, red);
    expect_int("subimage bad type", glGetError(), GL_INVALID_ENUM);
    glTexSubImage2D(GL_TEXTURE_2D, 0, -1, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, red);
    expect_int("subimage negative offset", glGetError(), GL_INVALID_VALUE);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 1, 0, 2, 1, GL_RGB, GL_UNSIGNED_BYTE, red);
    expect_int("subimage out of bounds", glGetError(), GL_INVALID_VALUE);

    glDeleteTextures(1, &tex);
    /* With the bound texture deleted there is no retained level to update. */
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGB, GL_UNSIGNED_BYTE, red);
    expect_int("subimage without texture", glGetError(), GL_INVALID_OPERATION);
}

int main(void)
{
    struct l10gl_ctx ctx;

    test_no_context();
    if (l10gl_create(&ctx, &capture_backend, 100, 80, 4) != 0) {
        fprintf(stderr, "test-gl: failed to create capture context\n");
        return 1;
    }
    l10glMakeCurrent(&ctx);
    expect_int("current context installed",
               l10glGetCurrentContext() == &ctx, 1);

    test_immediate_and_matrices(&ctx);
    test_state(&ctx);
    test_lighting_material(&ctx);
    test_shade_model(&ctx);
    test_texture_objects(&ctx);
    test_texture_subimage(&ctx);
    test_quake_entry_points(&ctx);
    test_clear_and_sync(&ctx);
    test_stack_errors();

    l10glMakeCurrent(NULL);
    l10gl_destroy(&ctx);
    test_swrast_texture_pixels();
    test_swrast_subimage_equivalence();

    if (failures) {
        fprintf(stderr, "test-gl: %d failure(s)\n", failures);
        return 1;
    }
    printf("test-gl: all tests passed\n");
    return 0;
}
