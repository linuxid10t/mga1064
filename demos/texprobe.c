/*
 * texprobe.c - readback probe for the textured-triangle path.
 *
 * textured_cube was the one demo never silicon-verified (HANDOFF only ever
 * confirmed it animates/swaps, not that texture mapping is correct). The
 * first run showed U,V are DEAD-CONSTANT across a face-on quad (R pinned at
 * 13, G at ~1-2) instead of interpolating 0..31 -- the texture gradients
 * aren't taking effect. Addresses/format/s/command are all datasheet-correct,
 * so this v2 localizes further:
 *
 *   TEST 1 - gradient quad (UV 0,0 -> 1,1): full R/G grids (as v1).
 *   REG dump - read back the ACTUAL programmed TUS/TVS/TWS/Td*dX/Td*dY/
 *              TEX_BASE/CMD_SET via virge_read32 (ground truth; some 3D
 *              regs are write-only, so expect 0/0xffffffff on those).
 *   TEST 2 - two CONSTANT-UV quads (all four verts uv=(0.5,0.5) then
 *              (0.9,0.1), so du=dv=0). If the START register works the quad
 *              reads that one texel's color; if it still reads ~(13,2) the
 *              start / perspective W-divide is what's broken, not the deltas.
 *
 * Drives the real frontend API and reaches struct virge_ctx via
 * ctx.backend_data (hw is the first field of virge_private) for readback.
 *
 * Build: make -B BACKEND=virge texprobe     Run: sudo ./texprobe
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>

#include "l10gl.h"
#include "backends/virge/virge.h"   /* struct virge_ctx + virge_read32 */

#define TEX 64

static volatile sig_atomic_t running = 1;
static void sighandler(int s) { (void)s; running = 0; }

/* Expected 5-bit channel under model A (normalized UV): texcol=floor(u*TEX),
 * clamped to [0,TEX-1]; chan = texcol>>1. */
static int exp_chan(int screen, int lo, int hi)
{
    int c = (int)((float)(screen - lo) / (float)(hi - lo) * TEX);
    if (c >= TEX) c = TEX - 1;
    if (c < 0) c = 0;
    return c >> 1;
}

/* texel(x,y) = opaque ARGB1555, R=x>>1, G=y>>1, B=1 (non-zero floor). */
static void gen_uv_gradient(uint16_t *tex, int size)
{
    for (int y = 0; y < size; y++)
        for (int x = 0; x < size; x++)
            tex[y * size + x] = (uint16_t)(0x8000 | ((x >> 1) << 10) | ((y >> 1) << 5) | 1);
}

/* Draw a face-on quad [x0,y0]-[x1,y1] with per-corner UVs, constant z,w. */
static void draw_quad(struct l10gl_ctx *ctx, int x0, int y0, int x1, int y1,
                      float zv, const float uv[4][2])
{
    float w = 1.0f;
    struct l10gl_vertex TL = { (float)x0,(float)y0, zv,w, 1,1,1,1, uv[0][0],uv[0][1] };
    struct l10gl_vertex TR = { (float)x1,(float)y0, zv,w, 1,1,1,1, uv[1][0],uv[1][1] };
    struct l10gl_vertex BR = { (float)x1,(float)y1, zv,w, 1,1,1,1, uv[2][0],uv[2][1] };
    struct l10gl_vertex BL = { (float)x0,(float)y1, zv,w, 1,1,1,1, uv[3][0],uv[3][1] };
    l10gl_draw_textured_triangle(ctx, TL, TR, BR);
    l10gl_draw_textured_triangle(ctx, TL, BR, BL);
    l10gl_wait_engine(ctx);
}

