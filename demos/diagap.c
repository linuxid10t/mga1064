/*
 * diagap.c - reproduce the cube's 310-deg Left-face shared-diagonal notch
 * in ISOLATION, to split coverage-regime vs Z/draw-order.
 *
 * seamtest proved edge-01 and edge-12 step IDENTICALLY, and that the
 * isolated edges predict a WATERTIGHT diagonal (meas_A = meas_B+1 every
 * row). Yet cubefb shows a ~3px background gap inside the Left face at
 * 310 deg (hole (485,365)) -- a within-face seam along the shared diagonal
 * vert0-vert7, which is edge-12 in tri {0,4,7} and edge-01 in tri {0,7,3}.
 *
 * The isolated edge probes are single-triangle-vs-background, so they are
 * blind to (a) the cube's scan_12=27 / yf~0.40 regime (probes used 48 and
 * {.25,.75}) and (b) any two-triangle Z=LESS interaction over the coplanar
 * pair. This probe renders the EXACT projected Left tris at 310 deg (same
 * rotation+projection as cubefb) in five passes and measures the per-row
 * gap directly:
 *   A-alone, B-alone             -> where each tri's diagonal boundary
 *                                   lands (true = correct; >true = the tri
 *                                   alone under-fills => coverage regime)
 *   both Z=LESS A-first / B-first-> does the gap reproduce; order-dependent?
 *   both Z_ALWAYS                -> does disabling the Z-test close it?
 * tri A = red, tri B = green (distinct so ownership/overlap is visible;
 * color does not affect geometry or Z).
 *
 * Build: make -B BACKEND=virge diagap     Run: sudo ./diagap [angle-index]
 *   default angle-index 31 = 310 deg (cubefb's worst). Try others, e.g. 3.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

#define PI 3.14159265358979323846f

static void build_rotation(float m[3][3], float ax, float ay)
{
    float sx = sinf(ax), cx = cosf(ax);
    float sy = sinf(ay), cy = cosf(ay);
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
struct screen_vertex { float sx, sy, sz; };
static void project(struct screen_vertex *o, const float in[3], int sw, int sh, float cd)
{
    float z = in[2] + cd;
    if (z < 0.1f) z = 0.1f;
    float s = (float)sh / z;
    o->sx = (float)sw * 0.5f + in[0] * s;
    o->sy = (float)sh * 0.5f - in[1] * s;
    o->sz = (in[2] + cd - 3.0f) / 4.0f;
    if (o->sz < 0.0f) o->sz = 0.0f;
    if (o->sz > 1.0f) o->sz = 1.0f;
}
static const float cube_verts[8][3] = {
    {-1, -1, -1}, { 1, -1, -1}, { 1,  1, -1}, {-1,  1, -1},
    {-1, -1,  1}, { 1, -1,  1}, { 1,  1,  1}, {-1,  1,  1},
};

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

/* 0=empty, 1=red(tri A), 2=green(tri B), 3=other */
static int classify555(uint16_t v)
{
    if (v == 0) return 0;
    int r = (v >> 10) & 0x1F, g = (v >> 5) & 0x1F, b = v & 0x1F;
    if (r >= 27 && g <= 4 && b <= 4) return 1;
    if (g >= 27 && r <= 4 && b <= 4) return 2;
    return 3;
}

static struct virge_vertex mkvert(struct screen_vertex s, float cr, float cg, float cb)
{
    struct virge_vertex v = { .x=s.sx, .y=s.sy, .z=s.sz, .w=1, .r=cr, .g=cg, .b=cb, .a=1 };
    return v;
}

static void clear_fb(uint8_t *vram, int W, int H, uint32_t stride)
{
    for (int y = 0; y < H; y++) {
        uint16_t *r = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) r[x] = 0;
    }
}

/* which: 0=A only, 1=B only, 2=both. Prints per-row boundary/gap; returns
 * the max gap width seen (0 = watertight). *gap_row gets the worst row. */
