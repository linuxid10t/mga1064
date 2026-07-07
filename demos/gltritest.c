/*
 * gltritest.c - 3D triangle readback: isolate the demo cutoff to the Z mode.
 *
 * First run (commit 7fa057e) settled the structural questions:
 *   - The demo's clear_z -> fill_rect -> draw sequence renders FULL through
 *     virge.c directly (demo_seq was maxy=579). So the cutoff is NOT in the
 *     virge.c call sequence, NOT a 2D/3D sync race, NOT FIFO, NOT re-arm.
 *   - The 3D clip readback is full-screen (right=799, bottom=599), so clip is
 *     out. virge_scanout_takeover adopts the live 800x600/1600 raster for any
 *     caller request, so ctx geometry matches gltritest regardless.
 *
 * That leaves exactly ONE engine-state difference vs the working gltritest
 * path: the demo's z_cmd_bits = ZB_NORMAL | ZBC_LESS | ZUP (set by the glue's
 * GL-default LESS), vs virge_init's ZBC_LEQUAL default. Uniform z (0.5 at
 * every vertex, cleared Zzb=0xFFFF) should pass identically under LESS and
 * LEQUAL, so this matrix is expected to be all-FULL -- which would mean the
 * demo cutoff is stale binaries, not current code. ALWAYS/NEVER are controls:
 * ALWAYS must be FULL, NEVER must be EMPTY (they prove the Z compare and the
 * readback are both genuinely wired up).
 *
 * Usage: sudo ./gltritest [init_w init_h]   (default 800 600; try 640 480 to
 *        exercise the demo's adoption path).
 * Build: make gltritest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;
static void sighandler(int sig) { (void)sig; running = 0; }

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

/* The demo's triangle (fractional screen positions, uniform z=0.5), scaled to
 * whatever ctx actually adopted. Vertex order is irrelevant (backend sorts). */
static void draw_demo_triangle(struct virge_ctx *ctx, int W, int H)
{
    struct virge_vertex v_red = {
        .x = 0.50f * W, .y = 0.917f * H, .z = 0.5f, .w = 1,
        .r = 1, .g = 0, .b = 0, .a = 1 };
    struct virge_vertex v_green = {
        .x = 0.81f * W, .y = 0.500f * H, .z = 0.5f, .w = 1,
        .r = 0, .g = 1, .b = 0, .a = 1 };
    struct virge_vertex v_blue = {
        .x = 0.19f * W, .y = 0.125f * H, .z = 0.5f, .w = 1,
        .r = 0, .g = 0, .b = 1, .a = 1 };
    virge_draw_triangle_gouraud(ctx, v_red, v_green, v_blue);
}

struct zmode { const char *name; uint32_t bits; };

/* Run the demo sequence (clear_z -> fill -> draw) under one Z mode and read
 * back the triangle's Y extent. Returns maxy. */
static int run_zmode(struct virge_ctx *ctx, int W, int H, uint32_t stride,
                     uint8_t *vram, const struct zmode *zm)
{
    virge_wait_engine(ctx);
    cpu_clear_fb(ctx, W, H, stride);
    virge_clear_z(ctx, 1.0f);
    virge_fill_rect(ctx, 0, 0, W, H, 0);      /* demo color clear */

    ctx->z_cmd_bits = zm->bits;               /* the one variable under test */

    draw_demo_triangle(ctx, W, H);
    virge_wait_engine(ctx);

    int miny = -1, maxy = -1, count = 0;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            if (read_px(vram, stride, x, y)) {
                if (miny < 0 || y < miny) miny = y;
                if (maxy < 0 || y > maxy) maxy = y;
                count++;
            }
        }
    }
    const char *verdict = (maxy > 540) ? "FULL" : (maxy > 0 ? "CUT" : "EMPTY");
    printf("  ZBC %-22s bits=0x%07x  maxy=%-4d count=%-6d [%s]\n",
           zm->name, zm->bits >> 20 & 0x7, maxy, count, verdict);
    return maxy;
}

int main(int argc, char **argv)
{
    int req_w = 800, req_h = 600;
    if (argc >= 3) {
        req_w = atoi(argv[1]);
        req_h = atoi(argv[2]);
    }

    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));
    if (virge_init(&vctx, req_w, req_h, 2) < 0) {
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

    printf("\n3D triangle Z-mode matrix: requested %dx%d -> adopted %dx%d "
           "stride %u, fb 0x%x z 0x%x\n", req_w, req_h, W, H, stride,
           vctx.fb_base, vctx.z_base);
    uint32_t clip_tb = virge_read32(&vctx, VIRGE_3D_CLIP_T_B);
    uint32_t clip_lr = virge_read32(&vctx, VIRGE_3D_CLIP_L_R);
    printf("3D clip readback: right=%u bottom=%u (L_R=0x%x T_B=0x%x)\n",
           clip_lr & 0x7FF, clip_tb & 0x7FF, clip_lr, clip_tb);

    /* The demo sequence under each Z mode. */
    struct zmode modes[] = {
        { "LEQUAL (init def)", VIRGE_ZB_NORMAL | VIRGE_ZBC_LEQUAL | VIRGE_ZUP_ENABLE },
        { "LESS    (demo/glue)", VIRGE_ZB_NORMAL | VIRGE_ZBC_LESS  | VIRGE_ZUP_ENABLE },
        { "ALWAYS  (+control)", VIRGE_ZB_NORMAL | VIRGE_ZBC_ALWAYS | VIRGE_ZUP_ENABLE },
        { "NEVER   (-control)", VIRGE_ZB_NORMAL | VIRGE_ZBC_NEVER  | VIRGE_ZUP_ENABLE },
    };
    printf("\nDemo sequence (clear_z -> fill -> draw) per Z compare code "
           "(full triangle reaches maxy~%d):\n", (int)(0.917f * H));
    for (size_t i = 0; i < sizeof(modes) / sizeof(modes[0]); i++)
        run_zmode(&vctx, W, H, stride, vram, &modes[i]);

    printf("\nReading guide:\n");
    printf("  LESS FULL (like LEQUAL)      -> Z mode exonerated; demo cutoff is\n"
           "                                  stale binaries -- rebuild triangle/cube.\n");
    printf("  LESS CUT, LEQUAL/ALWAYS FULL -> ZBC_LESS mis-encoded on this silicon.\n");
    printf("  ALWAYS not FULL / NEVER not EMPTY -> Z compare or readback not wired up.\n");

    /* Leave the demo's actual config (LESS) on screen for a photo. */
    printf("\nLeaving the demo's exact config (LESS) on screen. Ctrl-C to exit.\n");
    virge_clear_z(&vctx, 1.0f);
    virge_fill_rect(&vctx, 0, 0, W, H, 0);
    vctx.z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_LESS | VIRGE_ZUP_ENABLE;
    draw_demo_triangle(&vctx, W, H);
    virge_wait_engine(&vctx);
    while (running) {
        if (getchar() == EOF)
            break;
    }

    virge_cleanup(&vctx);
    return 0;
}
