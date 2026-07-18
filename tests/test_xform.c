/* Deterministic regression tests for X1 matrix stacks and viewport math. */

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "l10gl.h"

#define EPSILON 1.0e-5f

static int failures;

static void expect_int(const char *label, int actual, int expected)
{
    if (actual == expected)
        return;
    fprintf(stderr, "test-xform: %s is %d, expected %d\n",
            label, actual, expected);
    failures++;
}

static void expect_float(const char *label, float actual, float expected)
{
    if (fabsf(actual - expected) <= EPSILON)
        return;
    fprintf(stderr, "test-xform: %s is %.9g, expected %.9g\n",
            label, actual, expected);
    failures++;
}

static void expect_vec4(const char *label, const float actual[4],
                        float x, float y, float z, float w)
{
    const float expected[4] = { x, y, z, w };
    char component_label[96];

    for (int i = 0; i < 4; i++) {
        snprintf(component_label, sizeof(component_label), "%s[%d]", label, i);
        expect_float(component_label, actual[i], expected[i]);
    }
}

static void expect_identity(const char *label, const float matrix[16])
{
    char element_label[96];

    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            float expected = row == column ? 1.0f : 0.0f;
            int index = column * 4 + row;

            snprintf(element_label, sizeof(element_label), "%s[%d]",
                     label, index);
            expect_float(element_label, matrix[index], expected);
        }
    }
}

static void test_defaults(struct l10gl_ctx *ctx)
{
    float matrix[16];

    expect_int("default matrix mode", ctx->matrix_mode_val,
               L10GL_MATRIX_MODELVIEW);
    expect_int("default modelview top", ctx->modelview_top, 0);
    expect_int("default projection top", ctx->projection_top, 0);
    expect_int("default viewport x", ctx->viewport_x, 0);
    expect_int("default viewport y", ctx->viewport_y, 0);
    expect_int("default viewport width", ctx->viewport_width, ctx->width);
    expect_int("default viewport height", ctx->viewport_height, ctx->height);
    expect_float("default depth near", ctx->depth_range_near, 0.0f);
    expect_float("default depth far", ctx->depth_range_far, 1.0f);
    expect_int("get default modelview",
               l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, matrix), 0);
    expect_identity("default modelview", matrix);
    expect_int("get default projection",
               l10gl_get_matrix(ctx, L10GL_MATRIX_PROJECTION, matrix), 0);
    expect_identity("default projection", matrix);
}

static void test_postmultiply_and_rotation(struct l10gl_ctx *ctx)
{
    float point[4] = { 1, 1, 1, 1 };
    float result[4];
    float matrix[16];

    expect_int("select modelview",
               l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW), 0);
    l10gl_load_identity(ctx);
    l10gl_translatef(ctx, 1, 2, 3);
    l10gl_scalef(ctx, 2, 3, 4);
    expect_int("get translated/scaled modelview",
               l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, matrix), 0);
    l10gl_transform_vec4(matrix, point, result);
    expect_vec4("translate * scale", result, 3, 5, 7, 1);

    /* Exercise documented input/output aliasing. */
    memcpy(result, point, sizeof(result));
    l10gl_transform_vec4(matrix, result, result);
    expect_vec4("aliased transform", result, 3, 5, 7, 1);

    l10gl_load_identity(ctx);
    expect_int("rotate normalized Z axis", l10gl_rotatef(ctx, 90, 0, 0, 4), 0);
    expect_int("get rotation",
               l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, matrix), 0);
    point[0] = 1; point[1] = 0; point[2] = 0; point[3] = 1;
    l10gl_transform_vec4(matrix, point, result);
    expect_vec4("Z rotation", result, 0, 1, 0, 1);
    expect_int("reject zero rotation axis", l10gl_rotatef(ctx, 30, 0, 0, 0),
               -EINVAL);
}

static void test_stacks(struct l10gl_ctx *ctx)
{
    float matrix[16];

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_translatef(ctx, 5, 0, 0);
    expect_int("modelview push", l10gl_push_matrix(ctx), 0);
    l10gl_scalef(ctx, 2, 2, 2);
    expect_int("modelview pop", l10gl_pop_matrix(ctx), 0);
    l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, matrix);
    expect_float("pop restores translation", matrix[12], 5);
    expect_float("pop restores scale", matrix[0], 1);

    for (int i = 1; i < L10GL_MODELVIEW_STACK_DEPTH; i++)
        expect_int("modelview bounded push", l10gl_push_matrix(ctx), 0);
    expect_int("modelview overflow", l10gl_push_matrix(ctx), -EOVERFLOW);
    expect_int("modelview top at capacity", ctx->modelview_top,
               L10GL_MODELVIEW_STACK_DEPTH - 1);
    for (int i = 1; i < L10GL_MODELVIEW_STACK_DEPTH; i++)
        expect_int("modelview bounded pop", l10gl_pop_matrix(ctx), 0);
    expect_int("modelview underflow", l10gl_pop_matrix(ctx), -ERANGE);

    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    for (int i = 1; i < L10GL_PROJECTION_STACK_DEPTH; i++)
        expect_int("projection bounded push", l10gl_push_matrix(ctx), 0);
    expect_int("projection overflow", l10gl_push_matrix(ctx), -EOVERFLOW);
    for (int i = 1; i < L10GL_PROJECTION_STACK_DEPTH; i++)
        expect_int("projection bounded pop", l10gl_pop_matrix(ctx), 0);
    expect_int("projection underflow", l10gl_pop_matrix(ctx), -ERANGE);

    expect_int("reject invalid matrix mode",
               l10gl_matrix_mode(ctx, (enum l10gl_matrix_mode)99), -EINVAL);
    expect_int("invalid mode leaves projection selected", ctx->matrix_mode_val,
               L10GL_MATRIX_PROJECTION);
    expect_int("reject invalid matrix query",
               l10gl_get_matrix(ctx, (enum l10gl_matrix_mode)99, matrix),
               -EINVAL);
}