static void rg_grid(const char *title, uint8_t *base, uint32_t stride,
                    int x0, int y0, int x1, int y1)
{
    printf("\n%s\n", title);
    printf("ACTUAL R / EXPECTED R  (expect left->right rise 0..31):\n   ");
    for (int x = x0; x <= x1; x += 50) printf("  x%-3d", x);
    printf("\n");
    for (int y = y0; y <= y1; y += 50) {
        printf("y%-3d", y);
        for (int x = x0; x <= x1; x += 50) {
            uint16_t px = *(uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
            printf("  %2d/%-2d", (px >> 10) & 0x1F, exp_chan(x, x0, x1));
        }
        printf("\n");
    }
    printf("ACTUAL G / EXPECTED G  (expect top->bottom rise 0..31):\n   ");
    for (int x = x0; x <= x1; x += 50) printf("  x%-3d", x);
    printf("\n");
    for (int y = y0; y <= y1; y += 50) {
        printf("y%-3d", y);
        for (int x = x0; x <= x1; x += 50) {
            uint16_t px = *(uint16_t *)(base + (size_t)y * stride + (size_t)x * 2);
            printf("  %2d/%-2d", (px >> 5) & 0x1F, exp_chan(y, y0, y1));
        }
        printf("\n");
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    struct l10gl_ctx ctx;

#ifdef BACKEND_VIRGE
    const struct l10gl_backend *backend = &virge_backend;
#else
    fprintf(stderr, "texprobe requires BACKEND=virge\n");
    return 1;
#endif
    if (!(backend->caps & L10GL_CAP_TEXTURE)) {
        fprintf(stderr, "backend advertises no texture cap\n");
        return 1;
    }
    if (l10gl_create(&ctx, backend, 800, 600, 2) < 0) {
        fprintf(stderr, "l10gl_create failed\n");
        return 1;
    }
    int W = ctx.width, H = ctx.height;

    struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = sighandler;
    sigaction(SIGINT, &sa, NULL); sigaction(SIGTERM, &sa, NULL);

    struct l10gl_texture tex;
    uint16_t texmem[TEX * TEX];
    gen_uv_gradient(texmem, TEX);
    if (l10gl_tex_image_2d(&ctx, &tex, TEX, TEX, L10GL_TEX_FMT_ARGB1555, texmem) < 0) {
        fprintf(stderr, "texture upload failed\n");
        return 1;
    }
    l10gl_bind_texture(&ctx, &tex);
    l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);

    l10gl_clear_color(&ctx, 0.0f, 0.0f, 0.0f);
    l10gl_clear_depth(&ctx, 1.0f);
    l10gl_depth_func(&ctx, L10GL_LESS);

    struct virge_ctx *hw = (struct virge_ctx *)ctx.backend_data;
    uint8_t *base = (uint8_t *)hw->fb + hw->fb_base;
    uint32_t stride = hw->stride;

    int x0 = 200, y0 = 150, x1 = 600, y1 = 450;
    float zv = 0.5f;
    int s_val = (hw->tex_cmd_bits >> 8) & 0xF;

    printf("\ntexprobe v2 (%dx%d stride %u)  quad TL(%d,%d) BR(%d,%d) z=%.2f\n",
           W, H, stride, x0, y0, x1, y1, zv);
    printf("texture %dx%d ARGB1555 NEAREST CLAMP; tex encodes R=x>>1,G=y>>1.\n", TEX, TEX);
    printf("cached tex_cmd_bits=0x%08x (s field bits11-8 = %d -> UV format S%u.%u)\n",
           hw->tex_cmd_bits, s_val, 4 + s_val, 27 - s_val);

    /* ---- TEX VRAM readback: is the uploaded texture actually at TEX_BASE? ----
     * tex->backend_data holds the absolute VRAM offset the bump allocator
     * assigned (== the value written to VIRGE_3D_TEX_BASE). CPU-read it back
     * straight from the linear aperture and compare to what gen_uv_gradient
     * wrote. If these MISMATCH, the upload never landed in engine-visible
     * VRAM (aperture/write-path bug). If they MATCH but the rendered quad
     * below still isn't our texture, the engine reads ELSEWHERE (TEX_BASE
     * write ignored / overridden, or texel addressing off-texture). */
#define TEXEL(x,y) ((uint16_t)(0x8000 | (((x)>>1)<<10) | (((y)>>1)<<5) | 1))
    uint32_t tex_addr = (uint32_t)(uintptr_t)tex.backend_data;
    uint16_t *tvram = (uint16_t *)(hw->fb + tex_addr);
    printf("\nTEX upload readback: tex_addr (== TEX_BASE written) = 0x%x\n", tex_addr);
    printf("  dumping VRAM at that offset (expect the gen_uv_gradient texels):\n");
    struct { int idx, x, y; uint16_t exp; } tchk[] = {
        { 0,           0,  0,  TEXEL(0,0)   },
        { 2,           2,  0,  TEXEL(2,0)   },
        { 63,         63,  0,  TEXEL(63,0)  },
        { 64,          0,  1,  TEXEL(0,1)   },
        { 32*64+32,   32, 32,  TEXEL(32,32) },
    };
    for (int i = 0; i < 5; i++)
        printf("  texel(%2d,%2d) idx %4d = 0x%04x  (expect 0x%04x) %s\n",
               tchk[i].x, tchk[i].y, tchk[i].idx, tvram[tchk[i].idx], tchk[i].exp,
               tvram[tchk[i].idx] == tchk[i].exp ? "OK" : "*** MISMATCH ***");
#undef TEXEL

    /* ---- TEST 1: gradient quad ---- */
    l10gl_clear(&ctx);
    float uv_grad[4][2] = { {0,0},{1,0},{1,1},{0,1} };
    draw_quad(&ctx, x0, y0, x1, y1, zv, uv_grad);
    rg_grid("TEST 1: gradient quad (UV 0,0 -> 1,1)", base, stride, x0, y0, x1, y1);

    /* ---- REG dump: actual programmed registers (best-effort readback) ---- */
    printf("\nREG dump (read back via virge_read32 after TEST 1; write-only regs "
           "may read 0 or 0xffffffff):\n");
    struct { const char *name; uint32_t off; } regs[] = {
        { "TEX_BASE 0xB4EC", 0xB4EC }, { "CMD_SET  0xB500", 0xB500 },
        { "TdWdX   0xB50C", 0xB50C }, { "TdWdY   0xB510", 0xB510 },
        { "TWS     0xB514", 0xB514 }, { "TdDdX   0xB518", 0xB518 },
        { "TdVdX   0xB51C", 0xB51C }, { "TdUdX   0xB520", 0xB520 },
        { "TdDdY   0xB524", 0xB524 }, { "TdVdY   0xB528", 0xB528 },
        { "TdUdY   0xB52C", 0xB52C }, { "TDS     0xB530", 0xB530 },
        { "TVS     0xB534", 0xB534 }, { "TUS     0xB538", 0xB538 },
    };
    for (size_t i = 0; i < sizeof(regs)/sizeof(regs[0]); i++)
        printf("  %-16s = 0x%08x\n", regs[i].name, virge_read32(hw, regs[i].off));

    /* ---- TEST 2: constant-UV quads (du=dv=0; isolates the START register) ---- */
    float uv_c1[4][2] = { {0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f},{0.5f,0.5f} };
    float uv_c2[4][2] = { {0.9f,0.1f},{0.9f,0.1f},{0.9f,0.1f},{0.9f,0.1f} };
    struct { const char *name; const float (*uv)[2]; int expR, expG; } ctests[] = {
        { "TEST 2a: constant UV=(0.5,0.5) -> expect texel(32,32) R16 G16", uv_c1, 16, 16 },
        { "TEST 2b: constant UV=(0.9,0.1) -> expect texel(57,6)  R28 G3",  uv_c2, 28, 3  },
    };
    for (int t = 0; t < 2; t++) {
        l10gl_clear(&ctx);
        draw_quad(&ctx, x0, y0, x1, y1, zv, ctests[t].uv);
        int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
        int r = (px >> 10) & 0x1F, g = (px >> 5) & 0x1F;
        printf("\n%s\n  center px=0x%04x  R=%d G=%d  %s\n",
               ctests[t].name, px, r, g,
               (r == ctests[t].expR && g == ctests[t].expG)
               ? "MATCH (start register works)" : "MISMATCH (start/W-divide broken)");
    }

    /* ---- TEST 3: solid-color texture (does the engine read OUR texture at all?)
     * Upload a texture where EVERY texel is solid red (0xFC00: a=1,R=31,G=0,B=0)
     * and draw the gradient-UV quad over it.
     *   center R=31 (red)  -> engine IS sampling our texture memory => the
     *                         dead-constant/UV bug is purely the coord pipeline.
     *   center still ~(13,1)=0x3436 -> engine is NOT reading our texture at all
     *                         => TEX_BASE write ignored/overridden, or texel
     *                         addressing reads off-texture. (Root cause.) */
    {
        struct l10gl_texture stex;
        uint16_t solid[TEX * TEX];
        for (int i = 0; i < TEX * TEX; i++) solid[i] = 0xFC00;
        if (l10gl_tex_image_2d(&ctx, &stex, TEX, TEX, L10GL_TEX_FMT_ARGB1555, solid) == 0) {
            l10gl_bind_texture(&ctx, &stex);
            l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);
            l10gl_clear(&ctx);
            draw_quad(&ctx, x0, y0, x1, y1, zv, uv_grad);
            int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
            uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
            int r = (px >> 10) & 0x1F, g = (px >> 5) & 0x1F;
            uint32_t saddr = (uint32_t)(uintptr_t)stex.backend_data;
            uint16_t *svram = (uint16_t *)(hw->fb + saddr);
            printf("\nTEST 3: solid RED texture (0xFC00) under gradient UV\n");
            printf("  solid tex_addr=0x%x  VRAM[0]=0x%04x (expect 0xfc00) %s\n",
                   saddr, svram[0], svram[0] == 0xFC00 ? "OK" : "*** upload mismatch ***");
            printf("  center px=0x%04x  R=%d G=%d  %s\n", px, r, g,
                   (r == 31) ? "RED -> engine reads our texture (UV-only bug)"
                             : "NOT red -> engine NOT reading our texture (TEX_BASE/upload bug)");
        }
    }

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    l10gl_destroy(&ctx);
    return 0;
}
