/* Capture-backend regression tests for X2 immediate primitive assembly. */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "l10gl.h"

#define MAX_CAPTURED 16
#define EPSILON 1.0e-5f

struct captured_triangle {
    struct l10gl_vertex v[3];
    int textured;
};

struct captured_line {
    struct l10gl_vertex v[2];
};

struct capture_state {
    struct captured_triangle triangles[MAX_CAPTURED];
    struct captured_line lines[MAX_CAPTURED];
    int triangle_count;
    int line_count;
};

static struct capture_state capture;
static int failures;

static void reset_capture(void)
{
    memset(&capture, 0, sizeof(capture));
}

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-pipeline: %s is %d, expected %d\n",
            label, actual, expected);
    failures++;
}

static void expect_float(const char *label, float actual, float expected)
{
    if (fabsf(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-pipeline: %s is %.9g, expected %.9g\n",
            label, actual, expected);
    failures++;
}

static int capture_init(struct l10gl_ctx *ctx, int width, int height, int bpp)
{
    (void)bpp;
    ctx->width = width;
    ctx->height = height;
    ctx->backend_data = &capture;
    return 0;
}

static void capture_cleanup(struct l10gl_ctx *ctx)
{
    ctx->backend_data = NULL;
}

static void capture_triangle(struct l10gl_ctx *ctx,
                             struct l10gl_vertex v0,
                             struct l10gl_vertex v1,
                             struct l10gl_vertex v2)
{
    struct captured_triangle *triangle;
    (void)ctx;

    if (capture.triangle_count >= MAX_CAPTURED)
        return;
    triangle = &capture.triangles[capture.triangle_count++];
    triangle->v[0] = v0;
    triangle->v[1] = v1;
    triangle->v[2] = v2;
    triangle->textured = 0;
}

static void capture_textured_triangle(struct l10gl_ctx *ctx,
                                      struct l10gl_vertex v0,
                                      struct l10gl_vertex v1,
                                      struct l10gl_vertex v2)
{
    int before = capture.triangle_count;

    capture_triangle(ctx, v0, v1, v2);
    if (capture.triangle_count > before)
        capture.triangles[capture.triangle_count - 1].textured = 1;
}

static void capture_line(struct l10gl_ctx *ctx,
                         struct l10gl_vertex v0,
                         struct l10gl_vertex v1)
{
    struct captured_line *line;
    (void)ctx;

    if (capture.line_count >= MAX_CAPTURED)
        return;
    line = &capture.lines[capture.line_count++];
    line->v[0] = v0;
    line->v[1] = v1;
}

static const struct l10gl_backend capture_backend = {
    .name = "capture",
    .init = capture_init,
    .cleanup = capture_cleanup,
    .draw_triangle = capture_triangle,
    .draw_textured_triangle = capture_textured_triangle,
    .draw_line = capture_line,
    .caps = L10GL_CAP_GOURAUD | L10GL_CAP_LINES | L10GL_CAP_TEXTURE,
};

static void submit_colored_vertex(struct l10gl_ctx *ctx, float x, float y,
                                  float z, float id)
{
    l10gl_color4f(ctx, id, id + .01f, id + .02f, id + .03f);
    l10gl_texcoord2f(ctx, id + .04f, id + .05f);
    expect_int("vertex submission", l10gl_vertex3f(ctx, x, y, z), 0);
}

static void test_errors_and_defaults(struct l10gl_ctx *ctx)
{
    expect_float("default red", ctx->current_r, 1);
    expect_float("default green", ctx->current_g, 1);
    expect_float("default blue", ctx->current_b, 1);
    expect_float("default alpha", ctx->current_a, 1);
    expect_float("default normal X", ctx->current_nx, 0);
    expect_float("default normal Y", ctx->current_ny, 0);
    expect_float("default normal Z", ctx->current_nz, 1);
    expect_int("default culling", ctx->cull_mode_val, L10GL_CULL_NONE);
    expect_int("lighting disabled by default", ctx->lighting_enabled, 0);
    expect_float("default light direction X", ctx->light_dir_x, 0);
    expect_float("default light direction Y", ctx->light_dir_y, 0);
    expect_float("default light direction Z", ctx->light_dir_z, -1);
    expect_float("default diffuse light", ctx->light_r, .8f);
    expect_float("default ambient light", ctx->ambient_r, .2f);
    expect_float("default material red", ctx->material_r, 1);
    expect_float("default material alpha", ctx->material_a, 1);

    expect_int("vertex outside begin", l10gl_vertex3f(ctx, 0, 0, 0), -EPERM);
    expect_int("end outside begin", l10gl_end(ctx), -EPERM);
    expect_int("points unsupported", l10gl_begin(ctx, L10GL_POINTS), -ENOTSUP);
    expect_int("invalid cull mode",
               l10gl_cull_face(ctx, (enum l10gl_cull_mode)99), -EINVAL);

    reset_capture();
    expect_int("begin triangles", l10gl_begin(ctx, L10GL_TRIANGLES), 0);
    expect_int("nested begin", l10gl_begin(ctx, L10GL_LINES), -EBUSY);
    l10gl_vertex3f(ctx, 0, 0, 0);
    l10gl_vertex3f(ctx, .5f, 0, 0);
    expect_int("end incomplete triangle", l10gl_end(ctx), 0);
    expect_int("incomplete triangle ignored", capture.triangle_count, 0);

    l10gl_normal3f(ctx, 2, 3, 4);
    expect_float("current normal X", ctx->current_nx, 2);
    expect_float("current normal Y", ctx->current_ny, 3);
    expect_float("current normal Z", ctx->current_nz, 4);
}

static void test_triangle_transform_and_attributes(struct l10gl_ctx *ctx)
{
    const struct captured_triangle *triangle;

    reset_capture();
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    l10gl_viewport(ctx, 0, 0, 100, 80);
    l10gl_depth_range(ctx, 0, 1);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    l10gl_bind_texture(ctx, NULL);

    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    l10gl_end(ctx);

    expect_int("one transformed triangle", capture.triangle_count, 1);
    triangle = &capture.triangles[0];
    expect_int("plain triangle dispatch", triangle->textured, 0);
    expect_float("v0 screen x", triangle->v[0].x, 25);
    expect_float("v0 screen y", triangle->v[0].y, 60);
    expect_float("v1 screen x", triangle->v[1].x, 75);
    expect_float("v1 screen y", triangle->v[1].y, 60);
    expect_float("v2 screen x", triangle->v[2].x, 50);
    expect_float("v2 screen y", triangle->v[2].y, 20);
    expect_float("window depth", triangle->v[0].z, .5f);
    expect_float("X2 affine W", triangle->v[0].w, 1);
    expect_float("captured v0 red", triangle->v[0].r, .1f);
    expect_float("captured v1 green", triangle->v[1].g, .21f);
    expect_float("captured v2 alpha", triangle->v[2].a, .33f);
    expect_float("captured v0 U", triangle->v[0].u, .14f);
    expect_float("captured v2 V", triangle->v[2].v, .35f);
}

static void test_textured_dispatch(struct l10gl_ctx *ctx)
{
    struct l10gl_texture texture = { .width = 1, .height = 1 };

    reset_capture();
    l10gl_bind_texture(ctx, &texture);
    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    l10gl_end(ctx);
    expect_int("one textured triangle", capture.triangle_count, 1);
    expect_int("textured triangle dispatch", capture.triangles[0].textured, 1);
    l10gl_bind_texture(ctx, NULL);
}

static void test_modelview_connection(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_translatef(ctx, .25f, 0, 0);
    l10gl_scalef(ctx, .5f, .5f, 1);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);

    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, -1, -1, 0);
    l10gl_vertex3f(ctx,  1, -1, 0);
    l10gl_vertex3f(ctx,  0,  1, 0);
    l10gl_end(ctx);
    expect_int("modelview triangle count", capture.triangle_count, 1);
    expect_float("modelview transformed left X",
                 capture.triangles[0].v[0].x, 37.5f);
    expect_float("modelview transformed right X",
                 capture.triangles[0].v[1].x, 87.5f);
    expect_float("modelview transformed bottom Y",
                 capture.triangles[0].v[0].y, 60);
    expect_float("modelview transformed top Y",
                 capture.triangles[0].v[2].y, 20);

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
}

