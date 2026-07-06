/*
 * filltest.c - 2D engine fill readback test for the S3 ViRGE.
 *
 * Symptom-2 diagnostic (HANDOFF "Open symptom 2"): ./triangle leaves the
 * previous scantest pattern on screen with noise at the top, so the 2D
 * clear / Z-clear / 3D draws are not writing the framebuffer where they
 * should. Before touching any engine code, this test separates "engine
 * writes wrong address/stride" from "engine doesn't run": it programs
 * known rectangles at known coords via virge_fill_rect, then CPU-reads
 * VRAM back through ctx->fb (BAR0 linear aperture, stride 1600) and
 * prints pass/fail per corner + center. A photo shows where the rects
 * actually appear on screen.
 *
 * On a correct engine every sampled pixel matches its fill color and
 * nothing is written outside the rectangles. Failure modes this exposes:
 *   - readback = background everywhere -> engine didn't run / wrote nothing.
 *   - color present but at the wrong (x,y) -> wrong DEST_BASE or stride.
 *   - color smeared across rows / wrong pixel count -> stride mismatch.
 *   - a tall rect stops partway down -> the "row ceiling ~299" (PLAN).
 *   - a rect at y>=300 fills nothing -> the ceiling is an absolute row.
 *
 * The rectangles also re-probe the PLAN's two suspected 2D bugs on the
 * now-fixed scanout (they were measured through the old 3200-byte pitch
 * and may have been artifacts): R3 is full-height (row ceiling), R2 starts
 * at y=300 (absolute-row ceiling), R1/R3 are <=100px wide (narrow-width).
 *
 * Run after virge_init has taken over scanout (Mode 9/555, 800x600,
 * stride 1600). Build: make filltest     Run: sudo ./filltest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

/* RGB555 pixel values, matching rgb_to_555() in l10gl_virge.c (5-bit
 * channels: red<<10 | green<<5 | blue). virge_fill_rect's color arg is
 * this 16-bit value in the low bits at 16bpp (2D fill is format-agnostic
 * 555/565 per DB019-B PDF p.232, and scanout is forced to 555). */
#define PX_RED   ((uint16_t)0x7C00)
#define PX_GREEN ((uint16_t)0x03E0)
#define PX_BLUE  ((uint16_t)0x001F)
#define PX_BG    ((uint16_t)0x0000)   /* CPU-cleared background (black) */

static uint16_t read_px(const uint8_t *vram, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(vram + (size_t)y * stride + (size_t)x * 2);
}

static void cpu_clear(uint8_t *vram, uint32_t stride, int w, int h, uint16_t val)
{
    for (int y = 0; y < h; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < w; x++) row[x] = val;
    }
}

struct rect { int x, y, w, h; uint16_t color; const char *name; };

/* Read the four corners + center of a rect; report pass/fail vs the fill
 * color. Returns 1 if all five points match. */
static int check_rect(const uint8_t *vram, uint32_t stride, const struct rect *r)
{
    struct { int x, y; const char *tag; } pts[] = {
        { r->x,            r->y,            "TL" },
        { r->x + r->w - 1, r->y,            "TR" },
        { r->x,            r->y + r->h - 1, "BL" },
        { r->x + r->w - 1, r->y + r->h - 1, "BR" },
        { r->x + r->w / 2, r->y + r->h / 2, "C " },
    };
    int all_pass = 1;
    printf("  %-18s %4dx%-4d @ (%3d,%3d) color=%04x\n",
           r->name, r->w, r->h, r->x, r->y, r->color);
    for (int i = 0; i < 5; i++) {
        uint16_t got = read_px(vram, stride, pts[i].x, pts[i].y);
        int ok = (got == r->color);
        printf("    %s (%4d,%3d): got=%04x %s\n",
               pts[i].tag, pts[i].x, pts[i].y, got,
               ok ? "PASS" : "FAIL (expected above)");
        if (!ok) all_pass = 0;
    }
    return all_pass;
}

/* A rect failed: find where its color actually landed in VRAM (bounding
 * box + pixel count). Reveals wrong address/stride or row ceiling. */
static void locate_color(const uint8_t *vram, uint32_t stride, int w, int h,
                         uint16_t color)
{
    int minx = -1, miny = -1, maxx = -1, maxy = -1, count = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (read_px(vram, stride, x, y) == color) {
                if (minx < 0 || x < minx) minx = x;
                if (maxx < 0 || x > maxx) maxx = x;
                if (miny < 0 || y < miny) miny = y;
                if (maxy < 0 || y > maxy) maxy = y;
                count++;
            }
        }
    }
    if (count == 0)
        printf("    -> color %04x NOT FOUND anywhere in %dx%d "
               "(engine wrote nothing)\n", color, w, h);
    else
        printf("    -> color %04x found: %d px, bbox x=[%d,%d] y=[%d,%d]\n",
               color, count, minx, maxx, miny, maxy);
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

    /* CPU-clear the visible screen to a known black background so any
     * engine write is unambiguous (the fills use non-zero colors). */
    cpu_clear(vram, stride, W, H, PX_BG);

    /* R1: HANDOFF example, also a <=100px width probe.
     * R2: full-width bar starting at y=300 -- above the suspected absolute
     *     row ceiling of ~299, so it tests whether the ceiling is real on
     *     the fixed scanout.
     * R3: tall narrow strip at the bottom-right -- probes narrow width and
     *     low rows. Placed at y=340+ so it does NOT overlap R2 (an earlier
     *     full-height R3 overwrote R2's right edge and read back as a
     *     spurious FAIL; the engine was correct, the test overlapped). */
    struct rect rects[] = {
        {  50,  50, 100, 100, PX_RED,   "R1 red square"   },
        {   0, 300, 800,  40, PX_GREEN, "R2 green bar"    },
        { 736, 340,  64, 260, PX_BLUE,  "R3 blue strip"   },
    };
    int nrects = (int)(sizeof(rects) / sizeof(rects[0]));

    printf("\nFill readback test: %dx%d, stride %u, 16bpp RGB555\n", W, H, stride);
    printf("Programming %d rectangles via virge_fill_rect...\n", nrects);
    for (int i = 0; i < nrects; i++)
        virge_fill_rect(&vctx, rects[i].x, rects[i].y, rects[i].w, rects[i].h,
                        rects[i].color);
    virge_wait_engine(&vctx);

    printf("\nReadback (expected fill color at corners + center):\n");
    int total_pass = 1;
    for (int i = 0; i < nrects; i++) {
        int ok = check_rect(vram, stride, &rects[i]);
        if (!ok) {
            total_pass = 0;
            locate_color(vram, stride, W, H, rects[i].color);
        }
    }

    printf("\nRESULT: %s\n", total_pass
           ? "ALL RECTS LANDED CORRECTLY -- 2D fill is sound; symptom 2 is"
             " in the Z-clear / 3D path or demo usage, not 2D fill."
           : "MISMATCH -- 2D fills are NOT landing as expected; see the"
             " failures + bbox lines above.");

    /* Leave the rects on screen for a photo, then Ctrl-C restores scanout. */
    printf("\nRects are on screen (black bg): red 100x100 @ (50,50),\n"
           "green full-width bar @ y=300..339, blue 64-wide strip @\n"
           "x=736, y=340..599. Photograph, then Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;          /* Ctrl-C or closed stdin */
    }

    virge_cleanup(&vctx);
    return total_pass ? 0 : 2;
}