static int run_pass(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                    struct screen_vertex *P, const char *name,
                    int which, uint32_t zbc, int afirst,
                    int ylo, int yhi, int *gap_row)
{
    int W = vctx->width;
    clear_fb(vram, W, vctx->height, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);
    vctx->z_cmd_bits = VIRGE_ZB_NORMAL | zbc | VIRGE_ZUP_ENABLE;

    struct virge_vertex A0 = mkvert(P[0], 1,0,0), A4 = mkvert(P[4], 1,0,0), A7 = mkvert(P[7], 1,0,0);
    struct virge_vertex B0 = mkvert(P[0], 0,1,0), B7 = mkvert(P[7], 0,1,0), B3 = mkvert(P[3], 0,1,0);

    if (which == 0) {
        virge_draw_triangle_gouraud(vctx, A0, A4, A7);
    } else if (which == 1) {
        virge_draw_triangle_gouraud(vctx, B0, B7, B3);
    } else {
        if (afirst) { virge_draw_triangle_gouraud(vctx, A0, A4, A7);
                       virge_draw_triangle_gouraud(vctx, B0, B7, B3); }
        else        { virge_draw_triangle_gouraud(vctx, B0, B7, B3);
                       virge_draw_triangle_gouraud(vctx, A0, A4, A7); }
    }
    virge_wait_engine(vctx);

    float dyd = P[7].sy - P[0].sy;   /* signed; vert7-vert0 may flip with angle */
    printf("\n--- %s ---\n", name);
    printf("  %-4s %-8s %-8s %-8s %s\n", "y", "true", "B-rt", "A-lf", "result");
    int maxgap = 0; *gap_row = -1;
    int yr_lo = ylo < yhi ? ylo : yhi;
    int yr_hi = ylo < yhi ? yhi : ylo;
    for (int y = yr_lo; y <= yr_hi; y++) {
        const uint16_t *row = (const uint16_t *)(vram + (size_t)y * stride);
        int b_right = -1, a_left = -1;
        for (int x = 0; x < W; x++) if (classify555(row[x]) == 2) b_right = x;        /* last green  */
        for (int x = 0; x < W; x++) if (classify555(row[x]) == 1) { a_left = x; break; } /* first red */
        float tx = (dyd != 0.0f) ? P[7].sx + (P[7].sy - y) / dyd * (P[0].sx - P[7].sx) : P[7].sx;
        printf("  %-4d %-8.2f %-8d %-8d ", y, tx, b_right, a_left);
        if (b_right < 0 && a_left < 0) { printf("(empty)\n"); continue; }
        if (b_right < 0 || a_left < 0) { printf("(single tri: %s boundary above)\n",
                                                    a_left >= 0 ? "A" : "B"); continue; }
        int gw = a_left - b_right - 1;
        if (gw > 0) {
            printf("GAP %d-%d (%dpx)\n", b_right + 1, a_left - 1, gw);
            if (gw > maxgap) { maxgap = gw; *gap_row = y; }
        } else if (gw == 0) {
            printf("watertight\n");
        } else {
            printf("OVERLAP %dpx\n", -gw);
        }
    }
    return maxgap;
}

/* Draw tri A ALONE under Z=ALWAYS + Z-update (so A draws its full footprint
 * AND writes its interpolated Z everywhere, ignoring the Z test), then read
 * the Z buffer back across the scanline at three rows (top=correct region,
 * mid, bottom=worst gap). This shows tri A's ACTUAL Z plane as the engine
 * computed it.
 *
 * DECISIVE for the notch: A-alone-LESS under-fills the diagonal side but
 * A-alone-ALWAYS fills it perfectly, so the only thing LESS can be rejecting
 * on is A's own Z >= cleared 1.0. If the gap columns here read back saturated
 * at ~1.0 (0xffff) while the red columns show a normal gradient, that
 * confirms A's Z runs past 1.0 on its diagonal side -- a Z-seed/gradient
 * setup error, NOT coverage. The cleared-but-unwritten columns also read
 * 0xffff, so the telling signal is the Z value at the FIRST red column
 * (a_left): if it is ~1.0 there and only drops well inside the triangle,
 * A's Z plane is shifted/saturated at the diagonal. */
static void run_z_probe(struct virge_ctx *vctx, uint8_t *vram, uint32_t stride,
                        struct screen_vertex *P, int ylo, int yhi)
{
    int W = vctx->width;
    clear_fb(vram, W, vctx->height, stride);
    virge_clear_z(vctx, 1.0f);
    virge_wait_engine(vctx);
    vctx->z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_ALWAYS | VIRGE_ZUP_ENABLE;

    struct virge_vertex A0 = mkvert(P[0],1,0,0), A4 = mkvert(P[4],1,0,0), A7 = mkvert(P[7],1,0,0);
    virge_draw_triangle_gouraud(vctx, A0, A4, A7);
    virge_wait_engine(vctx);

    size_t zbase = vctx->z_base;
    int zstr = W * 2;                       /* 16-bit Z, one word per pixel */
    float dyd = P[7].sy - P[0].sy;
    int yr_lo = ylo < yhi ? ylo : yhi;
    int yr_hi = ylo < yhi ? yhi : ylo;
    int sample[3];
    const char *tag[3];
    sample[0] = yr_lo + 1; tag[0] = "top (near v2, should be correct)";
    sample[1] = (yr_lo + yr_hi) / 2; tag[1] = "mid";
    sample[2] = yr_hi;     tag[2] = "bottom (near v1, worst gap)";

