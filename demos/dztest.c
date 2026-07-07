/*
 * dztest.c - measure the 3D engine's per-pixel X Z-gradient (TdZdX) on silicon.
 *
 * Why this exists: tritest uses flat Z (z=0.5 at every vertex -> dzdx = 0), so
 * it cannot expose a Z-GRADIENT scale error. The cube's tilted faces have real
 * dzdx, and after the depth-range fix the back-face bleedthrough is "reduced
 * but not gone" -- the signature of a wrong gradient scale, not a wrong constant.
 *
 * Hypothesis (cited from 86Box vid_s3_virge.c, the behavioral reference): the
 * rasterizer adds TdZdX directly to the already-left-shifted Z accumulator
 * (z = base_z<<1 at line 4261; z += TdZdX at 4413; compare z>>16 at ~4355),
 * while TdZdY is added to base_z BEFORE the <<1 (line 4433). So TdZdX must be
 * scaled 2^16 (16 frac bits) but TZS/TdZdY are 2^15. The driver applies
 * VIRGE_Z_FIXED (2^15) to all three (virge.c:1102-1105) -> TdZdX is
 * HALF-STRENGTH. 86Box is known to diverge from real DX on Z (the Z_STRIDE
 * clobber), so confirm with one hardware measurement before changing the scale.
 *
 * Method: draw one Gouraud triangle whose Z is a function of X only (z=0.2 at
 * x=100, z=0.8 at x=700; the left edge is constant-z so the ramp is
 * Y-independent), Z-update ON (the virge_init default). CPU-read the Z buffer,
 * average the written Z per X column, and compute the rendered per-pixel Z-word
 * slope. Intended slope = dzdx*65535 = (0.6/600)*65535 ~= 65.5 Z-words/pixel.
 *
 * Outcomes:
 *   measured ~= 32-33  -> TdZdX half-strength CONFIRMED -> apply x2 fix.
 *   measured ~= 65-66  -> X-gradient correct; bleedthrough cause is elsewhere.
 *   anything else      -> raw column samples are printed; re-examine.
 *
 * Build: make -B BACKEND=virge dztest     Run: sudo ./dztest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

static uint16_t read_z(const uint8_t *zram, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(zram + (size_t)y * stride + (size_t)x * 2);
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
    uint8_t *zram = vram + vctx.z_base;

    /* Triangle: Z = z_left + (x-x_left)/(x_right-x_left) * (z_right-z_left),
     * a function of X only (dzdy = 0). Vertices in bottom/mid/top Y-order,
     * which the engine requires (virge.c edge assignment):
     *   v0 bottom (x_left, 500) z=z_left   v1 mid (x_right,300) z=z_right
     *   v2 top   (x_left, 100) z=z_left
     * The constant-z left edge makes the ramp identical on every scanline. */
    const float z_left = 0.2f, z_right = 0.8f;
    const int   x_left = 100,  x_right = 700;
    const float intended_slope =
        (z_right - z_left) / (float)(x_right - x_left) * 65535.0f;  /* dzdx*65535 */

    printf("\nTdZdX gradient probe: %dx%d stride %u, Z base 0x%x\n",
           W, H, stride, vctx.z_base);
    printf("Triangle z=f(X): z=%.3f at x=%d, z=%.3f at x=%d (dzdy=0).\n",
           z_left, x_left, z_right, x_right);
    printf("Intended per-pixel Z-word slope = %.2f (dzdx*65535).\n\n",
           intended_slope);

    /* CPU-clear framebuffer black, then Z to far (1.0 = 0xFFFF). */
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }
    virge_clear_z(&vctx, 1.0f);
    virge_wait_engine(&vctx);

    /* Draw the ramp triangle (white, so coverage is visible in the FB too).
     * z_cmd_bits left at the virge_init default: Z ON, LEQUAL, Z-update. */
    struct virge_vertex v0 = { .x = x_left,  .y = 500, .z = z_left,  .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v1 = { .x = x_right, .y = 300, .z = z_right, .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    struct virge_vertex v2 = { .x = x_left,  .y = 100, .z = z_left,  .w = 1,
                               .r = 1, .g = 1, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(&vctx, v0, v1, v2);
    virge_wait_engine(&vctx);

    /* Average written Z per X column over the screen. A pixel is "rendered"
     * iff its Z word != 0xFFFF (the cleared far value; no rendered z reaches
     * 1.0 here, so this is an unambiguous coverage test). */
    long col_sum[1024];
    int  col_cnt[1024];
    for (int x = 0; x < W && x < 1024; x++) { col_sum[x] = 0; col_cnt[x] = 0; }

    int minx = -1, maxx = -1, rendered = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t z = read_z(zram, stride, x, y);
            if (z != 0xFFFF) {
                col_sum[x] += z;
                col_cnt[x] += 1;
                rendered++;
                if (minx < 0 || x < minx) minx = x;
                if (maxx < 0 || x > maxx) maxx = x;
            }
        }
    }
    printf("Rendered Z-written pixels: %d, X range [%d,%d]\n", rendered, minx, maxx);

    if (rendered > 0) {
        /* Column-averaged Z at ~9 evenly spaced X positions, vs intended. */
        printf("\n  x      avg-Z-word   intended\n");
        for (int s = 0; s <= 8; s++) {
            int x = minx + (maxx - minx) * s / 8;
            if (x < 0 || x >= W || col_cnt[x] == 0) continue;
            float avg = (float)col_sum[x] / col_cnt[x];
            float inten = (z_left + (float)(x - x_left) / (x_right - x_left)
                           * (z_right - z_left)) * 65535.0f;
            printf("  %-4d   %10.1f   %10.1f\n", x, avg, inten);
        }

        /* Measured slope from column averages near the two ends, with a small
         * margin to avoid silhouette sub-pixel noise. */
        int margin = 8;
        int xa = minx + margin, xb = maxx - margin;
        while (xa < W && col_cnt[xa] == 0) xa++;
        while (xb > 0  && col_cnt[xb] == 0) xb--;
        if (xb > xa) {
            float za = (float)col_sum[xa] / col_cnt[xa];
            float zb = (float)col_sum[xb] / col_cnt[xb];
            float measured_slope = (zb - za) / (xb - xa);
            float ratio = measured_slope / intended_slope;

            printf("\nMeasured slope: (z[%d]=%.1f - z[%d]=%.1f) / (%d-%d)"
                   " = %.2f Z-words/pixel\n",
                   xb, zb, xa, za, xb, xa, measured_slope);
            printf("Intended slope: %.2f Z-words/pixel\n", intended_slope);
            printf("Ratio measured/intended = %.3f\n\n", ratio);

            if (ratio > 0.40f && ratio < 0.60f)
                printf("  -> TdZdX ~HALF-STRENGTH CONFIRMED: the X Z-gradient is\n"
                       "     scaled 2^15 but the hardware adds TdZdX to the\n"
                       "     already-<<1 Z accumulator (needs 2^16). Apply x2.\n");
            else if (ratio > 0.90f && ratio < 1.10f)
                printf("  -> X-gradient CORRECT on silicon. Bleedthrough cause is\n"
                       "     NOT the TdZdX scale; re-examine order/winding/edges.\n");
            else
                printf("  -> Ratio %.3f is neither ~0.5 nor ~1.0 -- raw samples\n"
                       "     above; re-examine the Z-gradient encoding first.\n",
                       ratio);
        } else {
            printf("\nCould not find two clean columns for a slope measurement.\n");
        }
    } else {
        printf("  -> NOTHING RENDERED (Z-write off or winding wrong?).\n");
    }

    printf("\nProbe complete. Ctrl-C to exit.\n");
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}
