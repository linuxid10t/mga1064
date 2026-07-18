/*
 * l10gl_xform.c - OpenGL-convention matrix stacks and viewport transform.
 *
 * Matrices are column-major and operate on column vectors. Transform
 * constructors post-multiply the current matrix, matching legacy OpenGL:
 * current = current * transform.
 */

#include <errno.h>
#include <math.h>
#include <string.h>

#include "l10gl.h"
#include "l10gl_xform.h"

#define L10GL_PI 3.14159265358979323846f

static const float identity_matrix[16] = {
    1, 0, 0, 0,
    0, 1, 0, 0,
    0, 0, 1, 0,
    0, 0, 0, 1,
};

static float *current_matrix(struct l10gl_ctx *ctx)
{
    if (ctx->matrix_mode_val == L10GL_MATRIX_PROJECTION)
        return ctx->projection_stack[ctx->projection_top];
    return ctx->modelview_stack[ctx->modelview_top];
}

static const float *matrix_for_mode(const struct l10gl_ctx *ctx,
                                    enum l10gl_matrix_mode mode)
{
    if (mode == L10GL_MATRIX_MODELVIEW)
        return ctx->modelview_stack[ctx->modelview_top];
    if (mode == L10GL_MATRIX_PROJECTION)
        return ctx->projection_stack[ctx->projection_top];
    return NULL;
}

static void multiply(float output[16], const float left[16],
                     const float right[16])
{
    float result[16];

    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            float value = 0.0f;

            for (int k = 0; k < 4; k++)
                value += left[k * 4 + row] * right[column * 4 + k];
            result[column * 4 + row] = value;
        }
    }
    memcpy(output, result, sizeof(result));
}