static void test_strip_and_fan_assembly(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_BACK);
    l10gl_begin(ctx, L10GL_TRIANGLE_STRIP);
    submit_colored_vertex(ctx, -.5f, -.5f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, -.5f, 0, .2f);
    submit_colored_vertex(ctx, -.5f,  .5f, 0, .3f);
    submit_colored_vertex(ctx,  .5f,  .5f, 0, .4f);
    l10gl_end(ctx);
    expect_int("strip triangle count", capture.triangle_count, 2);
    expect_float("strip t0 order 0", capture.triangles[0].v[0].r, .1f);
    expect_float("strip t0 order 1", capture.triangles[0].v[1].r, .2f);
    expect_float("strip t0 order 2", capture.triangles[0].v[2].r, .3f);
    expect_float("strip t1 alternating 0", capture.triangles[1].v[0].r, .3f);
    expect_float("strip t1 alternating 1", capture.triangles[1].v[1].r, .2f);
    expect_float("strip t1 alternating 2", capture.triangles[1].v[2].r, .4f);

    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLE_FAN);
    submit_colored_vertex(ctx,  0.0f, 0.0f, 0, .1f);
    submit_colored_vertex(ctx,  .5f, 0.0f, 0, .2f);
    submit_colored_vertex(ctx,  0.0f, .5f, 0, .3f);
    submit_colored_vertex(ctx, -.5f, 0.0f, 0, .4f);
    l10gl_end(ctx);
    expect_int("fan triangle count", capture.triangle_count, 2);
    expect_float("fan fixed origin t0", capture.triangles[0].v[0].r, .1f);
    expect_float("fan fixed origin t1", capture.triangles[1].v[0].r, .1f);
    expect_float("fan t1 previous", capture.triangles[1].v[1].r, .3f);
    expect_float("fan t1 new", capture.triangles[1].v[2].r, .4f);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
}

