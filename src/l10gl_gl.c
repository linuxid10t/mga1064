/* Thin, single-current-context OpenGL 1.1 compatibility layer. */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <GL/gl.h>

#include "l10gl.h"

struct l10gl_gl_texture {
    struct l10gl_gl_texture *next;
    GLuint name;
    struct l10gl_texture texture;
    enum l10gl_tex_filter filter;
    enum l10gl_tex_wrap wrap;
    int is_texture;
    int uploaded;
    /* Q4: retained converted ARGB8888 image so glTexSubImage2D can update a
     * subrectangle in shim-side memory and re-upload the whole level. */
    uint32_t *retained;
    GLsizei retained_width;
    GLsizei retained_height;
};

struct l10gl_gl_state {
    struct l10gl_ctx owned_context;
    struct l10gl_ctx *current;
    GLenum error;
    GLenum cull_face;
    GLenum shade_model;
    struct l10gl_gl_texture default_texture;
    struct l10gl_gl_texture *textures;
    struct l10gl_gl_texture *bound_texture;
    GLuint next_texture_name;
    GLint unpack_alignment;
    int owns_context;
    int cull_enabled;
    int lighting_enabled;
    int light0_enabled;
    int normalize_enabled;
    int texture_2d_enabled;
    GLenum env_mode;        /* GL_MODULATE/REPLACE/DECAL (Q6 wires it in) */
    GLenum alpha_func;      /* GL_ALPHA_TEST compare (Q5 wires it in) */
    GLclampf alpha_ref;     /* GL_ALPHA_TEST reference (Q5 wires it in) */
    GLenum draw_buffer;     /* GL_BACK; GL_FRONT unsupported (Q1) */
    GLenum read_buffer;     /* GL_FRONT/GL_BACK (Q1) */
    GLenum polygon_mode;    /* GL_FILL is the only drawn mode (Q1) */
};

static struct l10gl_gl_state gl_state = {
    .cull_face = GL_BACK,
    .shade_model = GL_SMOOTH,
};

static void gl_init_texture(struct l10gl_gl_texture *texture, GLuint name)
{
    memset(texture, 0, sizeof(*texture));
    texture->name = name;
    texture->filter = L10GL_FILTER_NEAREST;
    texture->wrap = L10GL_WRAP_REPEAT;
}

static void gl_release_textures(void)
{
    struct l10gl_gl_texture *texture = gl_state.textures;

    if (gl_state.current)
        l10gl_bind_texture(gl_state.current, NULL);
    while (texture) {
        struct l10gl_gl_texture *next = texture->next;

        free(texture->retained);
        free(texture);
        texture = next;
    }
    gl_state.textures = NULL;
    gl_state.bound_texture = NULL;
    memset(&gl_state.default_texture, 0, sizeof(gl_state.default_texture));
}

static void gl_record_error(GLenum error)
{
    if (gl_state.error == GL_NO_ERROR)
        gl_state.error = error;
}

static struct l10gl_ctx *gl_current(void)
{
    if (!gl_state.current)
        gl_record_error(GL_INVALID_OPERATION);
    return gl_state.current;
}

static void gl_reset_compat_state(void)
{
    gl_state.error = GL_NO_ERROR;
    gl_state.cull_face = GL_BACK;
    gl_state.shade_model = GL_SMOOTH;
    gl_state.cull_enabled = 0;
    gl_state.lighting_enabled = 0;
    gl_state.light0_enabled = 0;
    gl_state.normalize_enabled = 0;
    gl_state.texture_2d_enabled = 0;
    gl_state.env_mode = GL_MODULATE;
    gl_state.alpha_func = GL_ALWAYS;
    gl_state.alpha_ref = 0.0f;
    gl_state.draw_buffer = GL_BACK;
    gl_state.read_buffer = GL_BACK;
    gl_state.polygon_mode = GL_FILL;
    gl_init_texture(&gl_state.default_texture, 0);
    gl_state.bound_texture = &gl_state.default_texture;
    gl_state.next_texture_name = 1;
    gl_state.unpack_alignment = 4;
}

int l10glCreateContext(GLsizei width, GLsizei height, GLint bits_per_pixel)
{
    int ret;

    if (gl_state.owns_context)
        return -EBUSY;
    if (width <= 0 || height <= 0 ||
        (bits_per_pixel != 16 && bits_per_pixel != 32))
        return -EINVAL;

    ret = l10gl_create_auto(&gl_state.owned_context, width, height,
                            bits_per_pixel / 8);
    if (ret)
        return ret;

    gl_release_textures();
    gl_state.current = &gl_state.owned_context;
    gl_state.owns_context = 1;
    gl_reset_compat_state();

    /* l10gl's native default is depth-on; OpenGL 1.1 defaults it off. */
    l10gl_enable_depth_test(gl_state.current, 0);
    l10gl_cull_face(gl_state.current, L10GL_CULL_NONE);
    l10gl_enable_lighting(gl_state.current, 0);
    return 0;
}

