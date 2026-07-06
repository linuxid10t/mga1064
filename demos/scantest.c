/*
 * scantest.c - Scanout diagnosis + takeover experiment for the S3 ViRGE.
 *
 * Built for the machine where /dev/fb0 does not exist: no kernel fb
 * driver is bound to the card, so the chip scans out whatever mode the
 * bootloader's VBE call left behind. virge_init's CRTC truth dump read
 * 800x600 active, CR67 Mode 13 (24-bit), LSW=400 — but the CR13 byte
 * multiplier (x2/x4/x8) and the real pixel packing (packed 24 vs 32-bit
 * dwords) can't be settled from registers alone. This tool settles both
 * visually, then — on explicit keypress — takes over the scanout format
 * so the rest of the driver finally has a display it can be correct on.
 *
 * PHASE 1 (no register writes): CPU-draws three test strips into VRAM
 * over BAR0, each betting on a different scanout layout:
 *
 *      strip A at VRAM 0      : 32-bit pixels, pitch 3200
 *      strip B at VRAM 320000 : packed 24-bit, pitch 2400
 *      strip C at VRAM 640000 : 16-bit RGB555, pitch 1600
 *
 *   Exactly ONE strip will show clean solid RED/GREEN/BLUE/WHITE bands
 *   inside a crisp white outline — that one names the actual layout.
 *   The other two appear as striped noise. (All three offsets land
 *   on-screen under any of the candidate pitches.)
 *
 * PHASE 2 (press Enter; writes CR67/CR13/CR51 with save+restore):
 *   During vertical retrace, switch CR67 bits 7-4 to Mode 9 (15-bit
 *   555, DB019-B PDF p.216) and program the pitch for 1600 bytes
 *   (LSW=400 under the doubleword x4 rule — CR31 bit 3 is set), then
 *   draw a full-screen 800x600 555 pattern at pitch 1600. If the
 *   pattern shows clean (border hugging all edges, solid bands), the
 *   takeover recipe is proven and virge_init can adopt it. If it is
 *   sheared exactly 2:1, the multiplier is quadword (x8): press Enter
 *   again to toggle LSW between 400 and 200 and note which is clean.
 *   Ctrl-C restores the original CR67/CR13/CR51 and exits.
 *
 * Build: make scantest       (always builds against the virge backend)
 * Run:   sudo ./scantest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <errno.h>

#include "backends/virge/virge.h"

static volatile sig_atomic_t running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

/* Write one pixel at (x, row) into VRAM at base+row*pitch, in the given
 * packing. color is 00RRGGBB. */