static void test_line_assembly(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_begin(ctx, L10GL_LINES);
    for (int i = 0; i < 5; i++)
        submit_colored_vertex(ctx, -.8f + i * .3f, 0, 0, .1f * (i + 1));
    l10gl_end(ctx);
    expect_int("independent line count", capture.line_count, 2);
    expect_float("line 0 start", capture.lines[0].v[0].r, .1f);
    expect_float("line 0 end", capture.lines[0].v[1].r, .2f);
    expect_float("line 1 start", capture.lines[1].v[0].r, .3f);
    expect_float("line 1 end", capture.lines[1].v[1].r, .4f);

    reset_capture();
    l10gl_begin(ctx, L10GL_LINE_STRIP);
    for (int i = 0; i < 4; i++)
        submit_colored_vertex(ctx, -.6f + i * .4f, 0, 0, .1f * (i + 1));
    l10gl_end(ctx);
    expect_int("line strip count", capture.line_count, 3);
    expect_float("line strip shared end", capture.lines[1].v[0].r, .2f);
    expect_float("line strip shared start", capture.lines[1].v[1].r, .3f);
}

static void submit_test_triangle(struct l10gl_ctx *ctx, int clockwise,
                                 float z)
{
    l10gl_begin(ctx, L10GL_TRIANGLES);
    if (!clockwise) {
        l10gl_vertex3f(ctx, -.5f, -.5f, z);
        l10gl_vertex3f(ctx,  .5f, -.5f, z);
    } else {
        l10gl_vertex3f(ctx,  .5f, -.5f, z);
        l10gl_vertex3f(ctx, -.5f, -.5f, z);
    }
    l10gl_vertex3f(ctx, 0, .5f, z);
    l10gl_end(ctx);
}

static void test_culling_and_clip_rejection(struct l10gl_ctx *ctx)
{
    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_BACK);
    submit_test_triangle(ctx, 0, 0);
    submit_test_triangle(ctx, 1, 0);
    expect_int("back-face culling", capture.triangle_count, 1);

    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_FRONT);
    submit_test_triangle(ctx, 0, 0);
    submit_test_triangle(ctx, 1, 0);
    expect_int("front-face culling", capture.triangle_count, 1);
    /* The surviving clockwise triangle becomes clockwise only after the
     * backend-coordinate Y flip; culling was correctly done in NDC. */
    expect_float("front cull surviving first x",
                 capture.triangles[0].v[0].x, 75);

    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    submit_test_triangle(ctx, 0, -2);
    submit_test_triangle(ctx, 0, 2);
    expect_int("clip-depth rejection", capture.triangle_count, 0);

    reset_capture();
    submit_test_triangle(ctx, 0, 0);
    expect_int("valid triangle after rejection", capture.triangle_count, 1);
}

