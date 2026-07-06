/*
 * textured_cube.c - Spinning textured cube demo using L10GL.
 *
 * Hardware-agnostic — uses the L10GL frontend API. The backend is selected
 * at compile time. If the backend supports texture mapping (ViRGE), it
 * uses hardware textured triangles. If not (MGA-1064SG), it silently
 * falls back to Gouraud shading.
 *
 * Build: make DEMO=textured_cube
 * Run:   sudo ./textured_cube [width height bpp]
 *
 * Controls: Ctrl-C to exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>

#include "l10gl.h"

/* -----------------------------------------------------------------------
 * Minimal 3D Math
 * ----------------------------------------------------------------------- */

#define PI 3.14159265358979323846f

static void build_rotation(float m[3][3], float angle_x, float angle_y)
{
    float sx = sinf(angle_x), cx = cosf(angle_x);
    float sy = sinf(angle_y), cy = cosf(angle_y);

    m[0][0] = cy;     m[0][1] = 0;      m[0][2] = sy;
    m[1][0] = sx*sy;  m[1][1] = cx;     m[1][2] = -sx*cy;
    m[2][0] = -cx*sy; m[2][1] = sx;     m[2][2] = cx*cy;
}

static void mat3_transform(float out[3], const float m[3][3], const float in[3])
{
    out[0] = m[0][0]*in[0] + m[0][1]*in[1] + m[0][2]*in[2];
    out[1] = m[1][0]*in[0] + m[1][1]*in[1] + m[1][2]*in[2];
    out[2] = m[2][0]*in[0] + m[2][1]*in[1] + m[2][2]*in[2];
}

struct screen_vertex {
    float sx, sy, sz, sw;
};

static void project(struct screen_vertex *out, const float in[3],
                     int screen_w, int screen_h, float camera_dist)
{
    float z = in[2] + camera_dist;
    if (z < 0.1f) z = 0.1f;

    float scale = (float)screen_h / z;

    out->sx = (float)screen_w * 0.5f + in[0] * scale;
    out->sy = (float)screen_h * 0.5f - in[1] * scale;
    out->sz = (in[2] + camera_dist) / 1000.0f;
    out->sw = 1.0f / z;  /* 1/Z_eye for perspective-correct texturing */

    if (out->sz < 0.0f) out->sz = 0.0f;
    if (out->sz > 1.0f) out->sz = 1.0f;
}

/* -----------------------------------------------------------------------
 * Cube Geometry
 *
 * UV coordinates map each face to the full [0,1] texture range.
 * Each face has 2 triangles.
 * ----------------------------------------------------------------------- */

static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

/* Each face: 6 indices (2 triangles), each vertex has a UV */
static const int cube_faces[12][3] = {
    /* Back   */ {0, 2, 1}, {0, 3, 2},
    /* Front  */ {4, 5, 6}, {4, 6, 7},
    /* Left   */ {0, 4, 7}, {0, 7, 3},
    /* Right  */ {1, 2, 6}, {1, 6, 5},
    /* Bottom */ {0, 1, 5}, {0, 5, 4},
    /* Top    */ {3, 7, 6}, {3, 6, 2},
};

/* UVs for each vertex of each triangle.
 * Maps the full texture onto each quad face. */
static const float face_uvs[12][3][2] = {
    /* Back face */
    {{0,0}, {1,1}, {1,0}},
    {{0,0}, {0,1}, {1,1}},
    /* Front face */
    {{0,0}, {1,0}, {1,1}},
    {{0,0}, {1,1}, {0,1}},
    /* Left face */
    {{0,0}, {1,0}, {1,1}},
    {{0,0}, {1,1}, {0,1}},
    /* Right face */
    {{0,0}, {1,1}, {1,0}},
    {{0,0}, {1,1}, {0,0}},
    /* Bottom face */
    {{0,0}, {1,0}, {1,1}},
    {{0,0}, {1,1}, {0,1}},
    /* Top face */
    {{0,0}, {1,0}, {1,1}},
    {{0,0}, {1,1}, {0,1}},
};

/* -----------------------------------------------------------------------
 * Procedural Textures
 *
 * Generates 64×64 ARGB1555 textures. Each face gets a different pattern.
 * ----------------------------------------------------------------------- */

