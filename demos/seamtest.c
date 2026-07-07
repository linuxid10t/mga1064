/*
 * seamtest.c - measure the S3d triangle span-END fill rule on silicon.
 *
 * Diagnosis (Fable 5, 2026-07-07, verified against this repo): the cube's
 * persistent shared-edge bleedthrough is NOT Z-fighting -- it is a coverage
 * defect in triangle setup. The S3d span END is INCLUSIVE on real DX silicon
 * (datasheet: TXEND01/12 = "X of the last pixel drawn"), one pixel wider than
 * 86Box's end-exclusive model. Two triangles sharing a silhouette edge walk
 * bit-identical DDAs, so under the exclusive model they would be watertight;
 * the visible 1px band on real hardware is the evidence that DX draws the end
 * edge inclusive -> both triangles claim the boundary column, and under
 * LEQUAL+back-to-front the nearer face wins it, reading as a band on the
 * farther face. The fix lives in setup (bias the end edge), but the exact
 * bias needs the exact rule, so measure first.
 *
 * Method: draw a flat triangle whose END edge (edge 01) is VERTICAL at a
 * controlled X (v0 and v1 both at x=X), v2 on the far side. For L/R=1
 * (v2 left of X) edge 01 is the RIGHT/end edge; for L/R=0 (v2 right of X)
 * it is the LEFT/end edge. CPU-read the framebuffer over the bottom-half
 * rows and report the min/max filled column. Integer vs half-integer X
 * reveals inclusive vs exclusive and the rounding (ceil/floor), for both
 * directions -- exactly what the span-end bias needs.
 *
 * Build: make -B BACKEND=virge seamtest     Run: sudo ./seamtest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

static uint16_t pix(const uint8_t *fb, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(fb + (size_t)y * stride + (size_t)x * 2);
}

/* Draw a flat white triangle whose edge-01 is VERTICAL at x=X (bottom half).
 *   lr=1: v2 LEFT of X  -> L/R=1, X is the RIGHT (end) edge.
 *   lr=0: v2 RIGHT of X -> L/R=0, X is the LEFT  (end) edge.
 * Returns the filled column range over bottom-half rows y=[340,460]. */
static void edge_test(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                      float X, int lr, int *minx, int *maxx)
{
    int W = vctx->width, H = vctx->height;
    for (int y = 0; y < H; y++) {
        uint16_t *r = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) r[x] = 0;
    }
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    float farx = lr ? 100.0f : 700.0f;
    struct virge_vertex v0 = { .x = X,    .y = 500, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v1 = { .x = X,    .y = 300, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v2 = { .x = farx, .y = 100, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(vctx, v0, v1, v2);
    virge_wait_engine(vctx);

    *minx = W; *maxx = -1;
    for (int y = 340; y <= 460; y += 2) {   /* bottom half, away from the y_mid/y_bot junctions */
        for (int x = 0; x < W; x++) {
            if (pix(vram, stride, x, y) != 0) {
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
            }
        }
    }
    if (*minx == W) *minx = -1;             /* nothing filled */
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
    sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    uint32_t stride = vctx.stride;

    printf("\nseamtest: S3d span fill rule (%dx%d stride %u)\n",
           vctx.width, vctx.height, stride);
    printf("Triangle with vertical edge-01 at x=X; filled cols over y=[340,460].\n");
    printf("'end extent' = the END-edge side (max for L/R=1 right, min for L/R=0 left).\n\n");

    float Xs[] = { 300.0f, 300.5f, 301.0f, 301.5f };
    for (int lr = 1; lr >= 0; lr--) {
        printf("L/R=%d (%s edge at X):\n", lr, lr ? "RIGHT / end" : "LEFT / end");
        printf("  %-8s %-10s %-10s %-10s\n", "X", "min", "max", "endExtent");
        for (size_t i = 0; i < sizeof(Xs) / sizeof(Xs[0]); i++) {
            int minx, maxx;
            edge_test(&vctx, vram, stride, Xs[i], lr, &minx, &maxx);
            int ext = lr ? maxx : minx;
            printf("  %-8.1f %-10d %-10d %-10d\n", Xs[i], minx, maxx, ext);
        }
        printf("\n");
    }

    printf("How to read it:\n");
    printf("  END edge at INTEGER X: endExtent == X  -> INCLUSIVE (draws through X;\n");
    printf("                                    shared edges double-draw 1px -> the band).\n");
    printf("                          endExtent == X-1 -> EXCLUSIVE (watertight).\n");
    printf("  HALF-INTEGER X (e.g. 300.5): endExtent 300 vs 301 reveals ceil vs floor.\n");
    printf("  L/R=0 (left end) may differ from L/R=1 -- measure both before biasing.\n");
    printf("Fix once the rule is known: bias the END edge toward the triangle interior\n");
    printf("(TXEND01/12 down 1px for L/R=1, up 1px for L/R=0) so the mesh is watertight.\n");

    printf("\nProbe complete. Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;
    }
    virge_cleanup(&vctx);
    return 0;
}
