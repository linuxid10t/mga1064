/*
 * gltritest.c - 3D triangle readback through the DEMO engine sequence.
 *
 * Symptom-2 follow-on (HANDOFF "Open symptom 2", 3D cutoff). tritest proved a
 * full-height triangle renders when the engine sequence is  clear_z -> draw.
 * But the cube/triangle demos cut off ~2/5 down, and their sequence is
 * clear_z -> fill_rect (color clear) -> draw. This test reproduces BOTH
 * sequences on the same chip and CPU-reads the triangle's real Y extent, then
 * tests three interventions on the failing sequence to pin the cause:
 *
 *   raw_zclear  : clear_z -> draw                      (tritest seq; FULL?)
 *   demo_seq    : clear_z -> fill_rect -> draw         (demo seq;   CUT?)
 *   demo_sleep  : demo_seq + 10 ms wall-clock after the fill (timing probe)
 *   demo_fifowait: demo_seq + correct FIFO + 3D-idle poll after the fill
 *   demo_rearm  : demo_seq + reprogram the 3D dest/z/stride/clip before draw
 *
 * Why these three. A failed Z compare only *suppresses* pixels (DB019-B
 * §15.4.6, PDF p.129) -- it never stops the scanline walk, and this triangle
 * has uniform z at every vertex (TdZdX = TdZdY = 0), so the Z compare is
 * exonerated regardless. virge_clear_z ends with virge_wait_engine()
 * (virge.c:862) but virge_fill_rect does NOT, and virge_wait_engine polls
 * only SUBSYS_STATUS bit 13 (3DIDLE, virge.c:661) -- which is already set
 * while a 2D fill is still draining. So the demo's color-clear fill may still
 * be writing VRAM when the triangle draws. If demo_sleep or demo_fifowait
 * restores full height while demo_seq cuts off, the cause is a 2D/3D
 * synchronization gap and the durable fix is a correct engine wait.
 *
 * Links virge.c only (the bug is in the virge.c call sequence, not the
 * frontend). Build: make gltritest      Run: sudo ./gltritest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

/* Same full-height triangle as tritest (y=20..580, uniform z=0.5) so the
 * extents are directly comparable. Vertex order is irrelevant: the backend
 * sorts by Y. */
static void draw_test_triangle(struct virge_ctx *ctx)
{
    struct virge_vertex v_red   = { .x=400, .y=580, .z=0.5f, .w=1,
                                    .r=1, .g=0, .b=0, .a=1 };
    struct virge_vertex v_green = { .x=700, .y=300, .z=0.5f, .w=1,
                                    .r=0, .g=1, .b=0, .a=1 };
    struct virge_vertex v_blue  = { .x=120, .y=20,  .z=0.5f, .w=1,
                                    .r=0, .g=0, .b=1, .a=1 };
    virge_draw_triangle_gouraud(ctx, v_red, v_green, v_blue);
}

