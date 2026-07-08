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

/* EDGE-12 (top-half END edge) per-row profile -- the probe seamtest was missing.
 *
 * seamtest's END-edge test scans y=[340,460] with y_mid=300, so it measured
 * only edge 01 (the BOTTOM-half END edge). The RELOADED edge 12 (top-half END,
 * the TXEND12 path) was never measured -- and that is where cubefb's 310-deg
 * blue-face seam crack lives (tri {0,4,7}, lr=0, gap [485,487] at y=365).
 *
 * This probe renders one ISOLATED flat triangle against background and, for
 * every top-half scanline, prints the analytic edge-12 X, the MODELED extent
 * (virge.c's intended walk: TXEND12 + (y_mid-y)*dXdY12, seamtest ceil fill
 * rule), and the MEASUREDED extent (silicon's actual filled pixel on the
 * edge-12 side). The bias column = measured - modeled isolates the hardware
 * walk deviation from the prestep drift (drift = modeled - true), and its
 * per-row profile discriminates CONSTANT vs FRONT-LOADED (first N lines after
 * reload) vs TAPERING -- the one data point (row 365) cubefb cannot resolve.
 *
 * Isolated triangle => overspill is visible against background, so for lr=1
 * "masked-by-overspill" vs "absent" are directly distinguishable (a constant
 * +1 dXdY12 step predicts bias>0 for BOTH lr: gap for lr=0, overspill for
 * lr=1). Binary-fraction vertices (.0/.25/.5/.75) make the analytic X exact,
 * killing the ceil-boundary ambiguity that blurred the step count in the
 * cubefb reconstruction.
 *
 * Config matrix: lr{0,1} x slope12-sign{+,-} x v1.y-frac{0.25,0.75}, plus
 * scan_01=0 (flat-bottom, immediate reload -- cube corners hit this).
 */
static int32_t e12_xfixed(float x) { return (int32_t)(x * 1048576.0f); }   /* S11.20 */
static int32_t e12_dxdy(int32_t fxa, int ya, int32_t fxb, int yb)
{
    int dy = yb - ya; if (dy == 0) dy = 1;
    return -(int32_t)(((int64_t)(fxb - fxa)) / dy);
}