static void test_near_plane_clipping(struct l10gl_ctx *ctx)
{
    const struct captured_triangle *first;

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    expect_int("clipping perspective setup",
               l10gl_perspective(ctx, 90, 1, 1, 10), 0);
    l10gl_viewport(ctx, 0, 0, 100, 80);
    l10gl_depth_range(ctx, 0, 1);
    l10gl_cull_face(ctx, L10GL_CULL_BACK);

    /* One vertex is between the eye and near plane. The clipped quad is
     * emitted as two CCW triangles. Perspective makes clip W vary, so the
     * expected intersections also verify homogeneous W interpolation. */
    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx,  0,  .5f, -.5f, .1f);
    submit_colored_vertex(ctx, -1, -1.0f, -2.0f, .3f);
    submit_colored_vertex(ctx,  1, -1.0f, -2.0f, .5f);
    l10gl_end(ctx);
    expect_int("one-outside clips to two triangles", capture.triangle_count, 2);
    first = &capture.triangles[0];
    expect_float("near intersection right X", first->v[0].x, 66.6666667f);
    expect_float("near intersection right Y", first->v[0].y, 40);
    expect_float("near intersection left X", first->v[1].x, 33.3333333f);
    expect_float("near intersection left Y", first->v[1].y, 40);
    expect_float("near intersection depth 0", first->v[0].z, 0);
    expect_float("right intersection color", first->v[0].r, .233333333f);
    expect_float("left intersection color", first->v[1].r, .166666667f);
    expect_float("right intersection U", first->v[0].u, .273333333f);
    expect_float("left intersection U", first->v[1].u, .206666667f);
    expect_float("quad fan shared vertex X",
                 capture.triangles[1].v[0].x, first->v[0].x);
    expect_float("quad fan shared source X",
                 capture.triangles[1].v[1].x, first->v[2].x);

    /* Two outside vertices leave one triangle; three leave none. */
    reset_capture();
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    l10gl_begin(ctx, L10GL_TRIANGLES);
    submit_colored_vertex(ctx, 0, -1, -2, .1f);
    submit_colored_vertex(ctx, -1, 1, -.5f, .3f);
    submit_colored_vertex(ctx, 1, 1, -.5f, .5f);
    l10gl_end(ctx);
    expect_int("two-outside clips to one triangle", capture.triangle_count, 1);

    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, -.5f, -.5f, -.5f);
    l10gl_vertex3f(ctx,  .5f, -.5f, -.5f);
    l10gl_vertex3f(ctx,  0.0f, .5f, -.5f);
    l10gl_end(ctx);
    expect_int("all outside clips to zero triangles", capture.triangle_count, 0);

    /* A vertex exactly on the near plane is inside and remains at depth 0. */
    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, 0, .5f, -1);
    l10gl_vertex3f(ctx, -1, -1, -2);
    l10gl_vertex3f(ctx, 1, -1, -2);
    l10gl_end(ctx);
    expect_int("near-boundary triangle retained", capture.triangle_count, 1);
    expect_float("near-boundary depth", capture.triangles[0].v[0].z, 0);

    /* Far clipping is intentionally conservative and line clipping has not
     * landed: either crossing is rejected whole rather than producing bad Z. */
    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, 0, .5f, -20);
    l10gl_vertex3f(ctx, -1, -1, -2);
    l10gl_vertex3f(ctx, 1, -1, -2);
    l10gl_end(ctx);
    expect_int("far crossing rejected", capture.triangle_count, 0);

    reset_capture();
    l10gl_begin(ctx, L10GL_LINES);
    l10gl_vertex3f(ctx, 0, 0, -.5f);
    l10gl_vertex3f(ctx, 0, 0, -2);
    l10gl_end(ctx);
    expect_int("near-crossing line conservatively rejected", capture.line_count, 0);

    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
}

