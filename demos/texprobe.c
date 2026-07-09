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

    /* ---- TEST 4: WHERE does the engine read? TEX_BASE=0x2bf200 is confirmed
     * (reads back) and our texture is really there (O_SYNC aperture, matches
     * scantest), yet the engine samples 0x3436 from elsewhere. Two sub-tests:
     *  (a) VRAM scan for 0x3436 -> find the offset(s) holding the value the
     *      engine actually reads (its true texture-fetch site).
     *  (b) TEX_BASE marker sweep incl. a LOW address -> does the engine honor
     *      TEX_BASE at all, and is texture fetch banked/range-limited (only
     *      low VRAM)? We CPU-fill each candidate region (uncached O_SYNC =>
     *      really in VRAM) with a distinct marker, override hw->tex_base, and
     *      draw UV=(0,0). If the engine honors TEX_BASE, center == that base's
     *      marker; if all read 0x3436, TEX_BASE is ignored. Back-buffer
     *      (0xea600) is NOT cleared by l10gl_clear (only fb_base=0 + Z), so its
     *      marker survives. */
    /* (a) VRAM scan for 0x3436 */
    {
        uint16_t *vram = (uint16_t *)hw->fb;
        uint32_t total = hw->vram_size / 2;
        uint32_t hits = 0, first[8]; size_t nf = 0;
        uint32_t cur_run = 0, cur_at = 0, best_run = 0, best_at = 0;
        for (uint32_t i = 0; i < total; i++) {
            if (vram[i] == 0x3436) {
                hits++;
                if (nf < 8) first[nf++] = i * 2;
                if (cur_run == 0) cur_at = i * 2;
                cur_run++;
            } else {
                if (cur_run > best_run) { best_run = cur_run; best_at = cur_at; }
                cur_run = 0;
            }
        }
        if (cur_run > best_run) { best_run = cur_run; best_at = cur_at; }
        printf("\nTEST 4a: VRAM scan for 0x3436 (engine's constant output)\n");
        printf("  %u hits in %u texels; first byte offsets:", hits, total);
        for (size_t i = 0; i < nf; i++) printf(" 0x%x", first[i]);
        printf("\n  longest contiguous run: %u bytes starting at byte 0x%x\n",
               best_run * 2, best_at);
    }
    /* (b) TEX_BASE marker sweep */
    {
        l10gl_bind_texture(&ctx, &tex);     /* restore cmd bits (ARGB1555,s=6) */
        l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);
        struct { uint32_t base; uint16_t mark; const char *name; } mb[] = {
            { 0x000ea600, 0xF800, "backbuf LOW (0xea600)" },
            { 0x002bf200, 0x07E0, "tex-heap (0x2bf200)" },
            { 0x002d0000, 0x001F, "offscreen (0x2d0000)" },
            { 0x00300000, 0xFFE0, "high (0x300000)" },
        };
        printf("\nTEST 4b: TEX_BASE marker sweep (CPU-fill region, override TEX_BASE, draw UV=0,0)\n");
        float uv00[4][2] = { {0,0},{0,0},{0,0},{0,0} };
        for (int i = 0; i < 4; i++) {
            uint16_t *r = (uint16_t *)(hw->fb + mb[i].base);
            for (int j = 0; j < 4096; j++) r[j] = mb[i].mark;   /* 8 KB solid */
            hw->tex_base = mb[i].base;          /* program_3d_state re-arms this */
            l10gl_clear(&ctx);                   /* clears fb_base=0 + Z only */
            draw_quad(&ctx, x0, y0, x1, y1, zv, uv00);
            int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
            uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
            printf("  TEX_BASE=0x%06x mark=0x%04x %-22s center=0x%04x  %s\n",
                   mb[i].base, mb[i].mark, mb[i].name, px,
                   (px == mb[i].mark) ? "<-- HONORS TEX_BASE" : "(not marker)");
        }
        hw->tex_base = (uint32_t)(uintptr_t)tex.backend_data;   /* restore */
    }

    /* ---- TEST 5: address-encoding fill -> exactly WHERE does the engine read?
     * TEST 4b showed the engine ignores TEX_BASE (markers invisible at every
     * base, incl. low), and TEST 4a showed it still tiles (deltas applied) ->
     * it reads a FIXED base. To find it, fill ALL of VRAM so each 16-bit texel
     * encodes its own byte offset (low 15 bits in R/G/B, alpha=1 so it isn't
     * treated as transparent), then draw UV=(0,0). The center pixel's R/G/B
     * reveals the byte offset the engine actually sampled. Repeat for two
     * TEX_BASE values: if the decoded offset is the SAME for both, the base is
     * truly fixed (independent of TEX_BASE); if it tracks TEX_BASE, the engine
     * does honor TEX_BASE and the earlier marker result needs re-reading.
     * NOTE: 15 bits -> offset known mod 64KB; enough to tell base 0 (decodes
     * 0) from TEX_BASE 0x2bf200 (decodes 0xe600) from any other fixed base. */
    {
        l10gl_bind_texture(&ctx, &tex);
        l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);
        uint32_t bases[] = { 0x2bf200, 0x000000 };
        float uv00[4][2] = { {0,0},{0,0},{0,0},{0,0} };
        printf("\nTEST 5: address-encoding fill (each texel = its byte offset); draw UV=0,0\n");
        for (int b = 0; b < 2; b++) {
            l10gl_clear(&ctx);
            uint16_t *v = (uint16_t *)hw->fb;
            uint32_t n = hw->vram_size / 2;
            for (uint32_t i = 0; i < n; i++) v[i] = (uint16_t)(0x8000 | (i & 0x7FFF));
            hw->tex_base = bases[b];
            draw_quad(&ctx, x0, y0, x1, y1, zv, uv00);
            int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
            uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
            int r = (px >> 10) & 0x1F, g = (px >> 5) & 0x1F, pp = px & 0x1F;
            uint32_t dec = ((uint32_t)r << 10) | ((uint32_t)g << 5) | pp;  /* texel idx, low 15b */
            printf("  TEX_BASE=0x%06x -> center=0x%04x R/G/B=%d/%d/%d -> sampled texel=0x%x (byte 0x%x, mod 64KB)\n",
                   bases[b], px, r, g, pp, dec, dec * 2);
        }
        hw->tex_base = (uint32_t)(uintptr_t)tex.backend_data;
    }

    /* ---- TEST 6: is the constant 0x3436 READ FROM VRAM, or SYNTHESIZED?
     * TEST 5 "decoded" the center 0x3436 to byte 0x686C -- but 0x3436 has been
     * the engine's output in EVERY test (1/2/3/4b/5), so the decode is only
     * meaningful if the engine truly samples a VRAM texel. If instead it emits
     * a register default (texture border / fog / chroma color), the "decode"
     * was a coincidence. Discriminate two ways:
     *   (a)/(b) Fill ALL of VRAM with one uniform value (black, then white)
     *       and draw UV=(0,0). If the engine reads ANY texel, the quad takes
     *       that color (R=0 black / R=31 white); if it still emits 0x3436
     *       (R=13), no VRAM texel is sampled -> synthesized constant.
     *   (c) If synthesized, program each 3D color register to white one at a
     *       time over a black fill; whichever flips the output to white is the
     *       source of the constant. None of these regs is written by the draw
     *       path (grep-verified), so a write after l10gl_clear survives. */
    {
        l10gl_bind_texture(&ctx, &tex);
        l10gl_tex_parameter(&ctx, L10GL_FILTER_NEAREST, L10GL_WRAP_CLAMP);
        float uv00[4][2] = { {0,0},{0,0},{0,0},{0,0} };
        int mx = (x0 + x1) / 2, my = (y0 + y1) / 2;
        uint32_t n = hw->vram_size / 2;
        uint16_t *vram = (uint16_t *)hw->fb;

        printf("\nTEST 6: is the constant 0x3436 read from VRAM, or synthesized?\n");
        uint16_t  fills[2] = { 0x8000, 0xBFFF };     /* black, then white; alpha=1 */
        const char *fn[2]  = { "black 0x8000", "white 0xBFFF" };
        int       expR[2]  = { 0, 31 };
        for (int f = 0; f < 2; f++) {
            l10gl_clear(&ctx);
            for (uint32_t i = 0; i < n; i++) vram[i] = fills[f];
            hw->tex_base = 0x002bf200;
            draw_quad(&ctx, x0, y0, x1, y1, zv, uv00);
            uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
            int r = (px >> 10) & 0x1F;
            printf("  6%c fill %-13s center=0x%04x R=%-2d  %s\n", 'a' + f, fn[f], px, r,
                   (r == expR[f]) ? "<-- FILLS THE QUAD => engine READS VRAM"
                                  : (r == 13)  ? "still 0x3436 => SYNTHESIZED (no VRAM read)"
                                               : "(unexpected)");
        }

        /* (c) which register sources the synthesized constant? */
        struct { const char *name; uint32_t off; } cregs[] = {
            { "TEX_BDR_CLR 0xB4F0", 0xB4F0 }, { "FOG_CLR 0xB4F4", 0xB4F4 },
            { "COLOR0 0xB4F8",      0xB4F8 }, { "COLOR1 0xB4FC", 0xB4FC },
        };
        printf("  6c color-register sweep (black fill; one reg -> white 0x7FFF):\n");
        for (int k = 0; k < 4; k++) {
            l10gl_clear(&ctx);
            for (uint32_t i = 0; i < n; i++) vram[i] = 0x8000;   /* black */
            virge_write32(hw, cregs[k].off, 0x00007FFF);          /* white into one reg */
            hw->tex_base = 0x002bf200;
            draw_quad(&ctx, x0, y0, x1, y1, zv, uv00);
            uint16_t px = *(uint16_t *)(base + (size_t)my * stride + (size_t)mx * 2);
            printf("     %-19s -> center=0x%04x  %s\n", cregs[k].name, px,
                   (px == 0x7FFF) ? "<-- THIS reg sources 0x3436" : "(no change)");
            virge_write32(hw, cregs[k].off, 0x00000000);          /* restore */
        }
        hw->tex_base = (uint32_t)(uintptr_t)tex.backend_data;
    }

    printf("\nDone. Ctrl-C to exit.\n");
    while (running) { if (getchar() == EOF) break; }
    l10gl_destroy(&ctx);
    return 0;
}
