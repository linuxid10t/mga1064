/*
 * seamtest.c - measure the S3d triangle span fill rule on silicon.
 *
 * The cube's persistent shared-edge bleedthrough is a COVERAGE defect, not
 * Z-fighting. This probe measures the span-start and span-end fill rules
 * directly and checks whether two triangles sharing an edge actually
 * double-draw the boundary column.
 *
 * Three tests, one run:
 *   (END)  edge 01 vertical at X -> end-edge rule (L/R=1 right, L/R=0 left).
 *   (START) edge 02 vertical at X -> start-edge rule (L/R=1 left, L/R=0 right).
 *   (OVERLAP) two triangles sharing a vertical edge at x=N, drawn in both
 *             orders; if column N's owner flips with draw order, both
 *             triangles claim it (double-draw = the bleedthrough cause).
 *
 * First-run result (2026-07-07): END edge is direction-asymmetric --
 * L/R=1 exclusive (last filled = ceil(X)-1), L/R=0 inclusive (first filled =
 * ceil(X)) -- NOT uniformly inclusive. So the double-draw suspect is the START
 * edge (edge 02), measured below, plus the direct overlap check.
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

static void clear_fb(uint8_t *vram, int W, int H, uint32_t stride)
{
    for (int y = 0; y < H; y++) {
        uint16_t *r = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) r[x] = 0;
    }
}

/* Filled column range over bottom-half rows y=[340,460]. */
static void filled_range(uint8_t *vram, uint32_t stride, int W, int *minx, int *maxx)
{
    *minx = W; *maxx = -1;
    for (int y = 340; y <= 460; y += 2) {
        for (int x = 0; x < W; x++) {
            if (pix(vram, stride, x, y) != 0) {
                if (x < *minx) *minx = x;
                if (x > *maxx) *maxx = x;
            }
        }
    }
    if (*minx == W) *minx = -1;
}

/* END edge test: edge 01 vertical at X (v0,v1 at X; v2 on the far side). */
static void end_test(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                     float X, int lr, int *minx, int *maxx)
{
    int W = vctx->width, H = vctx->height;
    clear_fb(vram, W, H, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    float farx = lr ? 100.0f : 700.0f;
    struct virge_vertex v0 = { .x = X,    .y = 500, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v1 = { .x = X,    .y = 300, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v2 = { .x = farx, .y = 100, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(vctx, v0, v1, v2);
    virge_wait_engine(vctx);
    filled_range(vram, stride, W, minx, maxx);
}

/* START edge test: edge 02 vertical at X (v0,v2 at X; v1 on the far side). */
static void start_test(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                       float X, int lr, int *minx, int *maxx)
{
    int W = vctx->width, H = vctx->height;
    clear_fb(vram, W, H, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    float midx = lr ? (X + 200.0f) : (X - 200.0f); /* v1 right of X for L/R=1, left for L/R=0 */
    struct virge_vertex v0 = { .x = X,   .y = 500, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v1 = { .x = midx,.y = 300, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v2 = { .x = X,   .y = 100, .z = 0.5f, .w = 1, .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(vctx, v0, v1, v2);
    virge_wait_engine(vctx);
    filled_range(vram, stride, W, minx, maxx);
}

/* Classify a 555 pixel: "A(red)" / "B(grn)" / " . "(empty). */
static const char *classify(uint16_t v)
{
    if (v == 0) return ".";
    int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F;
    if (r > g) return "A";
    if (g > r) return "B";
    return "?";
}

/* OVERLAP test: two triangles sharing vertical edge at x=N (both long edges),
 * drawn in both orders. If column N's owner flips with order -> double-draw. */
static void overlap_test(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                         int N, int ab_first)
{
    int W = vctx->width, H = vctx->height;
    clear_fb(vram, W, H, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    /* A = left, red; B = right, green. Shared vertical edge at x=N (edge 02). */
    struct virge_vertex A0 = { .x = N,     .y = 500, .z = 0.5f, .w = 1, .r = 1, .g = 0, .b = 0, .a = 1 };
    struct virge_vertex A1 = { .x = N-200, .y = 300, .z = 0.5f, .w = 1, .r = 1, .g = 0, .b = 0, .a = 1 };
    struct virge_vertex A2 = { .x = N,     .y = 100, .z = 0.5f, .w = 1, .r = 1, .g = 0, .b = 0, .a = 1 };
    struct virge_vertex B0 = { .x = N,     .y = 500, .z = 0.5f, .w = 1, .r = 0, .g = 1, .b = 0, .a = 1 };
    struct virge_vertex B1 = { .x = N+200, .y = 300, .z = 0.5f, .w = 1, .r = 0, .g = 1, .b = 0, .a = 1 };
    struct virge_vertex B2 = { .x = N,     .y = 100, .z = 0.5f, .w = 1, .r = 0, .g = 1, .b = 0, .a = 1 };

    if (ab_first) {
        virge_draw_triangle_gouraud(vctx, A0, A1, A2);
        virge_draw_triangle_gouraud(vctx, B0, B1, B2);
    } else {
        virge_draw_triangle_gouraud(vctx, B0, B1, B2);
        virge_draw_triangle_gouraud(vctx, A0, A1, A2);
    }
    virge_wait_engine(vctx);

    printf("    cols N-2..N+2: ");
    for (int y = 300; y <= 300; y++)               /* one mid row is enough */
        for (int x = N - 2; x <= N + 2; x++)
            printf("%s ", classify(pix(vram, stride, x, y)));
    printf(" (%s first)\n", ab_first ? "A" : "B");
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
    float Xs[] = { 300.0f, 300.5f, 301.0f, 301.5f };

    printf("\nseamtest: S3d span fill rule (%dx%d stride %u)\n\n",
           vctx.width, vctx.height, stride);

    printf("=== END edge (edge 01 vertical at X) ===\n");
    for (int lr = 1; lr >= 0; lr--) {
        printf("L/R=%d (%s):\n  %-7s %-6s %-6s %-10s\n", lr,
               lr ? "right/end" : "left/end", "X", "min", "max", "endExtent");
        for (size_t i = 0; i < sizeof(Xs)/sizeof(Xs[0]); i++) {
            int mn, mx; end_test(&vctx, vram, stride, Xs[i], lr, &mn, &mx);
            int ext = lr ? mx : mn;
            printf("  %-7.1f %-6d %-6d %-10d\n", Xs[i], mn, mx, ext);
        }
        printf("\n");
    }

    printf("=== START edge (edge 02 vertical at X) ===\n");
    for (int lr = 1; lr >= 0; lr--) {
        printf("L/R=%d (%s):\n  %-7s %-6s %-6s %-10s\n", lr,
               lr ? "left/start" : "right/start", "X", "min", "max", "startExtent");
        for (size_t i = 0; i < sizeof(Xs)/sizeof(Xs[0]); i++) {
            int mn, mx; start_test(&vctx, vram, stride, Xs[i], lr, &mn, &mx);
            int ext = lr ? mn : mx;
            printf("  %-7.1f %-6d %-6d %-10d\n", Xs[i], mn, mx, ext);
        }
        printf("\n");
    }

    printf("=== OVERLAP: two triangles sharing edge at N=350 ===\n");
    printf("  A=red(left) B=green(right); cols shown left->right as N-2 N-1 N N+1 N+2.\n");
    printf("  If col N flips A<->B between the two orders, BOTH claim it (double-draw).\n");
    overlap_test(&vctx, vram, stride, 350, 1);
    overlap_test(&vctx, vram, stride, 350, 0);

    printf("\nProbe complete. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