static void test_projection_and_viewport(struct l10gl_ctx *ctx)
{
    const float object[4] = { 0, 0, 0, 1 };
    float clip[4];
    float ndc[3];
    float window[3];
    int old_width = ctx->viewport_width;

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_identity(ctx);
    l10gl_translatef(ctx, 0, 0, -5);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    expect_int("90-degree perspective",
               l10gl_perspective(ctx, 90, 1, 1, 10), 0);
    l10gl_object_to_clip(ctx, object, clip);
    expect_vec4("perspective clip", clip, 0, 0, 35.0f / 9.0f, 5);

    ndc[0] = clip[0] / clip[3];
    ndc[1] = clip[1] / clip[3];
    ndc[2] = clip[2] / clip[3];
    expect_int("custom viewport", l10gl_viewport(ctx, 10, 20, 200, 100), 0);
    l10gl_depth_range(ctx, .2f, .8f);
    l10gl_ndc_to_window(ctx, ndc, window);
    expect_float("viewport center x", window[0], 110);
    /* GL viewport Y=20 is lower-left; backend Y is measured from the top. */
    expect_float("viewport center y", window[1], ctx->height - 70);
    expect_float("viewport depth", window[2], 11.0f / 15.0f);

    expect_int("reject negative viewport",
               l10gl_viewport(ctx, 0, 0, -1, 10), -EINVAL);
    expect_int("invalid viewport unchanged", ctx->viewport_width, 200);
    l10gl_depth_range(ctx, -10, 10);
    expect_float("clamped depth near", ctx->depth_range_near, 0);
    expect_float("clamped depth far", ctx->depth_range_far, 1);

    l10gl_load_identity(ctx);
    expect_int("reject zero perspective aspect",
               l10gl_perspective(ctx, 60, 0, 1, 10), -EINVAL);
    expect_int("reject zero perspective FOV",
               l10gl_perspective(ctx, 0, 1, 1, 10), -EINVAL);
    {
        float matrix[16];
        l10gl_get_matrix(ctx, L10GL_MATRIX_PROJECTION, matrix);
        expect_identity("invalid perspective unchanged", matrix);
    }

    /* Restore a conventional default so later tests do not depend on this
     * function's custom viewport. */
    expect_int("restore viewport",
               l10gl_viewport(ctx, 0, 0, old_width, ctx->height), 0);
}

static void test_ortho_and_load_multiply(struct l10gl_ctx *ctx)
{
    static const float double_x[16] = {
        2, 0, 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1,
    };
    float object[4] = { 2, 3, -1, 1 };
    float result[4];
    float matrix[16];

    l10gl_matrix_mode(ctx, L10GL_MATRIX_MODELVIEW);
    l10gl_load_matrixf(ctx, double_x);
    l10gl_mult_matrixf(ctx, double_x);
    l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, matrix);
    l10gl_transform_vec4(matrix, object, result);
    expect_vec4("load and multiply", result, 8, 3, -1, 1);

    l10gl_load_identity(ctx);
    l10gl_matrix_mode(ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(ctx);
    expect_int("orthographic projection",
               l10gl_ortho(ctx, -2, 2, -1, 3, 1, 11), 0);
    l10gl_object_to_clip(ctx, object, result);
    expect_vec4("ortho corner", result, 1, 1, -1, 1);
    expect_int("reject degenerate ortho",
               l10gl_ortho(ctx, 1, 1, -1, 1, 1, 10), -EINVAL);
}

int main(void)
{
    struct l10gl_ctx ctx;

    if (l10gl_create(&ctx, &swrast_backend, 640, 480, 2) < 0) {
        fprintf(stderr, "test-xform: failed to create reference context\n");
        return 1;
    }

    test_defaults(&ctx);
    test_postmultiply_and_rotation(&ctx);
    test_stacks(&ctx);
    test_projection_and_viewport(&ctx);
    test_ortho_and_load_multiply(&ctx);
    l10gl_destroy(&ctx);

    if (failures) {
        fprintf(stderr, "test-xform: FAILED (%d checks)\n", failures);
        return 1;
    }
    printf("test-xform: PASS (matrix order, stacks, transforms, projections, "
           "viewport, depth range)\n");
    return 0;
}
