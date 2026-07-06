/*
 * tritest.c - 3D triangle readback test for the S3 ViRGE.
 *
 * 2D fill is verified sound (filltest reaches y=599), but 3D rendering
 * (cube, triangle) cuts off partway down the screen -- a 3D-engine-
 * specific row limit. This draws a single full-height Gouraud triangle
 * with Z-buffering DISABLED (to isolate the rasterizer from any Z-test
 * rejection), then CPU-reads VRAM back through ctx->fb and reports the
 * bounding box, the max Y actually reached, and a vertical sample down
 * the bottom-vertex column.
 *
 * Interpretation:
 *   maxy ~580  -> rasterizer spans full height; the cutoff in cube/triangle
 *                 is a Z-clear / Z-test issue, not rasterization.
 *   maxy ~240  -> 3D rasterizer itself stops at a fixed row (stride or
 *                 addressing limit); the cutoff is geometry-engine side.
 *   maxy ~300 with 2:1 vertical compression -> wrong 3D dest stride.
 *
 * Build: make tritest     Run: sudo ./tritest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static uint16_t read_px(const uint8_t *vram, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(vram + (size_t)y * stride + (size_t)x * 2);
}

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) {
        fprintf(stderr, "virge_init failed\n");
        return 1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sighandler;   /* no SA_RESTART: interrupt getchar() */
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;

    /* CPU-clear framebuffer black; clear Z to far (defensive -- Z is off
     * below, but a cleared Z can't reject pixels either way). */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }
    virge_clear_z(&vctx, 1.0f);

    /* Z off: the triangle cannot be rejected by Z-test, so any missing
     * pixels are a rasterizer/addressing limit, not a depth rejection. */
    vctx.z_cmd_bits = VIRGE_ZB_NONE;

    /* Full-height triangle: blue near top-left, green mid-right, red
     * bottom-center -- spans y=20..580 so maxy directly shows how far
     * down the rasterizer reaches. */
    struct virge_vertex v_blue  = { .x=120, .y=20,  .z=0.5f, .w=1,
                                    .r=0, .g=0, .b=1, .a=1 };
    struct virge_vertex v_green = { .x=700, .y=300, .z=0.5f, .w=1,
                                    .r=0, .g=1, .b=0, .a=1 };
    struct virge_vertex v_red   = { .x=400, .y=580, .z=0.5f, .w=1,
                                    .r=1, .g=0, .b=0, .a=1 };

    printf("\n3D triangle readback: %dx%d stride %u (Z OFF)\n", W, H, stride);
    printf("Drawing full-height Gouraud triangle "
           "(blue y=20, green y=300, red y=580)...\n");
    virge_draw_triangle_gouraud(&vctx, v_red, v_green, v_blue);
    virge_wait_engine(&vctx);

    int minx = -1, miny = -1, maxx = -1, maxy = -1, count = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (read_px(vram, stride, x, y)) {
                if (minx < 0 || x < minx) minx = x;
                if (maxx < 0 || x > maxx) maxx = x;
                if (miny < 0 || y < miny) miny = y;
                if (maxy < 0 || y > maxy) maxy = y;
                count++;
            }
        }
    }

    printf("Rendered: %d px, bbox x=[%d,%d] y=[%d,%d]\n",
           count, minx, maxx, miny, maxy);
    if (maxy < 0)
        printf("  -> NOTHING RENDERED (rasterizer drew no pixels)\n");
    else if (maxy > 500)
        printf("  -> maxy=%d: FULL HEIGHT -- rasterizer is OK; "
               "cube/triangle cutoff is a Z-clear / Z-test issue.\n", maxy);
    else
        printf("  -> maxy=%d: CUT OFF -- 3D rasterizer stops at a fixed row "
               "(geometry-engine stride/addressing limit).\n", maxy);

    /* Vertical run down the red-vertex column (x=400): the contiguous row
     * ranges that actually have pixels show whether it cuts hard or fades. */
    printf("Vertical run at x=400, row ranges with pixels:\n  ");
    int seg0 = -1, prev = -1;
    for (int y = 0; y < H; y++) {
        if (read_px(vram, stride, 400, y)) {
            if (seg0 < 0) seg0 = y;
            prev = y;
        } else if (seg0 >= 0) {
            printf("[%d,%d] ", seg0, prev);
            seg0 = -1;
        }
    }
    if (seg0 >= 0) printf("[%d,%d] ", seg0, prev);
    printf("\n");

    printf("\nTriangle is on screen (blue TL, green right, red bottom). "
           "Photograph, then Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}