#define TEX_SIZE 64

/* Generate a checkerboard pattern texture */
static void gen_checkerboard(uint16_t *tex, int size,
                              uint16_t col1, uint16_t col2, int cells)
{
    int cell_size = size / cells;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            int cx = x / cell_size;
            int cy = y / cell_size;
            tex[y * size + x] = ((cx + cy) & 1) ? col1 : col2;
        }
    }
}

/* Generate a gradient texture (top-left to bottom-right) */
static void gen_gradient(uint16_t *tex, int size,
                          uint16_t col_top, uint16_t col_bot)
{
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float t = (float)(x + y) / (2.0f * (size - 1));
            /* Simple blend between top and bottom colors */
            int r1 = (col_top >> 10) & 0x1F;
            int g1 = (col_top >> 5) & 0x1F;
            int b1 = col_top & 0x1F;
            int r2 = (col_bot >> 10) & 0x1F;
            int g2 = (col_bot >> 5) & 0x1F;
            int b2 = col_bot & 0x1F;
            int r = (int)(r1 + (r2 - r1) * t);
            int g = (int)(g1 + (g2 - g1) * t);
            int b = (int)(b1 + (b2 - b1) * t);
            tex[y * size + x] = (r << 10) | (g << 5) | b;
        }
    }
}

/* Generate a ring/circle pattern */
static void gen_rings(uint16_t *tex, int size, uint16_t fg, uint16_t bg)
{
    int center = size / 2;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            float dx = x - center;
            float dy = y - center;
            float dist = sqrtf(dx*dx + dy*dy);
            int ring = ((int)(dist / 4.0f)) % 2;
            tex[y * size + x] = ring ? fg : bg;
        }
    }
}

/* ARGB1555 color helper: a=1 (opaque), 5-bit RGB */
#define ARGB1555(r,g,b) (0x8000 | ((r & 0x1F) << 10) | ((g & 0x1F) << 5) | (b & 0x1F))

/* -----------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------- */

static volatile int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