void l10glDestroyContext(void)
{
    gl_release_textures();
    if (gl_state.owns_context)
        l10gl_destroy(&gl_state.owned_context);
    memset(&gl_state.owned_context, 0, sizeof(gl_state.owned_context));
    gl_state.current = NULL;
    gl_state.owns_context = 0;
    gl_reset_compat_state();
}

void l10glMakeCurrent(struct l10gl_ctx *ctx)
{
    gl_release_textures();
    gl_state.current = ctx;
    gl_reset_compat_state();
}

struct l10gl_ctx *l10glGetCurrentContext(void)
{
    return gl_state.current;
}

void l10glSwapBuffers(void)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_swap_buffers(ctx);
}

GLenum glGetError(void)
{
    GLenum error = gl_state.error;

    gl_state.error = GL_NO_ERROR;
    return error;
}

static int gl_primitive(GLenum mode, enum l10gl_primitive *primitive)
{
    switch (mode) {
    case GL_TRIANGLES:      *primitive = L10GL_TRIANGLES; return 0;
    case GL_TRIANGLE_STRIP: *primitive = L10GL_TRIANGLE_STRIP; return 0;
    case GL_TRIANGLE_FAN:   *primitive = L10GL_TRIANGLE_FAN; return 0;
    case GL_POLYGON:        *primitive = L10GL_POLYGON; return 0;
    case GL_QUADS:          *primitive = L10GL_QUADS; return 0;
    case GL_QUAD_STRIP:     *primitive = L10GL_QUAD_STRIP; return 0;
    case GL_LINES:          *primitive = L10GL_LINES; return 0;
    case GL_LINE_STRIP:     *primitive = L10GL_LINE_STRIP; return 0;
    default: return -1;
    }
}

void glBegin(GLenum mode)
{
    struct l10gl_ctx *ctx = gl_current();
    enum l10gl_primitive primitive;

    if (!ctx)
        return;
    if (gl_primitive(mode, &primitive)) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (l10gl_begin(ctx, primitive))
        gl_record_error(GL_INVALID_OPERATION);
}

void glEnd(void)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx && l10gl_end(ctx))
        gl_record_error(GL_INVALID_OPERATION);
}

void glVertex2f(GLfloat x, GLfloat y)
{
    glVertex3f(x, y, 0.0f);
}

void glVertex3f(GLfloat x, GLfloat y, GLfloat z)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx && l10gl_vertex3f(ctx, x, y, z))
        gl_record_error(GL_INVALID_OPERATION);
}

void glVertex3fv(const GLfloat *v)
{
    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glVertex3f(v[0], v[1], v[2]);
}

void glColor3f(GLfloat red, GLfloat green, GLfloat blue)
{
    glColor4f(red, green, blue, 1.0f);
}

void glColor4f(GLfloat red, GLfloat green, GLfloat blue, GLfloat alpha)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_color4f(ctx, red, green, blue, alpha);
}

void glColor3fv(const GLfloat *v)
{
    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glColor3f(v[0], v[1], v[2]);
}

void glColor4fv(const GLfloat *v)
{
    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glColor4f(v[0], v[1], v[2], v[3]);
}

void glColor3ubv(const GLubyte *v)
{
    static const float scale = 1.0f / 255.0f;

    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glColor3f(v[0] * scale, v[1] * scale, v[2] * scale);
}

void glNormal3f(GLfloat nx, GLfloat ny, GLfloat nz)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_normal3f(ctx, nx, ny, nz);
}

void glNormal3fv(const GLfloat *v)
{
    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glNormal3f(v[0], v[1], v[2]);
}

void glTexCoord2f(GLfloat s, GLfloat t)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_texcoord2f(ctx, s, t);
}

void glTexCoord2fv(const GLfloat *v)
{
    if (!v) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    glTexCoord2f(v[0], v[1]);
}

void glMatrixMode(GLenum mode)
{
    struct l10gl_ctx *ctx = gl_current();
    enum l10gl_matrix_mode target;

    if (!ctx)
        return;
    if (mode == GL_MODELVIEW)
        target = L10GL_MATRIX_MODELVIEW;
    else if (mode == GL_PROJECTION)
        target = L10GL_MATRIX_PROJECTION;
    else {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (l10gl_matrix_mode(ctx, target))
        gl_record_error(GL_INVALID_OPERATION);
}

void glLoadIdentity(void)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_load_identity(ctx);
}

void glLoadMatrixf(const GLfloat *m)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!m) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (ctx)
        l10gl_load_matrixf(ctx, m);
}

void glMultMatrixf(const GLfloat *m)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!m) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (ctx)
        l10gl_mult_matrixf(ctx, m);
}

