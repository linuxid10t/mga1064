/*
 * textured_cube.c - Spinning textured cube demo using L10GL.
 *
 * Hardware-agnostic — uses the L10GL frontend API. The backend is selected
 * at runtime. If the backend supports texture mapping, it uses textured
 * triangles. If not (currently MGA-1064SG), it silently
 * falls back to Gouraud shading.
 *
 * Build: make DEMO=textured_cube
 * Run:   sudo ./textured_cube [width height bpp]
 *
 * Controls: Ctrl-C to exit.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>

#include "l10gl.h"

#define CAMERA_DIST 5.0f
#define FOV_Y_DEGREES 53.130102f
#define ANGLE_STEP_DEGREES 1.14591559f

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

static const float face_colors[6][3] = {
    {1, 0, 0}, {0, 1, 0}, {0, 0, 1},
    {1, 1, 0}, {1, 0, 1}, {0, 1, 1},
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

static int frame_limit_from_env(void)
{
    const char *value = getenv("L10GL_FRAMES");
    char *end;
    long limit;

    if (!value || !value[0])
        return 0;
    limit = strtol(value, &end, 10);
    if (*end || limit <= 0 || limit > INT_MAX) {
        fprintf(stderr, "Ignoring invalid L10GL_FRAMES='%s'\n", value);
        return 0;
    }
    return (int)limit;
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

    printf("L10GL Textured Cube Demo\n");
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    struct l10gl_ctx ctx;
    if (l10gl_create_auto(&ctx, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }

    int has_texture = (ctx.backend->caps & L10GL_CAP_TEXTURE) != 0;
    printf("Selected backend: %s\n", ctx.backend->name);
    printf("Texture mapping: %s\n",
           has_texture ? "supported by backend"
                       : "not supported, using Gouraud fallback");

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
            /* LINEAR (bilinear): silicon-verified 2026-07-09 (texprobe TEST 18 --
             * a 1-texel R-stripe sampled at a texel boundary blends to R15 in
             * both U and V). Smoother textures than NEAREST. The filter is
             * path-independent (operates on the final texel address), so it is
             * valid under the cube's PERSPECTIVE path. */
            l10gl_tex_parameter(&ctx, L10GL_FILTER_LINEAR, L10GL_WRAP_REPEAT);
            printf("Uploaded %d textures (%dx%d ARGB1555)\n",
                   6, TEX_SIZE, TEX_SIZE);
        }
    }

    /* Set clear values */
    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_depth_func(&ctx, L10GL_LESS);
    l10gl_cull_face(&ctx, L10GL_CULL_BACK);

    l10gl_matrix_mode(&ctx, L10GL_MATRIX_PROJECTION);
    l10gl_load_identity(&ctx);
    if (l10gl_perspective(&ctx, FOV_Y_DEGREES,
                          (float)width / (float)height, 3.0f, 7.0f) < 0) {
        fprintf(stderr, "Failed to configure projection.\n");
        l10gl_destroy(&ctx);
        return 1;
    }

    float angle = 0.0f;
    int frame = 0;
    int frame_limit = frame_limit_from_env();

    printf("Rendering... (Ctrl-C to exit)");
    if (frame_limit)
        printf("  [L10GL_FRAMES=%d]", frame_limit);
    putchar('\n');

    while (running) {
        l10gl_clear(&ctx);

        l10gl_matrix_mode(&ctx, L10GL_MATRIX_MODELVIEW);
        l10gl_load_identity(&ctx);
        l10gl_translatef(&ctx, 0, 0, -CAMERA_DIST);
        l10gl_scalef(&ctx, 1, 1, -1);
        l10gl_rotatef(&ctx, angle, 1, 0, 0);
        l10gl_rotatef(&ctx, angle * 0.7f, 0, 1, 0);

        for (int side = 0; side < 6; side++) {
            if (has_texture) {
                l10gl_bind_texture(&ctx, &textures[side]);
                l10gl_color4f(&ctx, 1, 1, 1, 1);
            } else {
                l10gl_bind_texture(&ctx, NULL);
                l10gl_color4f(&ctx, face_colors[side][0],
                              face_colors[side][1],
                              face_colors[side][2], 1);
            }

            l10gl_begin(&ctx, L10GL_TRIANGLES);
            for (int triangle = 0; triangle < 2; triangle++) {
                int face = side * 2 + triangle;
                const int order[3] = {0, 2, 1};

                for (int corner = 0; corner < 3; corner++) {
                    int source = order[corner];
                    const float *vertex = cube_verts[cube_faces[face][source]];

                    l10gl_texcoord2f(&ctx, face_uvs[face][source][0],
                                     face_uvs[face][source][1]);
                    l10gl_vertex3f(&ctx, vertex[0], vertex[1], vertex[2]);
                }
            }
            l10gl_end(&ctx);
        }

        l10gl_wait_engine(&ctx);
        l10gl_swap_buffers(&ctx);   /* tear-free: publish frame at vblank, flip render target */

        frame++;
        if (frame_limit && frame >= frame_limit) {
            printf("Frame limit reached.\n");
            break;
        }

        angle += ANGLE_STEP_DEGREES;
        if (angle > 360.0f)
            angle -= 360.0f;

        if (frame % 60 == 0)
            printf("Frame %d\n", frame);
    }

    printf("\nExiting after %d frames.\n", frame);
    l10gl_destroy(&ctx);
    return 0;
}
