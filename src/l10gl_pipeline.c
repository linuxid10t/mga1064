/*
 * l10gl_pipeline.c - Immediate-mode geometry and lighting pipeline.
 *
 * This layer accepts model-space vertices, applies the X1 MODELVIEW,
 * PROJECTION, viewport, and depth-range state, then emits the existing
 * screen-space backend primitives.  It streams at most three captured
 * vertices and performs no allocation.
 */

#include <errno.h>
#include <math.h>

#include "l10gl.h"
#include "l10gl_pipeline.h"

struct projected_vertex {
    struct l10gl_vertex screen;
    float ndc_x;
    float ndc_y;
};

struct clip_vertex {
    float clip[4];
    float r, g, b, a;
    float nx, ny, nz;
    float u, v;
};

#define L10GL_MAX_TRIANGLE_SCANLINES 2047.0f

void l10gl_pipeline_init(struct l10gl_ctx *ctx)
{
    ctx->current_r = 1.0f;
    ctx->current_g = 1.0f;
    ctx->current_b = 1.0f;
    ctx->current_a = 1.0f;
    ctx->current_nx = 0.0f;
    ctx->current_ny = 0.0f;
    ctx->current_nz = 1.0f;
    ctx->current_u = 0.0f;
    ctx->current_v = 0.0f;
    ctx->cull_mode_val = L10GL_CULL_NONE;

    ctx->lighting_enabled = 0;
    ctx->light_dir_x = 0.0f;
    ctx->light_dir_y = 0.0f;
    ctx->light_dir_z = -1.0f;
    ctx->light_r = 0.8f;
    ctx->light_g = 0.8f;
    ctx->light_b = 0.8f;
    ctx->ambient_r = 0.2f;
    ctx->ambient_g = 0.2f;
    ctx->ambient_b = 0.2f;
    ctx->material_r = 1.0f;
    ctx->material_g = 1.0f;
    ctx->material_b = 1.0f;
    ctx->material_a = 1.0f;

    ctx->immediate_active = 0;
    ctx->immediate_vertex_count = 0;
}

static int primitive_supported(enum l10gl_primitive primitive)
{
    return primitive == L10GL_TRIANGLES ||
           primitive == L10GL_TRIANGLE_STRIP ||
           primitive == L10GL_TRIANGLE_FAN ||
           primitive == L10GL_LINES ||
           primitive == L10GL_LINE_STRIP;
}

static int capture_clip_vertex(const struct l10gl_ctx *ctx,
                               const struct l10gl_immediate_vertex *input,
                               struct clip_vertex *output)
{
    const float object[4] = { input->x, input->y, input->z, 1.0f };

    l10gl_object_to_clip(ctx, object, output->clip);
    if (!isfinite(output->clip[0]) || !isfinite(output->clip[1]) ||
        !isfinite(output->clip[2]) || !isfinite(output->clip[3]))
        return 0;
    output->r = input->r;
    output->g = input->g;
    output->b = input->b;
    output->a = input->a;
    output->nx = input->nx;
    output->ny = input->ny;
    output->nz = input->nz;
    output->u = input->u;
    output->v = input->v;
    return 1;
}

static void interpolate_clip_vertex(struct clip_vertex *output,
                                    const struct clip_vertex *a,
                                    const struct clip_vertex *b, float t)
{
#define INTERPOLATE(member) output->member = a->member + (b->member - a->member) * t
    for (int i = 0; i < 4; i++)
        output->clip[i] = a->clip[i] + (b->clip[i] - a->clip[i]) * t;
    INTERPOLATE(r);
    INTERPOLATE(g);
    INTERPOLATE(b);
    INTERPOLATE(a);
    INTERPOLATE(nx);
    INTERPOLATE(ny);
    INTERPOLATE(nz);
    INTERPOLATE(u);
    INTERPOLATE(v);
#undef INTERPOLATE
}

static double near_distance(const struct clip_vertex *vertex)
{
    return (double)vertex->clip[2] + vertex->clip[3];
}