static uint16_t read_px(const uint8_t *base, uint32_t stride, int x, int y)
{
    return *(const uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
}

static void cpu_clear_fb(struct virge_ctx *ctx, int W, int H, uint32_t stride)
{
    uint8_t *vram = (uint8_t *)ctx->fb;
    for (int y = 0; y < H; y++) {
        uint16_t *row = (uint16_t *)(vram + (size_t)y * stride);
        for (int x = 0; x < W; x++) row[x] = 0;
    }
}

/* Correct engine drain per DB019-B p.300: 3D idle (bit 13) AND all 16 FIFO
 * slots free (bits 12-8 == 0x10). virge_wait_engine checks only bit 13.
 * Bounded -- no unbounded register poll (HANDOFF ground rule). */
static void wait_fifo_2d_idle(struct virge_ctx *ctx)
{
    for (int i = 0; i < 200000; i++) {
        uint32_t s = virge_read32(ctx, VIRGE_SUBSYS_STATUS);
        int slots_free = (s >> 8) & 0x1F;
        if ((s & VIRGE_STATUS_3DIDLE) && slots_free == 0x10)
            return;
    }
    printf("    (wait_fifo_2d_idle: TIMEOUT)\n");
}

/* Reprogram the 3D destination/Z/stride/clip state that engine_init_3d sets
 * once (virge.c:696). The conservative period-driver pattern: re-arm before
 * every primitive. Tests whether real DX silicon shares more between the 2D
 * and 3D banks than 86Box models (86Box keeps them separate). */
static void rearm_3d(struct virge_ctx *ctx)
{
    uint32_t dest_stride = ctx->stride;
    uint32_t z_stride    = ctx->width * 2;
    virge_write32(ctx, VIRGE_3D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_3D_Z_BASE,    ctx->z_base & ~0x7);
    virge_write32(ctx, VIRGE_3D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
    virge_write32(ctx, VIRGE_3D_Z_STRIDE, z_stride & 0xFFF);
    virge_write32(ctx, VIRGE_3D_CLIP_L_R,
                  (0 << 16) | ((ctx->width - 1) & 0x7FF));
    virge_write32(ctx, VIRGE_3D_CLIP_T_B,
                  (0 << 16) | ((ctx->height - 1) & 0x7FF));
}

enum { RAW_ZCLEAR, DEMO_SEQ, DEMO_SLEEP, DEMO_FIFOWAIT, DEMO_REARM };

static const char *mode_name(int m)
{
    switch (m) {
    case RAW_ZCLEAR:   return "raw_zclear  (clear_z -> draw)";
    case DEMO_SEQ:     return "demo_seq    (clear_z -> fill -> draw)";
    case DEMO_SLEEP:   return "demo_sleep  (+10 ms after fill)";
    case DEMO_FIFOWAIT:return "demo_fifowait(+FIFO/2D-idle poll)";
    case DEMO_REARM:   return "demo_rearm  (+rearm 3D state)";
    }
    return "?";
}

/* Run one variant: known-black FB, the variant's clear sequence, the variant's
 * intervention, the triangle, then read back its real Y extent. Returns maxy. */
static int run_variant(struct virge_ctx *ctx, int W, int H, uint32_t stride,
                       uint8_t *vram, int mode)
{
    virge_wait_engine(ctx);
    cpu_clear_fb(ctx, W, H, stride);

    /* Clear sequence: every variant clears Z; the demo-based variants also do
     * the full-screen color fill (the demo's clear_color) AFTER the Z clear,
     * which is the load-bearing ordering difference vs tritest. */
    virge_clear_z(ctx, 1.0f);
    if (mode != RAW_ZCLEAR)
        virge_fill_rect(ctx, 0, 0, W, H, 0);   /* black color clear */

    /* Intervention between the fill and the draw. */
    if (mode == DEMO_SLEEP)
        usleep(10 * 1000);
    else if (mode == DEMO_FIFOWAIT)
        wait_fifo_2d_idle(ctx);
    else if (mode == DEMO_REARM)
        rearm_3d(ctx);

    draw_test_triangle(ctx);
    virge_wait_engine(ctx);

    /* Read back the triangle extent. */
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
    const char *verdict = (maxy > 540) ? "FULL" : (maxy > 0 ? "CUT" : "EMPTY");
    printf("  %-34s maxy=%-4d count=%-6d  [%s]\n",
           mode_name(mode), maxy, count, verdict);
    return maxy;
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

    int W = vctx.width, H = vctx.height;
    uint32_t stride = vctx.stride;
    uint8_t *vram = (uint8_t *)vctx.fb;

    printf("\n3D triangle readback (demo engine sequence): "
           "%dx%d stride %u, fb_base 0x%x z_base 0x%x\n",
           W, H, stride, vctx.fb_base, vctx.z_base);

    /* Confirm the 3D clip window engine_init_3d programmed is full-screen
     * (exonerates the clip before we start). Bottom clip = H-1 in [10:0]. */
    uint32_t clip_tb = virge_read32(&vctx, VIRGE_3D_CLIP_T_B);
    uint32_t clip_lr = virge_read32(&vctx, VIRGE_3D_CLIP_L_R);
    printf("3D clip readback: L_R=0x%x T_B=0x%x (expect right=%d bottom=%d)\n",
           clip_lr, clip_tb, W - 1, H - 1);

    printf("\nVariant results (full triangle reaches maxy~580):\n");
    int modes[] = { RAW_ZCLEAR, DEMO_SEQ, DEMO_SLEEP, DEMO_FIFOWAIT, DEMO_REARM };
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        run_variant(&vctx, W, H, stride, vram, modes[i]);

    printf("\nVerdict guide:\n");
    printf("  raw_zclear FULL + demo_seq CUT          -> reproduced (fill is the trigger)\n");
    printf("  demo_sleep or demo_fifowait FULL        -> cause is 2D/3D sync; fix the wait\n");
    printf("  demo_rearm FULL                         -> re-arm 3D state per draw on this silicon\n");
    printf("  all demo_* still CUT                    -> not timing/rearm; run L10GL_TRACE diff\n");

    /* Leave the reproduced bug (demo_seq) on screen for an optional photo. */
    printf("\nLeaving demo_seq (the reproduced cutoff) on screen. Ctrl-C to exit.\n");
    virge_clear_z(&vctx, 1.0f);
    virge_fill_rect(&vctx, 0, 0, W, H, 0);
    draw_test_triangle(&vctx);
    virge_wait_engine(&vctx);
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}
