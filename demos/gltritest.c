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
 * The matrix found it: ZBC_LESS (code 4) cuts the triangle at maxy~234 while
 * LEQUAL (6) and ALWAYS (7) are FULL and NEVER (0) is EMPTY. So the GL-default
 * LESS the glue sets is the culprit. Uniform z (0.5 every vertex, cleared
 * Zzb=0xFFFF) should make LESS pass everywhere, yet it fails the bottom --
 * implying Zs drifted up to 0xFFFF there (LEQUAL passes via equality, LESS
 * fails on strict inequality) even though dzdx=dzdy=0 -> TdZdX=TdZdY=0. This
 * build dumps the Z-buffer profile under LESS to read the actual written Zs
 * per row and settle drift vs compare-semantics.
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

/* Sample the Z buffer at the first triangle pixel of every 20th row. After a
 * draw with ZUP, the Z buffer holds the triangle's Zs where it passed and the
 * cleared 0xFFFF where it failed, so this prints Zs vs Y directly -- revealing
 * whether Zs drifted up toward 0xFFFF or stayed flat at ~0x4000. */
static void dump_z_profile(int W, int H, uint32_t stride,
                           const uint8_t *vram, const uint8_t *zram)
{
    for (int y = 0; y < H; y += 20) {
        int x_hit = -1;
        for (int x = 0; x < W; x++) {
            if (read_px(vram, stride, x, y)) { x_hit = x; break; }
        }
        if (x_hit < 0)
            continue;
        uint16_t z = read_px(zram, stride, x_hit, y);
        printf("    y=%3d x=%3d  Z=0x%04x\n", y, x_hit, z);
    }
}

/* Read the Z buffer at a fixed X column across sampled rows. Used to check
 * whether virge_clear_z actually wrote 0xFFFF to every row: the compare uses
 * the S16.15 integer part (Zs compares as 0 for z<1), so LESS fails wherever
 * Zzb==0 -- which is what cuts the triangle at a Z-buffer content boundary. */
static void dump_z_coverage(const char *label, const uint8_t *zram,
                            uint32_t stride, int H)
{
    static const int ys[] = {0, 100, 200, 230, 234, 238, 300, 450, 599};
    printf("  %s (x=400):\n", label);
    for (size_t i = 0; i < sizeof(ys) / sizeof(ys[0]); i++) {
        int y = ys[i];
        if (y >= H) continue;
        uint16_t z = read_px(zram, stride, 400, y);
        printf("    y=%3d Z=0x%04x%s\n", y, z,
               z == 0xFFFF ? "" :
               (z == 0 ? "  <-- ZERO (uncleared)" : "  <-- not 0xFFFF"));
    }
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
    uint8_t *zram = vram + vctx.z_base;

    printf("\n3D triangle Z-mode matrix: requested %dx%d -> adopted %dx%d "
           "stride %u, fb 0x%x z 0x%x\n", req_w, req_h, W, H, stride,
           vctx.fb_base, vctx.z_base);
    uint32_t clip_tb = virge_read32(&vctx, VIRGE_3D_CLIP_T_B);
    uint32_t clip_lr = virge_read32(&vctx, VIRGE_3D_CLIP_L_R);
    printf("3D clip readback: right=%u bottom=%u (L_R=0x%x T_B=0x%x)\n",
           clip_lr & 0x7FF, clip_tb & 0x7FF, clip_lr, clip_tb);

    /* Fable's decisive check: does virge_clear_z reach every row? Read Z right
     * after clear_z (before any fill/draw), then again after the FB fill, to
     * see if the clear covers all rows and whether fill_rect disturbs Z. */
    printf("\nZ-clear coverage probe (0xFFFF=cleared far, 0=uncleared):\n");
    cpu_clear_fb(&vctx, W, H, stride);
    virge_clear_z(&vctx, 1.0f);
    virge_wait_engine(&vctx);
    dump_z_coverage("after clear_z            ", zram, stride, H);
    virge_fill_rect(&vctx, 0, 0, W, H, 0);
    virge_wait_engine(&vctx);
    dump_z_coverage("after clear_z + fill_rect", zram, stride, H);

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

    printf("\nResult: LESS cuts the triangle below ~y=234 while LEQUAL and ALWAYS\n");
    printf("  pass and NEVER rejects all. The Z-buffer profile under LESS (next)\n");
    printf("  shows the actual Zs written per row: values climbing toward 0xFFFF\n");
    printf("  mean Zs is drifting up; a flat ~0x4000 means the compare itself is\n");
    printf("  the problem.\n");

    /* Z-buffer profile under LESS: the Z buffer holds the written Zs where the
     * triangle passed and 0xFFFF where it failed (cleared, not overwritten). */
    virge_wait_engine(&vctx);
    cpu_clear_fb(&vctx, W, H, stride);
    virge_clear_z(&vctx, 1.0f);
    virge_fill_rect(&vctx, 0, 0, W, H, 0);
    vctx.z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_LESS | VIRGE_ZUP_ENABLE;
    draw_demo_triangle(&vctx, W, H);
    virge_wait_engine(&vctx);
    printf("\nZ-buffer profile under LESS (TZS should be 0x4000; cleared far=0xFFFF):\n");
    dump_z_profile(W, H, stride, vram, zram);

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