static float clamp_depth(float value)
{
    if (!(value > 0.0f))
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

void l10gl_xform_init(struct l10gl_ctx *ctx)
{
    ctx->modelview_top = 0;
    ctx->projection_top = 0;
    ctx->matrix_mode_val = L10GL_MATRIX_MODELVIEW;
    memcpy(ctx->modelview_stack[0], identity_matrix,
           sizeof(identity_matrix));
    memcpy(ctx->projection_stack[0], identity_matrix,
           sizeof(identity_matrix));
    ctx->viewport_x = 0;
    ctx->viewport_y = 0;
    ctx->viewport_width = ctx->width;
    ctx->viewport_height = ctx->height;
    ctx->depth_range_near = 0.0f;
    ctx->depth_range_far = 1.0f;
}

int l10gl_matrix_mode(struct l10gl_ctx *ctx, enum l10gl_matrix_mode mode)
{
    if (!ctx || (mode != L10GL_MATRIX_MODELVIEW &&
                 mode != L10GL_MATRIX_PROJECTION))
        return -EINVAL;
    ctx->matrix_mode_val = mode;
    return 0;
}

void l10gl_load_identity(struct l10gl_ctx *ctx)
{
    memcpy(current_matrix(ctx), identity_matrix, sizeof(identity_matrix));
}

void l10gl_load_matrixf(struct l10gl_ctx *ctx, const float matrix[16])
{
    memmove(current_matrix(ctx), matrix, sizeof(float) * 16u);
}

void l10gl_mult_matrixf(struct l10gl_ctx *ctx, const float matrix[16])
{
    float *current = current_matrix(ctx);

    multiply(current, current, matrix);
}

int l10gl_get_matrix(const struct l10gl_ctx *ctx,
                     enum l10gl_matrix_mode mode, float matrix[16])
{
    const float *source;

    if (!ctx || !matrix)
        return -EINVAL;
    source = matrix_for_mode(ctx, mode);
    if (!source)
        return -EINVAL;
    memcpy(matrix, source, sizeof(float) * 16u);
    return 0;
}

int l10gl_push_matrix(struct l10gl_ctx *ctx)
{
    if (ctx->matrix_mode_val == L10GL_MATRIX_MODELVIEW) {
        if (ctx->modelview_top + 1 >= L10GL_MODELVIEW_STACK_DEPTH)
            return -EOVERFLOW;
        memcpy(ctx->modelview_stack[ctx->modelview_top + 1],
               ctx->modelview_stack[ctx->modelview_top], sizeof(float) * 16u);
        ctx->modelview_top++;
    } else {
        if (ctx->projection_top + 1 >= L10GL_PROJECTION_STACK_DEPTH)
            return -EOVERFLOW;
        memcpy(ctx->projection_stack[ctx->projection_top + 1],
               ctx->projection_stack[ctx->projection_top], sizeof(float) * 16u);
        ctx->projection_top++;
    }
    return 0;
}

int l10gl_pop_matrix(struct l10gl_ctx *ctx)
{
    if (ctx->matrix_mode_val == L10GL_MATRIX_MODELVIEW) {
        if (ctx->modelview_top == 0)
            return -ERANGE;
        ctx->modelview_top--;
    } else {
        if (ctx->projection_top == 0)
            return -ERANGE;
        ctx->projection_top--;
    }
    return 0;
}

void l10gl_translatef(struct l10gl_ctx *ctx, float x, float y, float z)
{
    float matrix[16];

    memcpy(matrix, identity_matrix, sizeof(matrix));
    matrix[12] = x;
    matrix[13] = y;
    matrix[14] = z;
    l10gl_mult_matrixf(ctx, matrix);
}

int l10gl_rotatef(struct l10gl_ctx *ctx, float angle_degrees,
                  float x, float y, float z)
{
    float length = sqrtf(x * x + y * y + z * z);
    float radians, sine, cosine, one_minus_cosine;
    float matrix[16];

    if (!(length > 0.0f) || !isfinite(length) || !isfinite(angle_degrees))
        return -EINVAL;
    x /= length;
    y /= length;
    z /= length;
    radians = angle_degrees * (L10GL_PI / 180.0f);
    sine = sinf(radians);
    cosine = cosf(radians);
    one_minus_cosine = 1.0f - cosine;

    memcpy(matrix, identity_matrix, sizeof(matrix));
    matrix[0] = x * x * one_minus_cosine + cosine;
    matrix[4] = x * y * one_minus_cosine - z * sine;
    matrix[8] = x * z * one_minus_cosine + y * sine;
    matrix[1] = y * x * one_minus_cosine + z * sine;
    matrix[5] = y * y * one_minus_cosine + cosine;
    matrix[9] = y * z * one_minus_cosine - x * sine;
    matrix[2] = z * x * one_minus_cosine - y * sine;
    matrix[6] = z * y * one_minus_cosine + x * sine;
    matrix[10] = z * z * one_minus_cosine + cosine;
    l10gl_mult_matrixf(ctx, matrix);
    return 0;
}

void l10gl_scalef(struct l10gl_ctx *ctx, float x, float y, float z)
{
    float matrix[16] = {0};

    matrix[0] = x;
    matrix[5] = y;
    matrix[10] = z;
    matrix[15] = 1.0f;
    l10gl_mult_matrixf(ctx, matrix);
}

int l10gl_frustum(struct l10gl_ctx *ctx,
                  float left, float right, float bottom, float top,
                  float z_near, float z_far)
{
    float width = right - left;
    float height = top - bottom;
    float depth = z_far - z_near;
    float matrix[16] = {0};

    if (!isfinite(width) || !isfinite(height) || !isfinite(depth) ||
        !isfinite(z_near) || !isfinite(z_far) || width == 0.0f ||
        height == 0.0f || depth == 0.0f || z_near <= 0.0f ||
        z_far <= 0.0f)
        return -EINVAL;

    matrix[0] = 2.0f * z_near / width;
    matrix[5] = 2.0f * z_near / height;
    matrix[8] = (right + left) / width;
    matrix[9] = (top + bottom) / height;
    matrix[10] = -(z_far + z_near) / depth;
    matrix[11] = -1.0f;
    matrix[14] = -(2.0f * z_far * z_near) / depth;
    l10gl_mult_matrixf(ctx, matrix);
    return 0;
}

int l10gl_perspective(struct l10gl_ctx *ctx, float fovy_degrees,
                      float aspect, float z_near, float z_far)
{
    float half_height;

    if (!isfinite(fovy_degrees) || !isfinite(aspect) ||
        fovy_degrees <= 0.0f || fovy_degrees >= 180.0f || aspect == 0.0f)
        return -EINVAL;
    half_height = z_near * tanf(fovy_degrees * (L10GL_PI / 360.0f));
    if (!isfinite(half_height))
        return -EINVAL;
    return l10gl_frustum(ctx, -half_height * aspect, half_height * aspect,
                         -half_height, half_height, z_near, z_far);
}

int l10gl_ortho(struct l10gl_ctx *ctx,
                float left, float right, float bottom, float top,
                float z_near, float z_far)
{
    float width = right - left;
    float height = top - bottom;
    float depth = z_far - z_near;
    float matrix[16];

    if (!isfinite(width) || !isfinite(height) || !isfinite(depth) ||
        width == 0.0f || height == 0.0f || depth == 0.0f)
        return -EINVAL;

    memcpy(matrix, identity_matrix, sizeof(matrix));
    matrix[0] = 2.0f / width;
    matrix[5] = 2.0f / height;
    matrix[10] = -2.0f / depth;
    matrix[12] = -(right + left) / width;
    matrix[13] = -(top + bottom) / height;
    matrix[14] = -(z_far + z_near) / depth;
    l10gl_mult_matrixf(ctx, matrix);
    return 0;
}

int l10gl_viewport(struct l10gl_ctx *ctx, int x, int y,
                   int width, int height)
{
    if (!ctx || width < 0 || height < 0)
        return -EINVAL;
    ctx->viewport_x = x;
    ctx->viewport_y = y;
    ctx->viewport_width = width;
    ctx->viewport_height = height;
    return 0;
}

void l10gl_depth_range(struct l10gl_ctx *ctx, float z_near, float z_far)
{
    ctx->depth_range_near = clamp_depth(z_near);
    ctx->depth_range_far = clamp_depth(z_far);
}

void l10gl_transform_vec4(const float matrix[16],
                          const float input[4], float output[4])
{
    float result[4];

    for (int row = 0; row < 4; row++) {
        result[row] = matrix[0 * 4 + row] * input[0]
                    + matrix[1 * 4 + row] * input[1]
                    + matrix[2 * 4 + row] * input[2]
                    + matrix[3 * 4 + row] * input[3];
    }
    memcpy(output, result, sizeof(result));
}

void l10gl_object_to_clip(const struct l10gl_ctx *ctx,
                          const float object[4], float clip[4])
{
    float eye[4];

    l10gl_transform_vec4(ctx->modelview_stack[ctx->modelview_top],
                         object, eye);
    l10gl_transform_vec4(ctx->projection_stack[ctx->projection_top],
                         eye, clip);
}

void l10gl_ndc_to_window(const struct l10gl_ctx *ctx,
                         const float ndc[3], float window[3])
{
    float gl_window_y;

    window[0] = ctx->viewport_x
              + (ndc[0] + 1.0f) * 0.5f * ctx->viewport_width;
    gl_window_y = ctx->viewport_y
                + (ndc[1] + 1.0f) * 0.5f * ctx->viewport_height;
    window[1] = ctx->height - gl_window_y;
    window[2] = ctx->depth_range_near
              + (ndc[2] + 1.0f) * 0.5f
              * (ctx->depth_range_far - ctx->depth_range_near);
}