static void snap_near_plane(struct clip_vertex *vertex)
{
    double distance = near_distance(vertex);
    double scale = fmax(1.0, fabs((double)vertex->clip[3]));

    if (fabs(distance) <= scale * 1.0e-6)
        vertex->clip[2] = -vertex->clip[3];
}

/* Sutherland-Hodgman clipping against the OpenGL near plane Z + W >= 0.
 * A triangle produces zero, three, or four vertices. */
static int clip_triangle_near(const struct clip_vertex input[3],
                              struct clip_vertex output[4])
{
    const struct clip_vertex *previous = &input[2];
    double previous_distance = near_distance(previous);
    int previous_inside = previous_distance >= 0.0;
    int count = 0;

    for (int i = 0; i < 3; i++) {
        const struct clip_vertex *current = &input[i];
        double current_distance = near_distance(current);
        int current_inside = current_distance >= 0.0;

        if (previous_inside != current_inside) {
            double denominator = previous_distance - current_distance;
            float t = (float)(previous_distance / denominator);

            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            interpolate_clip_vertex(&output[count], previous, current, t);
            /* Make the generated vertex exactly satisfy the plane despite
             * floating-point interpolation error, so near depth maps to the
             * configured depth-range endpoint rather than slightly outside. */
            output[count].clip[2] = -output[count].clip[3];
            count++;
        }
        if (current_inside)
            output[count++] = *current;

        previous = current;
        previous_distance = current_distance;
        previous_inside = current_inside;
    }
    return count;
}

static int project_clip_vertex(const struct l10gl_ctx *ctx,
                               const struct clip_vertex *input,
                               struct projected_vertex *output)
{
    float ndc[3];
    float window[3];
    float z_slop;

    if (!(input->clip[3] > 1.0e-20f))
        return 0;

    /* Near crossings have already been clipped. Far-plane clipping is not in
     * X3's one-plane scope, so retain conservative whole-primitive rejection
     * rather than sending out-of-range Z to vintage hardware. */
    z_slop = input->clip[3] * 1.0e-6f;
    if (input->clip[2] < -input->clip[3] - z_slop ||
        input->clip[2] > input->clip[3] + z_slop)
        return 0;

    ndc[0] = input->clip[0] / input->clip[3];
    ndc[1] = input->clip[1] / input->clip[3];
    ndc[2] = input->clip[2] / input->clip[3];
    if (!isfinite(ndc[0]) || !isfinite(ndc[1]) || !isfinite(ndc[2]))
        return 0;
    l10gl_ndc_to_window(ctx, ndc, window);
    if (!isfinite(window[0]) || !isfinite(window[1]) || !isfinite(window[2]))
        return 0;

    output->screen.x = window[0];
    output->screen.y = window[1];
    output->screen.z = window[2];
    /* X5 will replace this affine value with reciprocal eye-space depth. */
    output->screen.w = 1.0f;
    output->screen.r = input->r;
    output->screen.g = input->g;
    output->screen.b = input->b;
    output->screen.a = input->a;
    output->screen.u = input->u;
    output->screen.v = input->v;
    output->ndc_x = ndc[0];
    output->ndc_y = ndc[1];
    return 1;
}

static int triangle_exceeds_scan_limit(const struct projected_vertex *v0,
                                       const struct projected_vertex *v1,
                                       const struct projected_vertex *v2)
{
    float min_y = fminf(v0->screen.y, fminf(v1->screen.y, v2->screen.y));
    float max_y = fmaxf(v0->screen.y, fmaxf(v1->screen.y, v2->screen.y));
    float scanlines = ceilf(max_y) - floorf(min_y);

    /* ViRGE triangle scan counts are 11-bit fields (virge.h:296). Rejecting
     * an oversized projected primitive is conservative and backend-neutral;
     * X/Y clipping itself remains in the hardware clip rectangle. */
    return !isfinite(scanlines) ||
           scanlines > L10GL_MAX_TRIANGLE_SCANLINES;
}