int main(int argc, char **argv)
{
    int width = 640;
    int height = 480;
    int bpp = 2;

    if (argc >= 3) {
        width = atoi(argv[1]);
        height = atoi(argv[2]);
    }
    if (argc >= 4) {
        bpp = atoi(argv[3]) / 8;
    }

    /* Select backend */
#ifdef BACKEND_VIRGE
    const struct l10gl_backend *backend = &virge_backend;
#else
    const struct l10gl_backend *backend = &mga1064_backend;
#endif

    int has_texture = (backend->caps & L10GL_CAP_TEXTURE) != 0;

    printf("L10GL Textured Cube Demo (backend: %s)\n", backend->name);
    printf("Texture mapping: %s\n",
           has_texture ? "hardware" : "not supported, using Gouraud fallback");
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    struct l10gl_ctx ctx;
    if (l10gl_create(&ctx, backend, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }

    /* The backend may adopt the real screen mode instead of the request
     * (native scanout takeover on no-fbdev machines) -- render to what
     * we actually got or the projection/stride math is wrong. */
    if (ctx.width != width || ctx.height != height) {
        printf("Adopted actual screen %dx%d (requested %dx%d)\n",
               ctx.width, ctx.height, width, height);
        width = ctx.width;
        height = ctx.height;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    /* Generate textures (only used if backend supports them) */
    struct l10gl_texture textures[6];
    uint16_t tex_data[6][TEX_SIZE * TEX_SIZE];

    if (has_texture) {
        gen_checkerboard(tex_data[0], TEX_SIZE,
                         ARGB1555(31,31,31), ARGB1555(0,0,0), 8);      /* B&W checker */
        gen_gradient(tex_data[1], TEX_SIZE,
                     ARGB1555(0,31,0), ARGB1555(0,0,31));               /* green→blue */
        gen_checkerboard(tex_data[2], TEX_SIZE,
                         ARGB1555(31,0,0), ARGB1555(0,31,0), 4);       /* red/green */
        gen_rings(tex_data[3], TEX_SIZE,
                  ARGB1555(31,31,0), ARGB1555(0,0,31));                /* yellow/blue rings */
        gen_gradient(tex_data[4], TEX_SIZE,
                     ARGB1555(31,16,0), ARGB1555(16,0,31));             /* orange→purple */
        gen_checkerboard(tex_data[5], TEX_SIZE,
                         ARGB1555(0,31,31), ARGB1555(31,0,31), 16);    /* cyan/magenta fine */

        for (int i = 0; i < 6; i++) {
            if (l10gl_tex_image_2d(&ctx, &textures[i],
                                    TEX_SIZE, TEX_SIZE,
                                    L10GL_TEX_FMT_ARGB1555,
                                    tex_data[i]) < 0) {
                fprintf(stderr, "Failed to upload texture %d\n", i);
                has_texture = 0;
                break;
            }
        }

        if (has_texture) {
            l10gl_tex_parameter(&ctx, L10GL_FILTER_LINEAR, L10GL_WRAP_REPEAT);
            printf("Uploaded %d textures (%dx%d ARGB1555)\n",
                   6, TEX_SIZE, TEX_SIZE);
        }
    }

    /* Set clear values */
    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_depth_func(&ctx, L10GL_LESS);

    float angle = 0.0f;
    int frame = 0;

    printf("Rendering... (Ctrl-C to exit)\n");

    while (running) {
        float rot[3][3];
        build_rotation(rot, angle, angle * 0.7f);

        l10gl_clear(&ctx);

        /* Transform all 8 cube vertices */
        struct screen_vertex projected[8];
        float transformed[8][3];

        for (int i = 0; i < 8; i++) {
            mat3_transform(transformed[i], rot, cube_verts[i]);
            project(&projected[i], transformed[i],
                    width, height, 5.0f);
        }

        /* Draw all 12 triangles */
        for (int face = 0; face < 12; face++) {
            int i0 = cube_faces[face][0];
            int i1 = cube_faces[face][1];
            int i2 = cube_faces[face][2];

            int color_idx = face / 2;

            /* Full-bright white for textured (texel provides color),
             * face color for Gouraud fallback */
            float r, g, b;
            if (has_texture) {
                r = g = b = 1.0f;
            } else {
                /* Use face-based colors for Gouraud fallback */
                float cols[6][3] = {
                    {1,0,0}, {0,1,0}, {0,0,1},
                    {1,1,0}, {1,0,1}, {0,1,1}
                };
                r = cols[color_idx][0];
                g = cols[color_idx][1];
                b = cols[color_idx][2];
            }

            struct l10gl_vertex v0 = {
                .x = projected[i0].sx, .y = projected[i0].sy,
                .z = projected[i0].sz, .w = projected[i0].sw,
                .r = r, .g = g, .b = b, .a = 1.0f,
                .u = face_uvs[face][0][0], .v = face_uvs[face][0][1]
            };
            struct l10gl_vertex v1 = {
                .x = projected[i1].sx, .y = projected[i1].sy,
                .z = projected[i1].sz, .w = projected[i1].sw,
                .r = r, .g = g, .b = b, .a = 1.0f,
                .u = face_uvs[face][1][0], .v = face_uvs[face][1][1]
            };
            struct l10gl_vertex v2 = {
                .x = projected[i2].sx, .y = projected[i2].sy,
                .z = projected[i2].sz, .w = projected[i2].sw,
                .r = r, .g = g, .b = b, .a = 1.0f,
                .u = face_uvs[face][2][0], .v = face_uvs[face][2][1]
            };

            if (has_texture) {
                /* Bind the texture for this face pair */
                if (face % 2 == 0)
                    l10gl_bind_texture(&ctx, &textures[color_idx]);

                l10gl_draw_textured_triangle(&ctx, v0, v1, v2);
            } else {
                l10gl_draw_triangle(&ctx, v0, v1, v2);
            }
        }

        l10gl_wait_engine(&ctx);

        angle += 0.02f;
        if (angle > 2.0f * PI)
            angle -= 2.0f * PI;

        frame++;
        if (frame % 60 == 0)
            printf("Frame %d\n", frame);
    }

    printf("\nExiting after %d frames.\n", frame);
    l10gl_destroy(&ctx);
    return 0;
}
