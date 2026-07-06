/*
 * triangle.c - Static Gouraud-shaded triangle demo using L10GL.
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
 * Build: make triangle            (or: make BACKEND=virge triangle)
 * Run:   sudo ./triangle
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

#ifdef BACKEND_VIRGE
    const struct l10gl_backend *backend = &virge_backend;
#else
    const struct l10gl_backend *backend = &mga1064_backend;
#endif

    printf("L10GL Static Gouraud Triangle (backend: %s)\n", backend->name);
    printf("Initializing %dx%d @ %dbpp...\n", width, height, bpp * 8);

    struct l10gl_ctx ctx;
    if (l10gl_create(&ctx, backend, width, height, bpp) < 0) {
        fprintf(stderr, "Failed to initialize L10GL.\n");
        return 1;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);   /* black background */
    l10gl_clear_depth(&ctx, 1.0f);                /* far Z */
    l10gl_depth_func(&ctx, L10GL_LESS);

    /*
     * One scalene triangle, one primary color per vertex:
     *   red   at the bottom-center (320, 440),
     *   green at the middle-right  (520, 240),
     *   blue  at the top-left      (120,  60).
     * The 200px horizontal offset between the bottom (x=320) and middle
     * (x=520) vertices is what makes the old span-end bug visible: with
     * TXEND01 misprogrammed to the middle-vertex X, the bottom span ran
     * 320->520 at the very bottom scanline, fattening the lower-right
     * corner. With V9 it tapers cleanly to the red vertex. Vertices may
     * be passed in any order; the backend sorts by Y.
     */
    struct l10gl_vertex v_red = {
        .x = 320.0f, .y = 440.0f, .z = 0.5f, .w = 1.0f,
        .r = 1.0f, .g = 0.0f, .b = 0.0f, .a = 1.0f
    };
    struct l10gl_vertex v_green = {
        .x = 520.0f, .y = 240.0f, .z = 0.5f, .w = 1.0f,
        .r = 0.0f, .g = 1.0f, .b = 0.0f, .a = 1.0f
    };
    struct l10gl_vertex v_blue = {
        .x = 120.0f, .y = 60.0f, .z = 0.5f, .w = 1.0f,
        .r = 0.0f, .g = 0.0f, .b = 1.0f, .a = 1.0f
    };

    printf("Vertices: red(320,440) green(520,240) blue(120,60)\n");

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