static int triangle_is_culled(const struct l10gl_ctx *ctx,
                              const struct projected_vertex *v0,
                              const struct projected_vertex *v1,
                              const struct projected_vertex *v2)
{
    double area = ((double)v1->ndc_x - v0->ndc_x)
                * ((double)v2->ndc_y - v0->ndc_y)
                - ((double)v1->ndc_y - v0->ndc_y)
                * ((double)v2->ndc_x - v0->ndc_x);

    if (area == 0.0f)
        return 1;
    if (ctx->cull_mode_val == L10GL_CULL_BACK)
        return area < 0.0f;
    if (ctx->cull_mode_val == L10GL_CULL_FRONT)
        return area > 0.0f;
    return 0;
}

static void emit_triangle(struct l10gl_ctx *ctx,
                          const struct l10gl_immediate_vertex *a,
                          const struct l10gl_immediate_vertex *b,
                          const struct l10gl_immediate_vertex *c)
{
    struct clip_vertex input[3];
    struct clip_vertex clipped[4];
    struct projected_vertex projected[4];
    int count;

    if (!capture_clip_vertex(ctx, a, &input[0]) ||
        !capture_clip_vertex(ctx, b, &input[1]) ||
        !capture_clip_vertex(ctx, c, &input[2]))
        return;
    for (int i = 0; i < 3; i++)
        snap_near_plane(&input[i]);
    count = clip_triangle_near(input, clipped);
    if (count < 3)
        return;
    for (int i = 0; i < count; i++)
        if (!project_clip_vertex(ctx, &clipped[i], &projected[i]))
            return;

    /* A clipped quad is triangulated as a fan, preserving polygon winding and
     * shared-edge attribute identity. */
    for (int i = 1; i + 1 < count; i++) {
        if (triangle_is_culled(ctx, &projected[0], &projected[i],
                              &projected[i + 1]) ||
            triangle_exceeds_scan_limit(&projected[0], &projected[i],
                                        &projected[i + 1]))
            continue;

        /* Binding NULL selects untextured emission. Backends without a
         * textured hook still receive the established Gouraud fallback. */
        if (ctx->current_texture) {
            l10gl_draw_textured_triangle(ctx, projected[0].screen,
                                         projected[i].screen,
                                         projected[i + 1].screen);
        } else {
            l10gl_draw_triangle(ctx, projected[0].screen,
                                projected[i].screen,
                                projected[i + 1].screen);
        }
    }
}

static void emit_line(struct l10gl_ctx *ctx,
                      const struct l10gl_immediate_vertex *a,
                      const struct l10gl_immediate_vertex *b)
{
    struct clip_vertex clipped[2];
    struct projected_vertex projected[2];

    /* X3 clips triangles. Lines retain X2's conservative whole-segment depth
     * rejection until a dedicated line-clipping pass is added. */
    if (!capture_clip_vertex(ctx, a, &clipped[0]) ||
        !capture_clip_vertex(ctx, b, &clipped[1]) ||
        !project_clip_vertex(ctx, &clipped[0], &projected[0]) ||
        !project_clip_vertex(ctx, &clipped[1], &projected[1]))
        return;
    l10gl_draw_line(ctx, projected[0].screen, projected[1].screen);
}

static void assemble_vertex(struct l10gl_ctx *ctx,
                            struct l10gl_immediate_vertex vertex)
{
    struct l10gl_immediate_vertex *slots = ctx->immediate_vertices;
    unsigned long count = ctx->immediate_vertex_count;

