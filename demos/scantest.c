/*
 * scantest.c - Scanout diagnosis tool for the S3 ViRGE.
 *
 * Built for machines where /dev/fb0 does not exist: no kernel fb driver
 * is bound to the card, so the chip scans out whatever the bootloader's
 * VBE call left behind, and virge_init takes over the scanout natively
 * (Mode 9/555). This tool shows visually whether the takeover produced
 * the layout the driver thinks it did.
 *
 * PHASE 1 (no register writes of its own; virge_init has already run
 * its takeover): CPU-draws three test strips into VRAM over BAR0, each
 * betting on a different scanout layout:
 *
 *      strip A at VRAM 0      : 32-bit pixels, pitch 3200
 *      strip B at VRAM 320000 : packed 24-bit, pitch 2400
 *      strip C at VRAM 640000 : 16-bit RGB555, pitch 1600
 *
 *   Exactly ONE strip will show clean solid RED/GREEN/BLUE/WHITE bands
 *   inside a crisp white outline — that one names the actual layout.
 *   With the takeover working, strip C must be the clean one. (All
 *   three offsets land on-screen under any of the candidate pitches.)
 *
 * PHASE 2 (press Enter): draws a full-screen RGB555 pattern using the
 *   geometry virge_init adopted (ctx width/height/stride). Correct
 *   output: white border hugging all 4 edges, corner markers 1/2/3/4,
 *   solid red/green/blue/white bands, then two discriminator bands of
 *   which the FIRST (raw 0x03E0) is pure green. Ctrl-C exits;
 *   virge_cleanup restores the pre-takeover scanout registers.
 *
 * Build: make scantest       (always builds against the virge backend)
 * Run:   sudo ./scantest
 *
 * HISTORY (2026-07-06): phase 1 of the original version named strip A
 * (32-bit/3200) as the leftover VBE layout. The original phase 2 wrote
 * CR67/CR13/CR51 itself, with two bugs the hardware exposed: it did
 * not double the horizontal timings as 15/16bpp modes require, so
 * hsync doubled and the monitor dropped sync ("input signal out of
 * range" — it briefly showed the pattern before giving up, which read
 * as "displays, then screen non-responsive"); and it used a x4 pitch
 * rule where the chip uses quadwords (x8), so its pattern was silently
 * squashed 2:1 while it showed. The corrected takeover lives in
 * virge_scanout_takeover() in virge.c; this tool only verifies it.
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

int main(void)
{
    struct virge_ctx vctx;
    memset(&vctx, 0, sizeof(vctx));

    /* On a no-fbdev machine virge_init performs the native scanout
     * takeover (Mode 9/555, doubled horizontals, quadword pitch) and
     * adopts the real raster into vctx.width/height/stride. */
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

    printf("\nPHASE 1 — three hypothesis strips are in VRAM (scanout was\n"
           "already taken over by virge_init, so C must be the clean one):\n"
           "  A (top   ): 32-bit pixels, pitch 3200\n"
           "  B (middle): packed 24-bit, pitch 2400\n"
           "  C (lower ): 16-bit RGB555, pitch 1600\n"
           "Exactly ONE should show clean RED/GREEN/BLUE/WHITE bands in a\n"
           "crisp white outline — that is the real scanout layout.\n"
           "Photograph, note the winner (A/B/C), then press Enter for\n"
           "phase 2 (full-screen pattern), or Ctrl-C to exit.\n\n");

    if (getchar() == EOF)
        goto out;

    /* ---- Phase 2: full-screen pattern at the adopted geometry ---- */

    draw_555_screen(vram, vctx.width, vctx.height, vctx.stride);

    printf("PHASE 2 — full-screen 555 pattern at %dx%d, pitch %u.\n"
           "  CORRECT: white border hugging all 4 edges, corner markers\n"
           "  1/2/3/4, solid red/green/blue/white bands, then TWO more\n"
           "  bands of which the FIRST (0x03E0) is pure green.\n"
           "  Ctrl-C exits and restores the pre-takeover scanout.\n\n",
           vctx.width, vctx.height, vctx.stride);

    while (running) {
        if (getchar() == EOF)
            break;          /* Ctrl-C or closed stdin: exit */
    }

out:
    virge_cleanup(&vctx);
    return 0;
}