void glPushMatrix(void)
{
    struct l10gl_ctx *ctx = gl_current();
    int ret;

    if (!ctx)
        return;
    ret = l10gl_push_matrix(ctx);
    if (ret == -EOVERFLOW)
        gl_record_error(GL_STACK_OVERFLOW);
    else if (ret)
        gl_record_error(GL_INVALID_OPERATION);
}

void glPopMatrix(void)
{
    struct l10gl_ctx *ctx = gl_current();
    int ret;

    if (!ctx)
        return;
    ret = l10gl_pop_matrix(ctx);
    if (ret == -ERANGE)
        gl_record_error(GL_STACK_UNDERFLOW);
    else if (ret)
        gl_record_error(GL_INVALID_OPERATION);
}

void glTranslatef(GLfloat x, GLfloat y, GLfloat z)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_translatef(ctx, x, y, z);
}

void glRotatef(GLfloat angle, GLfloat x, GLfloat y, GLfloat z)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx && l10gl_rotatef(ctx, angle, x, y, z))
        gl_record_error(GL_INVALID_VALUE);
}

void glScalef(GLfloat x, GLfloat y, GLfloat z)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_scalef(ctx, x, y, z);
}

void glFrustum(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
               GLdouble z_near, GLdouble z_far)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx && l10gl_frustum(ctx, left, right, bottom, top, z_near, z_far))
        gl_record_error(GL_INVALID_VALUE);
}

void glOrtho(GLdouble left, GLdouble right, GLdouble bottom, GLdouble top,
             GLdouble z_near, GLdouble z_far)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx && l10gl_ortho(ctx, left, right, bottom, top, z_near, z_far))
        gl_record_error(GL_INVALID_VALUE);
}

void glViewport(GLint x, GLint y, GLsizei width, GLsizei height)
{
    struct l10gl_ctx *ctx = gl_current();

    if (width < 0 || height < 0) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (ctx && l10gl_viewport(ctx, x, y, width, height))
        gl_record_error(GL_INVALID_VALUE);
}

void glDepthRange(GLclampd z_near, GLclampd z_far)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!ctx)
        return;
    if (z_near < 0) z_near = 0;
    if (z_near > 1) z_near = 1;
    if (z_far < 0) z_far = 0;
    if (z_far > 1) z_far = 1;
    l10gl_depth_range(ctx, z_near, z_far);
}

void glClearColor(GLclampf red, GLclampf green, GLclampf blue, GLclampf alpha)
{
    struct l10gl_ctx *ctx = gl_current();
    (void)alpha;

    if (!ctx)
        return;
    if (red < 0)
        red = 0;
    if (red > 1)
        red = 1;
    if (green < 0)
        green = 0;
    if (green > 1)
        green = 1;
    if (blue < 0)
        blue = 0;
    if (blue > 1)
        blue = 1;
    l10gl_clear_color(ctx, red, green, blue);
}

void glClearDepth(GLclampd depth)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!ctx)
        return;
    if (depth < 0) depth = 0;
    if (depth > 1) depth = 1;
    l10gl_clear_depth(ctx, depth);
}

void glClear(GLbitfield mask)
{
    struct l10gl_ctx *ctx = gl_current();
    const GLbitfield supported = GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT;

    if (!ctx)
        return;
    if (mask & ~supported) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if ((mask & GL_DEPTH_BUFFER_BIT) && ctx->backend->clear_depth &&
        (ctx->backend->caps & L10GL_CAP_ZBUFFER))
        ctx->backend->clear_depth(ctx, ctx->clear_z);
    if ((mask & GL_COLOR_BUFFER_BIT) && ctx->backend->clear_color)
        ctx->backend->clear_color(ctx, ctx->clear_r, ctx->clear_g,
                                  ctx->clear_b);
}

static void gl_apply_lighting(struct l10gl_ctx *ctx)
{
    l10gl_enable_lighting(ctx,
                          gl_state.lighting_enabled &&
                          gl_state.light0_enabled);
}

static void gl_apply_texture_binding(struct l10gl_ctx *ctx)
{
    struct l10gl_gl_texture *object = gl_state.bound_texture;

    if (!gl_state.texture_2d_enabled || !object || !object->uploaded) {
        l10gl_bind_texture(ctx, NULL);
        return;
    }
    l10gl_bind_texture(ctx, &object->texture);
    /* Backends expose one active filter and wrap mode. Reapply the bound
     * object's cached values so GL object switching remains deterministic. */
    l10gl_tex_parameter(ctx, object->filter, object->wrap);
}