    switch (ctx->immediate_primitive) {
    case L10GL_TRIANGLES:
        slots[count % 3] = vertex;
        count++;
        if (count % 3 == 0)
            emit_triangle(ctx, &slots[0], &slots[1], &slots[2]);
        break;

    case L10GL_TRIANGLE_STRIP:
        if (count < 2) {
            slots[count] = vertex;
        } else {
            /* Alternating source order preserves a consistent CCW/CW facing
             * convention for every triangle in the strip. */
            if ((count - 2) & 1)
                emit_triangle(ctx, &slots[1], &slots[0], &vertex);
            else
                emit_triangle(ctx, &slots[0], &slots[1], &vertex);
            slots[0] = slots[1];
            slots[1] = vertex;
        }
        count++;
        break;

    case L10GL_TRIANGLE_FAN:
        if (count == 0)
            slots[0] = vertex;
        else if (count == 1)
            slots[1] = vertex;
        else {
            emit_triangle(ctx, &slots[0], &slots[1], &vertex);
            slots[1] = vertex;
        }
        count++;
        break;

    case L10GL_LINES:
        if ((count & 1) == 0)
            slots[0] = vertex;
        else
            emit_line(ctx, &slots[0], &vertex);
        count++;
        break;

    case L10GL_LINE_STRIP:
        if (count != 0)
            emit_line(ctx, &slots[0], &vertex);
        slots[0] = vertex;
        count++;
        break;

    case L10GL_POINTS:
        /* Rejected by l10gl_begin; retained for exhaustive enum handling. */
        break;
    }
    ctx->immediate_vertex_count = count;
}

int l10gl_begin(struct l10gl_ctx *ctx, enum l10gl_primitive primitive)
{
    if (!ctx)
        return -EINVAL;
    if (ctx->immediate_active)
        return -EBUSY;
    if (!primitive_supported(primitive))
        return -ENOTSUP;

    ctx->immediate_active = 1;
    ctx->immediate_primitive = primitive;
    ctx->immediate_vertex_count = 0;
    return 0;
}

int l10gl_end(struct l10gl_ctx *ctx)
{
    if (!ctx)
        return -EINVAL;
    if (!ctx->immediate_active)
        return -EPERM;
    ctx->immediate_active = 0;
    ctx->immediate_vertex_count = 0;
    return 0;
}

static float clamp_lit_channel(float value)
{
    if (!(value > 0.0f))
        return 0.0f;
    if (value > 1.0f)
        return 1.0f;
    return value;
}

/* Transform an object-space normal into eye space with inverse-transpose
 * MODELVIEW, then normalize it. This is deliberately correct for non-uniform
 * scale; a singular matrix or unusable normal produces ambient light only. */
static int transform_eye_normal(const struct l10gl_ctx *ctx,
                                float x, float y, float z,
                                float output[3])
{
    const float *m = ctx->modelview_stack[ctx->modelview_top];
    double a00 = m[0], a01 = m[4], a02 = m[8];
    double a10 = m[1], a11 = m[5], a12 = m[9];
    double a20 = m[2], a21 = m[6], a22 = m[10];
    double c00 = a11 * a22 - a12 * a21;
    double c01 = a12 * a20 - a10 * a22;
    double c02 = a10 * a21 - a11 * a20;
    double c10 = a02 * a21 - a01 * a22;
    double c11 = a00 * a22 - a02 * a20;
    double c12 = a01 * a20 - a00 * a21;
    double c20 = a01 * a12 - a02 * a11;
    double c21 = a02 * a10 - a00 * a12;
    double c22 = a00 * a11 - a01 * a10;
    double determinant = a00 * c00 + a01 * c01 + a02 * c02;
    double nx, ny, nz, length;

    if (!isfinite(determinant) || determinant == 0.0)
        return 0;
    /* Normalization cancels |determinant|. Keep only its sign to avoid
     * overflowing the inverse for very small but still invertible scales. */
    nx = c00 * x + c01 * y + c02 * z;
    ny = c10 * x + c11 * y + c12 * z;
    nz = c20 * x + c21 * y + c22 * z;
    if (determinant < 0.0) {
        nx = -nx;
        ny = -ny;
        nz = -nz;
    }
    length = sqrt(nx * nx + ny * ny + nz * nz);
    if (!(length > 1.0e-20) || !isfinite(length))
        return 0;
    output[0] = (float)(nx / length);
    output[1] = (float)(ny / length);
    output[2] = (float)(nz / length);
    return 1;
}

static void capture_vertex_color(const struct l10gl_ctx *ctx,
                                 struct l10gl_immediate_vertex *vertex)
{
    float normal[3];
    float diffuse = 0.0f;