static void test_triangle_scan_guard(struct l10gl_ctx *ctx)
{
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);

    reset_capture();
    l10gl_viewport(ctx, 0, 0, 100, 2047);
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, -.5f, -1, 0);
    l10gl_vertex3f(ctx,  .5f, -1, 0);
    l10gl_vertex3f(ctx,  0.0f,  1, 0);
    l10gl_end(ctx);
    expect_int("2047-line triangle retained", capture.triangle_count, 1);

    reset_capture();
    l10gl_viewport(ctx, 0, 0, 100, 2048);
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_vertex3f(ctx, -.5f, -1, 0);
    l10gl_vertex3f(ctx,  .5f, -1, 0);
    l10gl_vertex3f(ctx,  0.0f,  1, 0);
    l10gl_end(ctx);
    expect_int("oversized triangle rejected", capture.triangle_count, 0);

    l10gl_viewport(ctx, 0, 0, 100, 80);
}

static void submit_lighting_triangle(struct l10gl_ctx *ctx)
{
    expect_int("lighting begin", l10gl_begin(ctx, L10GL_TRIANGLES), 0);
    expect_int("lighting vertex 0", l10gl_vertex3f(ctx, -.25f, -.25f, 0), 0);
    expect_int("lighting vertex 1", l10gl_vertex3f(ctx,  .25f, -.25f, 0), 0);
    expect_int("lighting vertex 2", l10gl_vertex3f(ctx,  0.0f,  .25f, 0), 0);
    expect_int("lighting end", l10gl_end(ctx), 0);
}