static void gl_set_enable(GLenum cap, int enable)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!ctx)
        return;
    switch (cap) {
    case GL_DEPTH_TEST:
        l10gl_enable_depth_test(ctx, enable);
        break;
    case GL_BLEND:
        l10gl_enable_blend(ctx, enable);
        break;
    case GL_CULL_FACE:
        gl_state.cull_enabled = enable;
        l10gl_cull_face(ctx, enable ? (gl_state.cull_face == GL_FRONT ?
                          L10GL_CULL_FRONT : L10GL_CULL_BACK) :
                          L10GL_CULL_NONE);
        break;
    case GL_LIGHTING:
        gl_state.lighting_enabled = enable;
        gl_apply_lighting(ctx);
        break;
    case GL_LIGHT0:
        gl_state.light0_enabled = enable;
        gl_apply_lighting(ctx);
        break;
    case GL_NORMALIZE:
        /* Phase 2 always normalizes transformed normals. */
        gl_state.normalize_enabled = enable;
        break;
    case GL_TEXTURE_2D:
        gl_state.texture_2d_enabled = enable;
        gl_apply_texture_binding(ctx);
        break;
    default:
        gl_record_error(GL_INVALID_ENUM);
        break;
    }
}

void glEnable(GLenum cap)
{
    gl_set_enable(cap, 1);
}

void glDisable(GLenum cap)
{
    gl_set_enable(cap, 0);
}

void glCullFace(GLenum mode)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!ctx)
        return;
    if (mode != GL_FRONT && mode != GL_BACK) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    gl_state.cull_face = mode;
    if (gl_state.cull_enabled)
        l10gl_cull_face(ctx, mode == GL_FRONT ? L10GL_CULL_FRONT :
                                               L10GL_CULL_BACK);
}

void glDepthFunc(GLenum func)
{
    struct l10gl_ctx *ctx = gl_current();
    enum l10gl_depth_func mapped;

    if (!ctx)
        return;
    switch (func) {
    case GL_NEVER:    mapped = L10GL_NEVER; break;
    case GL_LESS:     mapped = L10GL_LESS; break;
    case GL_EQUAL:    mapped = L10GL_EQUAL; break;
    case GL_LEQUAL:   mapped = L10GL_LEQUAL; break;
    case GL_GREATER:  mapped = L10GL_GREATER; break;
    case GL_NOTEQUAL: mapped = L10GL_NOTEQUAL; break;
    case GL_GEQUAL:   mapped = L10GL_GEQUAL; break;
    case GL_ALWAYS:   mapped = L10GL_ALWAYS; break;
    default: gl_record_error(GL_INVALID_ENUM); return;
    }
    l10gl_depth_func(ctx, mapped);
}

void glDepthMask(GLboolean flag)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_depth_mask(ctx, flag != GL_FALSE);
}

static int gl_blend_factor(GLenum factor, enum l10gl_blend_func *mapped)
{
    switch (factor) {
    case GL_ZERO:                *mapped = L10GL_ZERO; return 0;
    case GL_ONE:                 *mapped = L10GL_ONE; return 0;
    case GL_SRC_COLOR:           *mapped = L10GL_SRC_COLOR; return 0;
    case GL_ONE_MINUS_SRC_COLOR: *mapped = L10GL_ONE_MINUS_SRC_COLOR; return 0;
    case GL_SRC_ALPHA:           *mapped = L10GL_SRC_ALPHA; return 0;
    case GL_ONE_MINUS_SRC_ALPHA: *mapped = L10GL_ONE_MINUS_SRC_ALPHA; return 0;
    case GL_DST_COLOR:           *mapped = L10GL_DST_COLOR; return 0;
    case GL_ONE_MINUS_DST_COLOR: *mapped = L10GL_ONE_MINUS_DST_COLOR; return 0;
    default: return -1;
    }
}

void glBlendFunc(GLenum sfactor, GLenum dfactor)
{
    struct l10gl_ctx *ctx = gl_current();
    enum l10gl_blend_func source, destination;

    if (!ctx)
        return;
    if (gl_blend_factor(sfactor, &source) ||
        gl_blend_factor(dfactor, &destination)) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    l10gl_blend_func(ctx, source, destination);
}

void glShadeModel(GLenum mode)
{
    if (!gl_current())
        return;
    if (mode != GL_FLAT && mode != GL_SMOOTH) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    gl_state.shade_model = mode;
    l10gl_shade_flat(gl_state.current, mode == GL_FLAT);
}

void glLightfv(GLenum light, GLenum pname, const GLfloat *params)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!params) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (!ctx)
        return;
    if (light != GL_LIGHT0) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }

    switch (pname) {
    case GL_AMBIENT:
        l10gl_light_ambient(ctx, params[0], params[1], params[2]);
        break;
    case GL_DIFFUSE:
        l10gl_light_color(ctx, params[0], params[1], params[2]);
        break;
    case GL_POSITION: {
        float modelview[16];
        float position[4] = { params[0], params[1], params[2], params[3] };
        float eye_direction[4];

        /* Phase 2 implements one directional light. A w=0 GL position is a
         * direction toward the light and is transformed by the current
         * MODELVIEW at call time. l10gl_light_dir stores the opposite ray
         * travel direction. */
        if (params[3] != 0.0f) {
            gl_record_error(GL_INVALID_VALUE);
            return;
        }
        if (l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, modelview)) {
            gl_record_error(GL_INVALID_OPERATION);
            return;
        }
        l10gl_transform_vec4(modelview, position, eye_direction);
        if (l10gl_light_dir(ctx, -eye_direction[0], -eye_direction[1],
                           -eye_direction[2]))
            gl_record_error(GL_INVALID_VALUE);
        break;
    }
    default:
        gl_record_error(GL_INVALID_ENUM);
        break;
    }
}