static void put_px(uint8_t *vram, uint32_t base, uint32_t pitch,
                   int bytes_pp, int x, int row, uint32_t color)
{
    uint8_t *p = vram + base + (size_t)row * pitch + (size_t)x * bytes_pp;
    uint8_t r = color >> 16, g = color >> 8, b = color;
    switch (bytes_pp) {
    case 2: {  /* RGB555 */
        uint16_t v = (uint16_t)(((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3));
        p[0] = (uint8_t)v;
        p[1] = (uint8_t)(v >> 8);
        break;
    }
    case 3:    /* packed RGB888, B first in memory */
        p[0] = b; p[1] = g; p[2] = r;
        break;
    case 4:    /* XRGB8888 */
        p[0] = b; p[1] = g; p[2] = r; p[3] = 0;
        break;
    }
}

static void fill_rows(uint8_t *vram, uint32_t base, uint32_t pitch,
                      int bytes_pp, int w, int row0, int nrows,
                      uint32_t color)
{
    for (int row = row0; row < row0 + nrows; row++)
        for (int x = 0; x < w; x++)
            put_px(vram, base, pitch, bytes_pp, x, row, color);
}

/* One hypothesis strip: 1px white outline around w x nrows, RED/GREEN/
 * BLUE/WHITE bands inside. Coherent on screen iff (pitch, bytes_pp)
 * match the real scanout layout. */
static void draw_strip(uint8_t *vram, uint32_t base, uint32_t pitch,
                       int bytes_pp, int w, int nrows)
{
    static const uint32_t bands[4] =
        { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFFFF };
    int band_h = (nrows - 2) / 4;

    for (int i = 0; i < 4; i++)
        fill_rows(vram, base, pitch, bytes_pp, w,
                  1 + i * band_h, band_h, bands[i]);

    fill_rows(vram, base, pitch, bytes_pp, w, 0, 1, 0xFFFFFF);
    fill_rows(vram, base, pitch, bytes_pp, w, nrows - 1, 1, 0xFFFFFF);
    for (int row = 0; row < nrows; row++) {
        put_px(vram, base, pitch, bytes_pp, 0, row, 0xFFFFFF);
        put_px(vram, base, pitch, bytes_pp, w - 1, row, 0xFFFFFF);
    }
}

/* Full-screen phase-2 pattern: 800x600 RGB555 at pitch 1600. Border,
 * corner markers (1=TL 2=TR 3=BL 4=BR), R/G/B/W bands, then the two
 * raw discriminator bands (0x03E0 = green iff 555, 0x07E0 = green iff
 * 565) as final confirmation that scanout really decodes 555 now. */
static void draw_555_screen(uint8_t *vram, int w, int h, uint32_t pitch)
{
    for (int row = 0; row < h; row++)
        memset(vram + (size_t)row * pitch, 0, (size_t)w * 2);

    struct { uint16_t px; } bands[6] = {
        { (31 << 10) },            /* red   */
        { (31 << 5)  },            /* green */
        { 31 },                    /* blue  */
        { 0x7FFF },                /* white */
        { 0x03E0 },                /* raw: green iff scanout is 555 */
        { 0x07E0 },                /* raw: green iff scanout is 565 */
    };
    int margin = 16;
    int band_h = (h - 2 * margin) / 6;
    for (int i = 0; i < 6; i++) {
        for (int row = margin + i * band_h;
             row < margin + i * band_h + band_h - 4; row++) {
            uint16_t *line = (uint16_t *)(vram + (size_t)row * pitch);
            for (int x = margin; x < w - margin; x++)
                line[x] = bands[i].px;
        }
    }

    /* 1px white border */
    uint16_t white = 0x7FFF;
    for (int x = 0; x < w; x++) {
        ((uint16_t *)vram)[x] = white;
        ((uint16_t *)(vram + (size_t)(h - 1) * pitch))[x] = white;
    }
    for (int row = 0; row < h; row++) {
        uint16_t *line = (uint16_t *)(vram + (size_t)row * pitch);
        line[0] = white;
        line[w - 1] = white;
    }

    /* corner markers: n+1 white 10x10 squares */
    for (int n = 0; n < 4; n++) {
        for (int k = 0; k <= n; k++) {
            int x0 = (n == 0 || n == 2) ? 8 + k * 16 : w - 18 - k * 16;
            int y0 = (n == 0 || n == 1) ? 8 : h - 18;
            for (int row = y0; row < y0 + 10; row++) {
                uint16_t *line = (uint16_t *)(vram + (size_t)row * pitch);
                for (int x = x0; x < x0 + 10; x++)
                    line[x] = white;
            }
        }
    }
}

/* Program scanout: CR67 Mode 9 (15-bit 555) + logical screen width.
 * All writes during vertical retrace. lsw is the CR13/CR51 value
 * (pitch = lsw * 4 under the doubleword rule, DB019-B CR13/CR51). */
static void scanout_program(struct virge_ctx *ctx, uint8_t cr67_low,
                            uint8_t cr51_keep, uint32_t lsw)
{
    virge_wait_vsync(ctx);
    virge_crtc_poke(0x67, (uint8_t)((0x3 << 4) | cr67_low));
    virge_crtc_poke(0x13, (uint8_t)(lsw & 0xFF));
    virge_crtc_poke(0x51, (uint8_t)(cr51_keep | (((lsw >> 8) & 3) << 4)));
}

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));

    /* 800x600 @16bpp: what the CRTC truth dump says the raster is.
     * virge_init programs engine registers only — nothing here touches
     * scanout until phase 2. */
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

    /* ---- Phase 1: hypothesis strips, no register writes ---- */

    /* Blank the largest candidate scanout window (3200 x 600) so stale
     * VRAM garbage doesn't pollute the photo. */
    memset(vram, 0, 3200u * 600u);

    draw_strip(vram, 0,      3200, 4, 800, 96);  /* A: 32bpp  */
    draw_strip(vram, 320000, 2400, 3, 800, 96);  /* B: 24bpp  */
    draw_strip(vram, 640000, 1600, 2, 800, 96);  /* C: 555    */

    printf("\nPHASE 1 — no registers written. Three strips are in VRAM:\n"
           "  A (top   ): 32-bit pixels, pitch 3200\n"
           "  B (middle): packed 24-bit, pitch 2400\n"
           "  C (lower ): 16-bit RGB555, pitch 1600\n"
           "Exactly ONE should show clean RED/GREEN/BLUE/WHITE bands in a\n"
           "crisp white outline — that is the real scanout layout.\n"
           "Photograph, note the winner (A/B/C), then press Enter for\n"
           "phase 2 (scanout takeover), or Ctrl-C to exit without any\n"
           "register writes.\n\n");

    /* Ctrl-C or closed stdin: exit with no registers ever written. */
    if (getchar() == EOF)
        goto out_no_restore;

    /* ---- Phase 2: take over scanout, with save/restore ---- */

    uint8_t old67 = virge_crtc_peek(0x67);
    uint8_t old13 = virge_crtc_peek(0x13);
    uint8_t old51 = virge_crtc_peek(0x51);
    uint8_t cr67_low = old67 & 0x0F;
    uint8_t cr51_keep = old51 & ~0x30;
    printf("saved: CR67=%02x CR13=%02x CR51=%02x\n", old67, old13, old51);

    uint32_t lsw = 400;  /* pitch 1600 under the doubleword x4 rule */
    draw_555_screen(vram, 800, 600, 1600);
    scanout_program(&vctx, cr67_low, cr51_keep, lsw);

    printf("PHASE 2 — scanout is now CR67 Mode 9 (555), LSW=%u.\n"
           "  CORRECT: white border hugging all 4 edges, corner markers\n"
           "  1/2/3/4, solid red/green/blue/white bands, then TWO more\n"
           "  bands of which the FIRST (0x03E0) is pure green.\n"
           "  If the image is sheared exactly 2:1 instead, the multiplier\n"
           "  is quadword — press Enter to toggle LSW 400 <-> 200 and\n"
           "  note which value is clean.\n"
           "  Ctrl-C restores the original scanout registers and exits.\n\n",
           lsw);

    while (running) {
        if (getchar() == EOF)
            break;          /* Ctrl-C or closed stdin: restore and exit */
        lsw = (lsw == 400) ? 200 : 400;
        scanout_program(&vctx, cr67_low, cr51_keep, lsw);
        printf("LSW=%u (pitch %u if x4, %u if x8)\n", lsw, lsw * 4, lsw * 8);
    }

    printf("restoring CR67=%02x CR13=%02x CR51=%02x\n", old67, old13, old51);
    virge_wait_vsync(&vctx);
    virge_crtc_poke(0x67, old67);
    virge_crtc_poke(0x13, old13);
    virge_crtc_poke(0x51, old51);

out_no_restore:
    virge_cleanup(&vctx);
    return 0;
}
