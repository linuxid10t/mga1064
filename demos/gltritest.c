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

/* Draw a full-screen rectangle (two triangles) under ALWAYS+ZUP at a known
 * depth, then scan VRAM for the Zs word. For a full-screen rect, first hit =
 * the 3D Z base and (last-first-(W-1)*2)/(H-1) = the 3D Z row stride --
 * precisely, regardless of edge-inclusion fuzz. Compare to the programmed
 * Z_BASE / Z_STRIDE to pin the addressing bug. */
static void probe_z_layout(struct virge_ctx *ctx, int W, int H, uint32_t stride_fb,
                           uint8_t *vram, uint32_t vram_size, uint16_t target,
                           uint32_t prog_zb, uint32_t prog_zs)
{
    virge_wait_engine(ctx);
    cpu_clear_fb(ctx, W, H, stride_fb);
    virge_clear_z(ctx, 1.0f);
    virge_fill_rect(ctx, 0, 0, W, H, 0);
    ctx->z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_ALWAYS | VIRGE_ZUP_ENABLE;
    struct virge_vertex v00 = {.x=0,   .y=0,   .z=0.7f,.w=1,.r=1,.g=1,.b=1,.a=1};
    struct virge_vertex vW0 = {.x=W-1, .y=0,   .z=0.7f,.w=1,.r=1,.g=1,.b=1,.a=1};
    struct virge_vertex vWH = {.x=W-1, .y=H-1, .z=0.7f,.w=1,.r=1,.g=1,.b=1,.a=1};
    struct virge_vertex v0H = {.x=0,   .y=H-1, .z=0.7f,.w=1,.r=1,.g=1,.b=1,.a=1};
    virge_draw_triangle_gouraud(ctx, v00, vW0, vWH);
    virge_draw_triangle_gouraud(ctx, v00, vWH, v0H);
    virge_wait_engine(ctx);

    long total = 0;
    uint32_t first = 0, last = 0;
    int have = 0;
    for (uint32_t off = 0; off + 1 < vram_size; off += 2) {
        if (*(const uint16_t *)(vram + off) == target) {
            if (!have) { first = off; have = 1; }
            last = off;
            total++;
        }
    }
    uint32_t s_meas = (last > first + (uint32_t)(W - 1) * 2)
                      ? (last - first - (uint32_t)(W - 1) * 2) / (uint32_t)(H - 1)
                      : 0;
    printf("  measured Z stride=%-5u (programmed %u), base@0x%06x (%ld hits) %s\n",
           s_meas, prog_zs, first, total,
           (s_meas == prog_zs && first == prog_zb) ? "[OK]" : "[MISMATCH]");
    (void)target;
}

/* Run one LESS draw of the demo triangle with Z relocated to zb; return maxy.
 * Reprograms VIRGE_3D_Z_BASE, clears the new Z region, draws under LESS. */
static int run_less_at_zbase(struct virge_ctx *ctx, int W, int H, uint32_t stride,
                             uint8_t *vram, uint32_t zb)
{
    ctx->z_base = zb;
    virge_write32(ctx, VIRGE_3D_Z_BASE, zb & ~0x7);
    virge_wait_engine(ctx);
    cpu_clear_fb(ctx, W, H, stride);
    virge_clear_z(ctx, 1.0f);
    virge_fill_rect(ctx, 0, 0, W, H, 0);
    ctx->z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_LESS | VIRGE_ZUP_ENABLE;
    draw_demo_triangle(ctx, W, H);
    virge_wait_engine(ctx);
    int maxy = -1;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++)
            if (read_px(vram, stride, x, y))
                if (y > maxy) maxy = y;
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
    uint32_t orig_zbase = vctx.z_base;

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

    /* With the VIRGE_Z_FIXED scale fix, re-examine the ZBC_LESS row above: if
     * it's now FULL, the scale was the whole bug. If still CUT/EMPTY, the 3D Z
     * unit isn't fetching/writing the CPU-visible z_base region -- the 4MB
     * scan and Z_BASE toggle below diagnose that. */

    /* Z-layout probe: full-screen rect under ALWAYS+ZUP at z=0.7 writes the
     * Zs word to every pixel; measure the 3D Z unit's actual base and row
     * stride vs the programmed Z_BASE/Z_STRIDE. The demo-triangle 4MB scan
     * already showed writes at 0x1364d2..0x30e7f8 (~2MB span, ~2x expected),
     * implying base ok but stride ~2.5x; this pins the exact stride. */
    printf("\nZ-layout probe (full-screen rect, ALWAYS+ZUP @ z=0.7):\n");
    {
        uint16_t target = (uint16_t)((VIRGE_Z_FIXED(0.7f) >> 15) & 0xFFFF);
        printf("  expected Zs word = 0x%04x; programmed Z_BASE=0x%06x Z_STRIDE=%u\n",
               target, orig_zbase, W * 2);
        probe_z_layout(&vctx, W, H, stride, vram, vctx.vram_size, target,
                       orig_zbase, W * 2);
        printf("  -> [OK] means the per-draw re-arm holds (base 0x%06x + stride %u):\n"
               "     the Z fetch now matches the 2D clear, so the matrix LESS row\n"
               "     should be FULL and the triangle/cube should render completely.\n",
               orig_zbase, W * 2);
    }

    /* Z_BASE toggle: the default Z region 0xEA600-0x1D4C00 crosses the 1MB
     * boundary (0x100000) -- a plausible DX-silicon wrap point the 2D engine
     * and CPU handle but the 3D Z port might not. Move Z to 0x200000 (clear)
     * and re-run LESS; if the cutoff moves/vanishes, Z-fetch addressing is
     * the bug. */
    printf("\nZ_BASE toggle (LESS, fixed scale; full triangle reaches ~%d):\n",
           (int)(0.917f * H));
    {
        int m_def = run_less_at_zbase(&vctx, W, H, stride, vram, orig_zbase);
        int m_2mb = run_less_at_zbase(&vctx, W, H, stride, vram, 0x200000);
        vctx.z_base = orig_zbase;
        virge_write32(&vctx, VIRGE_3D_Z_BASE, orig_zbase & ~0x7);
        printf("  Z_BASE=0x%06x (default, crosses 1MB): LESS maxy=%d\n", orig_zbase, m_def);
        printf("  Z_BASE=0x200000 (clear of 1MB):       LESS maxy=%d\n", m_2mb);
        if (m_2mb > m_def + 50)
            printf("  -> cutoff moved at 0x200000: Z-fetch addressing bug (1MB wrap?).\n");
        else if (m_2mb == m_def)
            printf("  -> same cutoff: walk-relative, internal to the span engine.\n");
        else
            printf("  -> cutoff changed (def=%d, 2mb=%d): addressing partly involved.\n",
                   m_def, m_2mb);
    }

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