    printf("\n--- A alone, Z=ALWAYS+Zupdate, Z-readback ---\n");
    printf("  Z buf @0x%x stride %d (cleared 1.0 = 0xffff). Dumps A's written Z\n", vctx->z_base, zstr);
    printf("  across each scanline: 'A'=A drew color, '.'=empty; z=Zfloat(0..1).\n");
    for (int r = 0; r < 3; r++) {
        int y = sample[r];
        if (y < yr_lo || y > yr_hi) continue;
        const uint16_t *crow = (const uint16_t *)(vram + (size_t)y * stride);
        const uint16_t *zrow = (const uint16_t *)(vram + zbase + (size_t)y * zstr);
        float tx = (dyd != 0.0f) ? P[7].sx + (P[7].sy - y) / dyd * (P[0].sx - P[7].sx) : P[7].sx;
        int xc = (int)tx;
        int a_left = -1;
        for (int x = 0; x < W; x++) if (classify555(crow[x]) == 1) { a_left = x; break; }
        int hi = (a_left > 0 ? a_left : xc) + 6;
        printf("\n  y=%d %s  trueX=%.1f  a_left=%d  gap=%d\n",
               y, tag[r], tx, a_left, a_left > 0 ? a_left - xc : -1);
        int lo = xc - 2, n = 0;
        for (int x = lo; x <= hi; x++) {
            if (x < 0 || x >= W) continue;
            int cls = classify555(crow[x]);
            float zf = zrow[x] / 65535.0f;
            printf("  x%-4d%c z%.3f ", x, cls == 1 ? 'A' : '.', zf);
            if (++n % 6 == 0) printf("\n");
        }
        if (n % 6) printf("\n");
    }
}

int main(int argc, char **argv)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, 800, 600, 2) < 0) { fprintf(stderr, "virge_init failed\n"); return 1; }
    struct sigaction sa; memset(&sa, 0, sizeof(sa)); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    uint8_t *vram = (uint8_t *)vctx.fb;
    uint32_t stride = vctx.stride;
    int idx = (argc >= 2) ? atoi(argv[1]) : 31;                 /* 31 = 310 deg */
    int N = 36;
    float angle = (float)idx * 2.0f * (float)PI / (float)N;

    float rot[3][3];
    build_rotation(rot, angle, angle * 0.7f);
    struct screen_vertex P[8];
    float transformed[8][3];
    for (int i = 0; i < 8; i++) {
        mat3_transform(transformed[i], rot, cube_verts[i]);
        project(&P[i], transformed[i], vctx.width, vctx.height, 5.0f);
    }

    int ylo = (int)P[0].sy, yhi = (int)P[7].sy;                 /* diagonal scan range */
    float dxdy = (P[7].sy != P[0].sy) ? (P[0].sx - P[7].sx) / (P[7].sy - P[0].sy) : 0.0f;

    printf("\ndiagap: isolated cube Left-face shared diagonal (%dx%d stride %u)\n",
           vctx.width, vctx.height, stride);
    printf("angle index %d = %.2f rad (%.0f deg)\n", idx, angle, angle * 180.0f / (float)PI);
    printf("Left verts: 0=(%.1f,%.1f) 3=(%.1f,%.1f) 4=(%.1f,%.1f) 7=(%.1f,%.1f)\n",
           P[0].sx,P[0].sy, P[3].sx,P[3].sy, P[4].sx,P[4].sy, P[7].sx,P[7].sy);
    printf("shared diagonal vert7->vert0: rows [%d,%d] (scan=%d), slope %.2f px/row\n",
           ylo, yhi, yhi - ylo, dxdy);
    printf("tri A {0,4,7}=red (diagonal=edge-12); tri B {0,7,3}=green (diagonal=edge-01)\n");

    struct { const char *name; int which; uint32_t zbc; int afirst; } passes[] = {
        { "A alone (LESS)",           0, VIRGE_ZBC_LESS,   1 },
        { "B alone (LESS)",           1, VIRGE_ZBC_LESS,   1 },
        { "both LESS  A-first",       2, VIRGE_ZBC_LESS,   1 },
        { "both LESS  B-first",       2, VIRGE_ZBC_LESS,   0 },
        { "both ALWAYS A-first",      2, VIRGE_ZBC_ALWAYS, 1 },
    };
    int results[5];
    for (int i = 0; i < 5; i++) {
        int grow = 0;
        results[i] = run_pass(&vctx, vram, stride, P, passes[i].name,
                              passes[i].which, passes[i].zbc, passes[i].afirst,
                              ylo, yhi, &grow);
        if (results[i] > 0)
            printf("  => %s: WORST GAP %dpx at row %d\n", passes[i].name, results[i], grow);
        else
            printf("  => %s: watertight (no gap)\n", passes[i].name);
    }

    printf("\n=== SUMMARY ===\n");
    printf("Read A-alone/B-alone first: if a tri's boundary is >trueX by ~the gap,\n");
    printf("the tri ALONE under-fills (coverage regime, scan_12=%d). If both alone\n", yhi - ylo);
    printf("are correct but the both-passes gap, it is a two-triangle interaction:\n");
    printf("Z=LESS vs Z=ALWAYS splits Z-rejection vs pure coverage.\n");
    for (int i = 0; i < 5; i++)
        printf("  %-22s max gap %dpx\n", passes[i].name, results[i]);

    /* If any both-pass gapped but A-alone-ALWAYS didn't (the 310-deg
     * signature: LESS rejects A's diagonal-side pixels), read A's actual Z
     * to confirm saturation. Run unconditionally -- at clean angles it is a
     * useful control (A's Z should stay <1.0 everywhere). */
    run_z_probe(&vctx, vram, stride, P, ylo, yhi);

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    virge_cleanup(&vctx);
    return 0;
}