static void edge12_profile(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                           const char *label,
                           float x0, float y0, float x1, float y1,
                           float x2, float y2)
{
    int W = vctx->width, H = vctx->height;
    clear_fb(vram, W, H, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);

    struct virge_vertex v0 = { .x=x0, .y=y0, .z=0.5f, .w=1, .r=1, .g=1, .b=1, .a=1 };
    struct virge_vertex v1 = { .x=x1, .y=y1, .z=0.5f, .w=1, .r=1, .g=1, .b=1, .a=1 };
    struct virge_vertex v2 = { .x=x2, .y=y2, .z=0.5f, .w=1, .r=1, .g=1, .b=1, .a=1 };
    virge_draw_triangle_gouraud(vctx, v0, v1, v2);
    virge_wait_engine(vctx);

    /* sort y desc to label rows the way virge.c does (v0=bottom, v1=mid, v2=top) */
    float sy0=y0, sy1=y1, sy2=y2, sx0=x0, sx1=x1, sx2=x2;
    if (sy0 < sy1) { float t=sy0; sy0=sy1; sy1=t; t=sx0; sx0=sx1; sx1=t; }
    if (sy1 < sy2) { float t=sy1; sy1=sy2; sy2=t; t=sx1; sx1=sx2; sx2=t; }
    if (sy0 < sy1) { float t=sy0; sy0=sy1; sy1=t; t=sx0; sx0=sx1; sx1=t; }
    int y_bot=(int)sy0, y_mid=(int)sy1, y_top=(int)sy2;
    float cross=(sx2-sx0)*(sy1-sy0)-(sx1-sx0)*(sy2-sy0);
    int lr = (cross > 0) ? 1 : 0;
    int scan_01 = y_bot - y_mid, scan_12 = y_mid - y_top;

    int32_t fx1 = e12_xfixed(sx1), fx2 = e12_xfixed(sx2);
    int32_t dY12 = e12_dxdy(fx1, y_mid, fx2, y_top);          /* S11.20 px per row-up */
    float yf1 = sy1 - (float)y_mid;
    int64_t x_end12 = (int64_t)fx1 + (int64_t)(dY12 * yf1);   /* S11.20, edge12 @ y_mid */
    float dY12_px = (float)dY12 / 1048576.0f;

    printf("\n%s: lr=%d  dXdY12=%+.4f px/row  scan_01=%d scan_12=%d  "
           "y_mid=%.2f (yf1=%.2f) y_top=%.2f  (1 step = %+.3f px)\n",
           label, lr, dY12_px, scan_01, scan_12, sy1, yf1, sy2, dY12_px);
    printf("  %-4s %-9s %-9s %-9s %-8s %-8s\n",
           "row", "true", "model", "meas", "drift", "bias");
    printf("  %-4s %-9s %-9s %-9s %-8s %-8s\n",
           "", "", "(intended)", "(silicon)", "m-t", "meas-mod");

    for (int y = y_mid; y > y_top; y--) {
        /* true edge-12 X at row y (interp v1->v2) */
        float t = (sy1 != sy2) ? (sy1 - y) / (sy1 - sy2) : 0.0f;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        float true_x = sx1 + (sx2 - sx1) * t;
        /* modeled walked edge-12 X: TXEND12 + (y_mid - y)*dY12, S11.20 */
        int64_t mod_fixed = x_end12 + (int64_t)(y_mid - y) * (int64_t)dY12;
        float mod_x = (double)mod_fixed / 1048576.0;
        /* modeled fill extent (seamtest END rule): lr=0 first=ceil(X), lr=1 last=ceil(X)-1 */
        int64_t ce = (mod_fixed + 0xFFFFF) >> 20;   /* ceil, mod_fixed is positive here */
        int mod_ext = lr ? (int)ce - 1 : (int)ce;
        /* measured filled extent on the edge-12 side */
        const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
        int meas_ext = -1;
        if (lr == 0) { for (int x=0; x<W; x++) if (row[x]) { meas_ext=x; break; } }   /* min x */
        else         { for (int x=W-1; x>=0; x--) if (row[x]) { meas_ext=x; break; } } /* max x */
        float drift = mod_x - true_x;
        int bias = (meas_ext >= 0) ? meas_ext - mod_ext : 0;
        printf("  %-4d %-9.3f %-9.3f %-9d %-8.3f %-8d\n",
               y, true_x, mod_x, meas_ext, drift, bias);
    }
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

    printf("\n=== EDGE-12 (top-half END edge) per-row profile ===\n");
    printf("  bias = measured - modeled (silicon walk vs virge.c intent).\n");
    printf("  +1-step law predicts bias ~= +1 step (gap for lr=0, overspill for lr=1),\n");
    printf("  CONSTANT across all top-half rows. Front-loaded/tapering = bias only near y_mid.\n");
    /* lr{0,1} x slope12{+,-} x v1.y-frac{0.25,0.75}; scan_01=100, scan_12=48.
     * slope mag 4 px/row (dx=192 over 48 rows); binary-fraction verts. */
    edge12_profile(&vctx, vram, stride, "lr0 sl+ yf.25", 750,500,  60,400.25, 252,352);
    edge12_profile(&vctx, vram, stride, "lr0 sl+ yf.75", 750,500,  60,400.75, 252,352);
    edge12_profile(&vctx, vram, stride, "lr0 sl- yf.25", 750,500, 240,400.25,  48,352);
    edge12_profile(&vctx, vram, stride, "lr0 sl- yf.75", 750,500, 240,400.75,  48,352);
    edge12_profile(&vctx, vram, stride, "lr1 sl+ yf.25",  40,500, 560,400.25, 752,352);
    edge12_profile(&vctx, vram, stride, "lr1 sl+ yf.75",  40,500, 560,400.75, 752,352);
    edge12_profile(&vctx, vram, stride, "lr1 sl- yf.25",  40,500, 730,400.25, 538,352);
    edge12_profile(&vctx, vram, stride, "lr1 sl- yf.75",  40,500, 730,400.75, 538,352);
    /* scan_01=0: flat-bottom (y_bot==y_mid), immediate reload -- cube-corner config. */
    edge12_profile(&vctx, vram, stride, "lr0 sl+ flat  ", 750,400.25,  60,400.25, 252,352);
    edge12_profile(&vctx, vram, stride, "lr1 sl- flat  ",  40,400.25, 730,400.25, 538,352);

    printf("\nProbe complete. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