void glMaterialfv(GLenum face, GLenum pname, const GLfloat *params)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!params) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (!ctx)
        return;
    if (face != GL_FRONT && face != GL_FRONT_AND_BACK) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (pname != GL_AMBIENT && pname != GL_DIFFUSE &&
        pname != GL_AMBIENT_AND_DIFFUSE) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }

    /* L10GL's deliberately small lighting model has one RGB reflectance,
     * shared by ambient and diffuse. This exactly covers gears' use of
     * GL_AMBIENT_AND_DIFFUSE and is the documented fallback for either
     * component alone. */
    l10gl_material(ctx, params[0], params[1], params[2], params[3]);
}

static struct l10gl_gl_texture *gl_find_texture(GLuint name)
{
    struct l10gl_gl_texture *texture;

    if (name == 0)
        return &gl_state.default_texture;
    for (texture = gl_state.textures; texture; texture = texture->next)
        if (texture->name == name)
            return texture;
    return NULL;
}

static struct l10gl_gl_texture *gl_alloc_texture(GLuint name, int is_texture)
{
    struct l10gl_gl_texture *texture = malloc(sizeof(*texture));

    if (!texture)
        return NULL;
    gl_init_texture(texture, name);
    texture->is_texture = is_texture;
    texture->next = gl_state.textures;
    gl_state.textures = texture;
    return texture;
}

static GLuint gl_allocate_texture_name(void)
{
    GLuint candidate = gl_state.next_texture_name;

    while (candidate != 0 && gl_find_texture(candidate))
        candidate++;
    if (candidate == 0)
        return 0;
    gl_state.next_texture_name = candidate + 1;
    return candidate;
}

void glGenTextures(GLsizei n, GLuint *textures)
{
    if (!gl_current())
        return;
    if (n < 0 || (n > 0 && !textures)) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        GLuint name = gl_allocate_texture_name();

        if (!name || !gl_alloc_texture(name, 0)) {
            gl_record_error(GL_OUT_OF_MEMORY);
            for (; i < n; i++)
                textures[i] = 0;
            return;
        }
        textures[i] = name;
    }
}

void glDeleteTextures(GLsizei n, const GLuint *textures)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!ctx)
        return;
    if (n < 0 || (n > 0 && !textures)) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    for (GLsizei i = 0; i < n; i++) {
        struct l10gl_gl_texture **link = &gl_state.textures;

        if (textures[i] == 0)
            continue;
        while (*link && (*link)->name != textures[i])
            link = &(*link)->next;
        if (*link) {
            struct l10gl_gl_texture *dead = *link;

            *link = dead->next;
            if (gl_state.bound_texture == dead) {
                gl_state.bound_texture = &gl_state.default_texture;
                gl_apply_texture_binding(ctx);
            }
            /* Backend allocations intentionally live until context teardown:
             * swrast owns its allocation list and ViRGE uses a VRAM bump
             * allocator. Only the GL name/object metadata (and the Q4
             * retained CPU image) is reclaimed here. */
            free(dead->retained);
            free(dead);
        }
    }
}

GLboolean glIsTexture(GLuint texture)
{
    struct l10gl_gl_texture *object;

    if (!gl_current())
        return GL_FALSE;
    object = gl_find_texture(texture);
    return object && object->is_texture ? GL_TRUE : GL_FALSE;
}

void glBindTexture(GLenum target, GLuint texture)
{
    struct l10gl_ctx *ctx = gl_current();
    struct l10gl_gl_texture *object;

    if (!ctx)
        return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    object = gl_find_texture(texture);
    if (!object && texture != 0)
        object = gl_alloc_texture(texture, 1);
    if (!object) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    if (texture != 0)
        object->is_texture = 1;
    gl_state.bound_texture = object;
    gl_apply_texture_binding(ctx);
}

static int gl_texture_filter(GLint param, int magnification,
                             enum l10gl_tex_filter *filter)
{
    switch ((GLenum)param) {
    case GL_NEAREST:
        *filter = L10GL_FILTER_NEAREST;
        return 0;
    case GL_LINEAR:
        *filter = L10GL_FILTER_LINEAR;
        return 0;
    case GL_NEAREST_MIPMAP_NEAREST:
    case GL_NEAREST_MIPMAP_LINEAR:
        if (magnification)
            return -1;
        *filter = L10GL_FILTER_NEAREST;
        return 0;
    case GL_LINEAR_MIPMAP_NEAREST:
    case GL_LINEAR_MIPMAP_LINEAR:
        if (magnification)
            return -1;
        *filter = L10GL_FILTER_LINEAR;
        return 0;
    default:
        return -1;
    }
}

