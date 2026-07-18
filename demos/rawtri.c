/*
 * rawtri.c - Static raw screen-space Gouraud triangle using L10GL.
 *
 * One non-animated triangle with red, green, and blue vertices. Its
 * purpose is to verify the 3D triangle edge/attribute setup (V9/V7) on
 * real hardware: the cube demo can't reveal those bugs because its faces
 * are flat-colored (all color deltas zero). This draws a single triangle
 * with distinct per-vertex colors and a deliberately scalene, asymmetric
 * shape that makes a misprogrammed span-end visible -- the old V9 bug
 * ("bottom span jumps straight to the middle-vertex X") fattened the
 * corner at the bottom vertex.
 *
 * Expected on correct hardware: a clean scalene triangle, straight
 * edges, all three corners tapering to a point, with a smooth
 * red -> green -> blue Gouraud gradient and vivid (full-brightness)
 * colors. A fat/sheared corner, a sheared gradient, or black output
 * indicates a regression in the edge setup (V9) or color scale (V10).
 *
 * Build: make rawtri
 * Run:   sudo ./rawtri
 */

#include <stdio.h>
#include <signal.h>
#include <unistd.h>

#include "l10gl.h"

static volatile int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

int main(void)
{
    int width = 640;
    int height = 480;
    int bpp = 2;  /* 16bpp */

    printf("L10GL Static Gouraud Triangle\n");
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    struct l10gl_ctx ctx;
    if (l10gl_create_auto(&ctx, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }
    printf("Selected backend: %s\n", ctx.backend->name);

    /* The backend may adopt the real screen mode instead of the request
     * (native scanout takeover on no-fbdev machines) -- place the
     * triangle in what we actually got. */
    if (ctx.width != width || ctx.height != height) {
        printf("Adopted actual screen %dx%d (requested %dx%d)\n",
               ctx.width, ctx.height, width, height);
        width = ctx.width;
        height = ctx.height;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);   /* black background */
    l10gl_clear_depth(&ctx, 1.0f);                /* far Z */
    l10gl_depth_func(&ctx, L10GL_LESS);

    /*
     * One scalene triangle, one primary color per vertex, placed as
     * fractions of the actual screen (at 640x480 these reproduce the
     * original hardcoded vertices):
     *   red   at the bottom-center (0.50w, 0.917h),
     *   green at the middle-right  (0.81w, 0.500h),
     *   blue  at the top-left      (0.19w, 0.125h).
     * The horizontal offset between the bottom and middle vertices is
     * what makes the old span-end bug visible: with TXEND01
     * misprogrammed to the middle-vertex X, the bottom span ran all the
     * way to the middle-vertex X on the very bottom scanline, fattening
     * the lower-right corner. With V9 it tapers cleanly to the red
     * vertex. Vertices may be passed in any order; the backend sorts
     * by Y.
     */
    struct l10gl_vertex v_red = {
        .x = 0.50f * width, .y = 0.917f * height, .z = 0.5f, .w = 1.0f,
        .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f
    };
    struct l10gl_vertex v_green = {
        .x = 0.81f * width, .y = 0.500f * height, .z = 0.5f, .w = 1.0f,
        .r = 0.0f, .g = 1.0f, .b = 0.0f, .a = 1.0f
    };
    struct l10gl_vertex v_blue = {
        .x = 0.19f * width, .y = 0.125f * height, .z = 0.5f, .w = 1.0f,
        .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f
    };

    printf("Vertices: red(%.0f,%.0f) green(%.0f,%.0f) blue(%.0f,%.0f)\n",
           v_red.x, v_red.y, v_green.x, v_green.y, v_blue.x, v_blue.y);

    /* The framebuffer is single-buffered, so one draw persists on screen;
     * draw once and idle so the image is rock-solid for photographing. */
    l10gl_clear(&ctx);
    l10gl_draw_triangle(&ctx, v_red, v_green, v_blue);
    l10gl_wait_engine(&ctx);

    printf("Triangle rendered. Ctrl-C to exit.\n");
    while (running)
        usleep(100000);

    l10gl_destroy(&ctx);
    return 0;
}