static void test_directional_lighting(struct l10gl_ctx *ctx)
{
    const struct captured_triangle *triangle;

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    l10gl_viewport(ctx, 0, 0, 100, 80);
    l10gl_cull_face(ctx, L10GL_CULL_NONE);
    l10gl_bind_texture(ctx, NULL);

    expect_int("normalize light direction",
               l10gl_light_dir(ctx, 0, 0, -5), 0);
    expect_float("normalized light direction Z", ctx->light_dir_z, -1);
    expect_int("reject zero light direction",
               l10gl_light_dir(ctx, 0, 0, 0), -EINVAL);
    expect_float("invalid direction preserves state", ctx->light_dir_z, -1);
    expect_int("reject non-finite light direction",
               l10gl_light_dir(ctx, NAN, 0, -1), -EINVAL);

    l10gl_enable_lighting(ctx, 1);
    l10gl_light_ambient(ctx, .1f, .1f, .1f);
    l10gl_light_color(ctx, .6f, .6f, .6f);
    l10gl_material(ctx, .5f, .25f, 2.0f, .75f);
    l10gl_color4f(ctx, 0, 0, 0, 0); /* Lighting uses material, not color. */
    l10gl_normal3f(ctx, 0, 0, 7);   /* Normalization is automatic. */

    reset_capture();
    submit_lighting_triangle(ctx);
    expect_int("lit triangle count", capture.triangle_count, 1);
    triangle = &capture.triangles[0];
    expect_float("lit material red", triangle->v[0].r, .35f);
    expect_float("lit material green", triangle->v[0].g, .175f);
    expect_float("lit blue clamps", triangle->v[0].b, 1);
    expect_float("material alpha", triangle->v[0].a, .75f);

    /* A normal facing away from the light receives ambient only. */
    l10gl_normal3f(ctx, 0, 0, -1);
    reset_capture();
    submit_lighting_triangle(ctx);
    expect_float("back-facing ambient red", capture.triangles[0].v[0].r,
                 .05f);
    expect_float("back-facing ambient green", capture.triangles[0].v[0].g,
                 .025f);
    expect_float("back-facing ambient blue", capture.triangles[0].v[0].b,
                 .2f);

    /* Inverse-transpose is required here: scale(2,1,1) maps normal (1,1,0)
     * to normalized (.5,1,0), exactly opposite light ray (-1,-2,0). A plain
     * MODELVIEW vector transform would produce only 0.8 diffuse. */
    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_scalef(ctx, 2, 1, 1);
    expect_int("scaled-normal light direction",
               l10gl_light_dir(ctx, -1, -2, 0), 0);
    l10gl_light_ambient(ctx, 0, 0, 0);
    l10gl_light_color(ctx, 1, 1, 1);
    l10gl_material(ctx, 1, 1, 1, 1);
    l10gl_normal3f(ctx, 1, 1, 0);
    reset_capture();
    submit_lighting_triangle(ctx);
    expect_float("inverse-transpose diffuse", capture.triangles[0].v[0].r,
                 1);

    /* A reflected MODELVIEW has a negative determinant; normal orientation
     * must follow the reflection rather than merely using unsigned cofactors. */
    l10gl_load_identity(ctx);
    l10gl_scalef(ctx, -1, 1, 1);
    expect_int("reflected-normal light direction",
               l10gl_light_dir(ctx, 1, 0, 0), 0);
    l10gl_normal3f(ctx, 1, 0, 0);
    reset_capture();
    submit_lighting_triangle(ctx);
    expect_float("reflected normal diffuse", capture.triangles[0].v[0].r, 1);

    /* A singular normal matrix is safe and deterministically yields ambient. */
    l10gl_load_identity(ctx);
    l10gl_scalef(ctx, 1, 1, 0);
    l10gl_light_ambient(ctx, .2f, .3f, .4f);
    l10gl_normal3f(ctx, 0, 0, 1);
    reset_capture();
    submit_lighting_triangle(ctx);
    expect_int("singular normal triangle", capture.triangle_count, 1);
    expect_float("singular normal ambient red", capture.triangles[0].v[0].r,
                 .2f);
    expect_float("singular normal ambient green", capture.triangles[0].v[0].g,
                 .3f);
    expect_float("singular normal ambient blue", capture.triangles[0].v[0].b,
                 .4f);

    /* Material changes are captured per vertex, just like other attributes. */
    l10gl_load_identity(ctx);
    expect_int("material capture light direction",
               l10gl_light_dir(ctx, 0, 0, -1), 0);
    l10gl_light_ambient(ctx, 0, 0, 0);
    l10gl_light_color(ctx, 1, 1, 1);
    l10gl_normal3f(ctx, 0, 0, 1);
    reset_capture();
    l10gl_begin(ctx, L10GL_TRIANGLES);
    l10gl_material(ctx, 1, 0, 0, .25f);
    l10gl_vertex3f(ctx, -.25f, -.25f, 0);
    l10gl_material(ctx, 0, 1, 0, .5f);
    l10gl_vertex3f(ctx, .25f, -.25f, 0);
    l10gl_material(ctx, 0, 0, 1, .75f);
    l10gl_vertex3f(ctx, 0, .25f, 0);
    l10gl_end(ctx);
    expect_float("captured red material", capture.triangles[0].v[0].r, 1);
    expect_float("captured green material", capture.triangles[0].v[1].g, 1);
    expect_float("captured blue material", capture.triangles[0].v[2].b, 1);
    expect_float("captured material alpha", capture.triangles[0].v[1].a,
                 .5f);

    /* Disabling lighting restores the established current-color path. */
    l10gl_enable_lighting(ctx, 0);
    l10gl_color4f(ctx, .2f, .3f, .4f, .6f);
    reset_capture();
    submit_lighting_triangle(ctx);
    expect_float("disabled lighting red", capture.triangles[0].v[0].r, .2f);
    expect_float("disabled lighting alpha", capture.triangles[0].v[0].a, .6f);

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
}

int main(void)
{
    struct l10gl_ctx ctx;

    reset_capture();
    if (l10gl_create(&ctx, &capture_backend, 100, 80, 3) < 0) {
        fprintf(stderr, "test-pipeline: failed to create capture context\n");
        return 1;
    }

    test_errors_and_defaults(&ctx);
    test_triangle_transform_and_attributes(&ctx);
    test_textured_dispatch(&ctx);
    test_modelview_connection(&ctx);
    test_strip_and_fan_assembly(&ctx);
    test_line_assembly(&ctx);
    test_culling_and_clip_rejection(&ctx);
    test_near_plane_clipping(&ctx);
    test_triangle_scan_guard(&ctx);
    test_directional_lighting(&ctx);
    l10gl_destroy(&ctx);

    if (failures) {
        fprintf(stderr, "test-pipeline: FAILED (%d checks)\n", failures);
        return 1;
    }
    printf("test-pipeline: PASS (attributes, assembly, transforms, culling, "
           "near clipping, interpolation, scan guard, lighting)\n");
    return 0;
}