void glTexParameteri(GLenum target, GLenum pname, GLint param)
{
    struct l10gl_ctx *ctx = gl_current();
    struct l10gl_gl_texture *object = gl_state.bound_texture;
    enum l10gl_tex_filter filter;

    if (!ctx)
        return;
    if (target != GL_TEXTURE_2D) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (!object) {
        gl_record_error(GL_INVALID_OPERATION);
        return;
    }

    switch (pname) {
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_MAG_FILTER:
        if (gl_texture_filter(param, pname == GL_TEXTURE_MAG_FILTER,
                              &filter)) {
            gl_record_error(GL_INVALID_ENUM);
            return;
        }
        /* There is no LOD derivative in the Phase 2 contract, so minification
         * and magnification share the backend's single filter selector. The
         * most recently specified one is the effective object setting. */
        object->filter = filter;
        break;
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T:
        if ((GLenum)param == GL_REPEAT)
            object->wrap = L10GL_WRAP_REPEAT;
        else if ((GLenum)param == GL_CLAMP)
            object->wrap = L10GL_WRAP_CLAMP;
        else {
            gl_record_error(GL_INVALID_ENUM);
            return;
        }
        /* ViRGE exposes one wrap bit for both axes; the most recently
         * specified S/T value is therefore effective for the object. */
        break;
    default:
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (gl_state.texture_2d_enabled && object->uploaded)
        gl_apply_texture_binding(ctx);
}

void glTexParameterf(GLenum target, GLenum pname, GLfloat param)
{
    /* GLQuake selects filters and wrap modes through the float entry point.
     * The arguments are enum values, so this delegates to the integer path. */
    glTexParameteri(target, pname, (GLint)param);
}

void glPixelStorei(GLenum pname, GLint param)
{
    if (!gl_current())
        return;
    if (pname != GL_UNPACK_ALIGNMENT) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (param != 1 && param != 2 && param != 4 && param != 8) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    gl_state.unpack_alignment = param;
}

static int gl_is_power_of_two(GLsizei value)
{
    return value > 0 && (value & (value - 1)) == 0;
}

/* Pack one source texel (GL_RGB or GL_RGBA, GL_UNSIGNED_BYTE) into the
 * ARGB8888 word the backends store. A NULL texel yields the same defaults
 * glTexImage2D uses for an unpack source of NULL (zero RGB; alpha 255 for
 * GL_RGB, 0 for GL_RGBA). Shared by glTexImage2D and glTexSubImage2D. */
static uint32_t gl_pack_texel_argb(const uint8_t *texel, GLenum format)
{
    uint8_t red = 0, green = 0, blue = 0;
    uint8_t alpha = format == GL_RGB ? 255 : 0;

    if (texel) {
        red = texel[0];
        green = texel[1];
        blue = texel[2];
        if (format == GL_RGBA)
            alpha = texel[3];
    }
    return ((uint32_t)alpha << 24) | ((uint32_t)red << 16) |
           ((uint32_t)green << 8) | blue;
}

void glTexImage2D(GLenum target, GLint level, GLint internal_format,
                  GLsizei width, GLsizei height, GLint border,
                  GLenum format, GLenum type, const GLvoid *pixels)
{
    struct l10gl_ctx *ctx = gl_current();
    struct l10gl_gl_texture *object = gl_state.bound_texture;
    const uint8_t *source = pixels;
    uint32_t *converted;
    size_t components, row_bytes, source_stride, pixel_count;
    int ret;

    if (!ctx)
        return;
    if (target != GL_TEXTURE_2D ||
        (format != GL_RGB && format != GL_RGBA) ||
        type != GL_UNSIGNED_BYTE) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    /* Each side must be a positive power of two up to 512. DB019-B §19.4
     * (PDF p.251) caps a ViRGE texture side at 2^9; swrast has no such limit
     * but the shim applies the conservative ViRGE maximum uniformly.
     * Rectangular (width != height) POT images are accepted: swrast samples
     * them natively, and the ViRGE backend represents a rectangle as its
     * bounding square with the short axis tile-replicated (Q3). */
    if (level != 0 || border != 0 ||
        (internal_format != 3 && internal_format != 4 &&
         internal_format != (GLint)GL_RGB &&
         internal_format != (GLint)GL_RGBA) ||
        width < 1 || height < 1 ||
        width > 512 || height > 512 ||
        !gl_is_power_of_two(width) || !gl_is_power_of_two(height)) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    if (!object) {
        gl_record_error(GL_INVALID_OPERATION);
        return;
    }
    if (!(ctx->backend->caps & L10GL_CAP_TEXTURE) ||
        !ctx->backend->tex_image_2d) {
        gl_record_error(GL_INVALID_OPERATION);
        return;
    }

    components = format == GL_RGBA ? 4u : 3u;
    if ((size_t)width > SIZE_MAX / components) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    row_bytes = (size_t)width * components;
    if (row_bytes > SIZE_MAX - (size_t)(gl_state.unpack_alignment - 1)) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    source_stride = (row_bytes + (size_t)(gl_state.unpack_alignment - 1)) &
                    ~(size_t)(gl_state.unpack_alignment - 1);
    pixel_count = (size_t)width * (size_t)height;
    if (pixel_count > SIZE_MAX / sizeof(*converted)) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    converted = malloc(pixel_count * sizeof(*converted));
    if (!converted) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }

    for (GLsizei y = 0; y < height; y++) {
        for (GLsizei x = 0; x < width; x++) {
            const uint8_t *texel = source ? source + (size_t)y * source_stride +
                                                 (size_t)x * components
                                          : NULL;
            converted[(size_t)y * (size_t)width + (size_t)x] =
                gl_pack_texel_argb(texel, format);
        }
    }

    ret = l10gl_tex_image_2d(ctx, &object->texture, width, height,
                             L10GL_TEX_FMT_ARGB8888, converted);
    if (ret) {
        free(converted);
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    /* Retain the converted image for glTexSubImage2D, replacing any prior
     * retained copy from a previous upload of this name. */
    free(object->retained);
    object->retained = converted;
    object->retained_width = width;
    object->retained_height = height;
    object->uploaded = 1;
    object->is_texture = object->name != 0;
    if (gl_state.texture_2d_enabled)
        gl_apply_texture_binding(ctx);
}

void glFlush(void)
{
    /* L10GL submits register writes synchronously; there is no client queue. */
    (void)gl_current();
}

void glFinish(void)
{
    struct l10gl_ctx *ctx = gl_current();

    if (ctx)
        l10gl_wait_engine(ctx);
}

const GLubyte *glGetString(GLenum name)
{
    static const GLubyte vendor[] = "L10GL";
    /* Honest pre-1.1 tier: L10GL implements a GL 1.1 subset, not the whole
     * 1.1 surface, so the version must not claim 1.1 (Phase 8 raises it). */
    static const GLubyte version[] = "1.0 L10GL (Phase 7 compatibility)";
    /* Empty-but-valid extensions string: GLQuake probes SGIS_multitexture
     * and EXT_shared_texture_palette here and falls back cleanly. */
    static const GLubyte extensions[] = "";
    static GLubyte renderer[64];

    switch (name) {
    case GL_VENDOR:
        return vendor;
    case GL_VERSION:
        return version;
    case GL_EXTENSIONS:
        return extensions;
    case GL_RENDERER:
        if (gl_state.current && gl_state.current->backend &&
            gl_state.current->backend->name)
            snprintf((char *)renderer, sizeof(renderer), "L10GL/%s",
                     gl_state.current->backend->name);
        else
            snprintf((char *)renderer, sizeof(renderer), "L10GL");
        return renderer;
    default:
        gl_record_error(GL_INVALID_ENUM);
        return NULL;
    }
}

void glGetFloatv(GLenum pname, GLfloat *params)
{
    struct l10gl_ctx *ctx = gl_current();

    if (!params)
        return;
    if (!ctx)
        return;
    /* Full query family is Phase 8 C1; only the modelview capture GLQuake
     * needs (gl_rmain.c: r_world_matrix) is served today. */
    switch (pname) {
    case GL_MODELVIEW_MATRIX:
        l10gl_get_matrix(ctx, L10GL_MATRIX_MODELVIEW, params);
        break;
    default:
        gl_record_error(GL_INVALID_ENUM);
    }
}

void glHint(GLenum target, GLenum mode)
{
    if (!gl_current())
        return;
    if (target != GL_PERSPECTIVE_CORRECTION_HINT) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (mode != GL_DONT_CARE && mode != GL_FASTEST && mode != GL_NICEST) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    /* No-op: L10GL's perspective path is fixed, so the hint has no effect. */
}

void glPolygonMode(GLenum face, GLenum mode)
{
    if (!gl_current())
        return;
    if (face != GL_FRONT && face != GL_BACK && face != GL_FRONT_AND_BACK) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (mode != GL_FILL) {
        /* GL_POINT/GL_LINE rasterization is not implemented. */
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    gl_state.polygon_mode = GL_FILL;
}

void glDrawBuffer(GLenum mode)
{
    if (!gl_current())
        return;
    if (mode != GL_BACK && mode != GL_FRONT && mode != GL_FRONT_AND_BACK) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (mode == GL_BACK) {
        gl_state.draw_buffer = GL_BACK;
        return;
    }
    /* Direct front-buffer drawing is the L10GL_VSYNC=0 path; the GL entry
     * point reports unsupported until a use case needs it. */
    gl_record_error(GL_INVALID_OPERATION);
}

void glReadBuffer(GLenum mode)
{
    if (!gl_current())
        return;
    if (mode != GL_FRONT && mode != GL_BACK) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    gl_state.read_buffer = mode;
}

void glReadPixels(GLint x, GLint y, GLsizei width, GLsizei height,
                  GLenum format, GLenum type, GLvoid *data)
{
    (void)x;
    (void)y;
    (void)width;
    (void)height;
    (void)format;
    (void)type;
    (void)data;
    /* Framebuffer readback is a Phase 8 C7 path (the ViRGE CPU aperture is
     * an unreliable read path). Record the error and write nothing. */
    gl_record_error(GL_INVALID_OPERATION);
}

void glAlphaFunc(GLenum func, GLclampf ref)
{
    if (!gl_current())
        return;
    switch (func) {
    case GL_NEVER:
    case GL_LESS:
    case GL_EQUAL:
    case GL_LEQUAL:
    case GL_GREATER:
    case GL_NOTEQUAL:
    case GL_GEQUAL:
    case GL_ALWAYS:
        break;
    default:
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (ref < 0.0f)
        ref = 0.0f;
    else if (ref > 1.0f)
        ref = 1.0f;
    gl_state.alpha_func = func;
    gl_state.alpha_ref = ref;
    /* Q5 wires alpha_func/ref into the swrast fragment stage. The GL
     * default (ALWAYS, 0) passes every fragment, matching current behavior. */
}

void glTexEnvf(GLenum target, GLenum pname, GLfloat param)
{
    if (!gl_current())
        return;
    if (target != GL_TEXTURE_ENV || pname != GL_TEXTURE_ENV_MODE) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    switch ((GLenum)param) {
    case GL_MODULATE:
    case GL_DECAL:
    case GL_REPLACE:
        break;
    default:
        /* GL_BLEND/GL_ADD texture environments are out of scope (Phase 8). */
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    gl_state.env_mode = (GLenum)param;
    /* Q6 wires env_mode into the fragment path; MODULATE is the current
     * fixed behavior, so this is observationally a no-op until then. */
}

void glTexSubImage2D(GLenum target, GLint level, GLint xoffset, GLint yoffset,
                     GLsizei width, GLsizei height, GLenum format,
                     GLenum type, const GLvoid *pixels)
{
    struct l10gl_ctx *ctx = gl_current();
    struct l10gl_gl_texture *object = gl_state.bound_texture;
    const uint8_t *source = pixels;
    size_t components, row_bytes, source_stride;

    if (!ctx)
        return;
    if (target != GL_TEXTURE_2D ||
        (format != GL_RGB && format != GL_RGBA) ||
        type != GL_UNSIGNED_BYTE) {
        gl_record_error(GL_INVALID_ENUM);
        return;
    }
    if (level != 0 || width < 0 || height < 0 || xoffset < 0 || yoffset < 0) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    /* No retained level image (no prior glTexImage2D on this name) means
     * there is nothing to update. */
    if (!object || !object->uploaded || !object->retained) {
        gl_record_error(GL_INVALID_OPERATION);
        return;
    }
    if (xoffset + width > object->retained_width ||
        yoffset + height > object->retained_height) {
        gl_record_error(GL_INVALID_VALUE);
        return;
    }
    /* An empty subrectangle is a no-op (and avoids a needless re-upload). */
    if (width == 0 || height == 0)
        return;

    components = format == GL_RGBA ? 4u : 3u;
    if ((size_t)width > SIZE_MAX / components) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    row_bytes = (size_t)width * components;
    if (row_bytes > SIZE_MAX - (size_t)(gl_state.unpack_alignment - 1)) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    source_stride = (row_bytes + (size_t)(gl_state.unpack_alignment - 1)) &
                    ~(size_t)(gl_state.unpack_alignment - 1);

    /* Apply the subrectangle to the retained ARGB8888 image in place. */
    for (GLsizei y = 0; y < height; y++) {
        for (GLsizei x = 0; x < width; x++) {
            const uint8_t *texel = source ? source + (size_t)y * source_stride +
                                                 (size_t)x * components
                                          : NULL;
            object->retained[(size_t)(yoffset + y) *
                                 (size_t)object->retained_width +
                             (size_t)(xoffset + x)] =
                gl_pack_texel_argb(texel, format);
        }
    }

    /* Re-upload the whole level through the existing backend path. The
     * per-frame full re-upload on ViRGE is a known cost (Q12), not a
     * correctness issue. */
    if (l10gl_tex_image_2d(ctx, &object->texture, object->retained_width,
                           object->retained_height, L10GL_TEX_FMT_ARGB8888,
                           object->retained)) {
        gl_record_error(GL_OUT_OF_MEMORY);
        return;
    }
    if (gl_state.texture_2d_enabled)
        gl_apply_texture_binding(ctx);
}