    if (!ctx->lighting_enabled) {
        vertex->r = ctx->current_r;
        vertex->g = ctx->current_g;
        vertex->b = ctx->current_b;
        vertex->a = ctx->current_a;
        return;
    }

    if (transform_eye_normal(ctx, ctx->current_nx, ctx->current_ny,
                             ctx->current_nz, normal)) {
        diffuse = -(normal[0] * ctx->light_dir_x +
                    normal[1] * ctx->light_dir_y +
                    normal[2] * ctx->light_dir_z);
        if (!(diffuse > 0.0f))
            diffuse = 0.0f;
        else if (diffuse > 1.0f)
            diffuse = 1.0f;
    }

    vertex->r = clamp_lit_channel(ctx->material_r *
                                  (ctx->ambient_r + ctx->light_r * diffuse));
    vertex->g = clamp_lit_channel(ctx->material_g *
                                  (ctx->ambient_g + ctx->light_g * diffuse));
    vertex->b = clamp_lit_channel(ctx->material_b *
                                  (ctx->ambient_b + ctx->light_b * diffuse));
    vertex->a = clamp_lit_channel(ctx->material_a);
}

int l10gl_vertex3f(struct l10gl_ctx *ctx, float x, float y, float z)
{
    struct l10gl_immediate_vertex vertex;

    if (!ctx)
        return -EINVAL;
    if (!ctx->immediate_active)
        return -EPERM;

    vertex.x = x;
    vertex.y = y;
    vertex.z = z;
    capture_vertex_color(ctx, &vertex);
    vertex.nx = ctx->current_nx;
    vertex.ny = ctx->current_ny;
    vertex.nz = ctx->current_nz;
    vertex.u = ctx->current_u;
    vertex.v = ctx->current_v;
    assemble_vertex(ctx, vertex);
    return 0;
}

void l10gl_color4f(struct l10gl_ctx *ctx,
                   float r, float g, float b, float a)
{
    ctx->current_r = r;
    ctx->current_g = g;
    ctx->current_b = b;
    ctx->current_a = a;
}

void l10gl_normal3f(struct l10gl_ctx *ctx, float x, float y, float z)
{
    ctx->current_nx = x;
    ctx->current_ny = y;
    ctx->current_nz = z;
}

void l10gl_texcoord2f(struct l10gl_ctx *ctx, float u, float v)
{
    ctx->current_u = u;
    ctx->current_v = v;
}

int l10gl_cull_face(struct l10gl_ctx *ctx, enum l10gl_cull_mode mode)
{
    if (!ctx || (mode != L10GL_CULL_NONE && mode != L10GL_CULL_FRONT &&
                 mode != L10GL_CULL_BACK))
        return -EINVAL;
    ctx->cull_mode_val = mode;
    return 0;
}

void l10gl_enable_lighting(struct l10gl_ctx *ctx, int enable)
{
    ctx->lighting_enabled = !!enable;
}

int l10gl_light_dir(struct l10gl_ctx *ctx, float x, float y, float z)
{
    float length;

    if (!ctx)
        return -EINVAL;
    length = sqrtf(x * x + y * y + z * z);
    if (!(length > 0.0f) || !isfinite(length))
        return -EINVAL;
    ctx->light_dir_x = x / length;
    ctx->light_dir_y = y / length;
    ctx->light_dir_z = z / length;
    return 0;
}

void l10gl_light_color(struct l10gl_ctx *ctx, float r, float g, float b)
{
    ctx->light_r = r;
    ctx->light_g = g;
    ctx->light_b = b;
}

void l10gl_light_ambient(struct l10gl_ctx *ctx, float r, float g, float b)
{
    ctx->ambient_r = r;
    ctx->ambient_g = g;
    ctx->ambient_b = b;
}

void l10gl_material(struct l10gl_ctx *ctx,
                    float r, float g, float b, float a)
{
    ctx->material_r = r;
    ctx->material_g = g;
    ctx->material_b = b;
    ctx->material_a = a;
}
