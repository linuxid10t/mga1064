/*
 * fbtest.c - Engine-free fbdev test pattern (diagnostic, no L10GL code).
 *
 * Draws a known pattern through /dev/fb0 with plain CPU writes via mmap,
 * using ONLY the geometry fbdev reports (line_length, xres/yres, color
 * bitfields). No PCI, no MMIO, no ViRGE registers -- so its output
 * separates the two halves of every "garbled screen" report:
 *
 *   - Pattern displays clean  -> scanout and fbdev's view of the mode are
 *     trustworthy; any remaining garble is engine-side (register bugs).
 *   - Pattern is garbled too  -> fbdev's view disagrees with what the CRTC
 *     actually scans out; no engine fix can help until the mode itself is
 *     understood (compare against the "CRTC truth" dump in virge_init).
 *
 * What correct output looks like:
 *   - A crisp 1px white rectangle hugging all four screen edges. A wrong
 *     pitch shears it into diagonals; a wrong resolution crops it or
 *     leaves a margin.
 *   - Distinct corner markers: 1 white square top-left, 2 top-right,
 *     3 bottom-left, 4 bottom-right -- so cropping, wrap-around, and
 *     repetition are attributable to a specific screen region.
 *   - Horizontal bands top-to-bottom: RED, GREEN, BLUE, WHITE (packed via
 *     the reported bitfields), then at 16bpp two format-discriminator
 *     bands packed as HARDCODED raw values: 0x03E0 (green if scanout is
 *     RGB555) and 0x07E0 (green if RGB565). Whichever of the two looks
 *     pure green names the ACTUAL scanout format, regardless of what
 *     fbdev claims -- this checks whether the V8 15bpp switch really
 *     reprogrammed the RAMDAC color mode (CR67).
 *
 * Build: make fbtest        (standalone -- links no backend)
 * Run:   sudo ./fbtest
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static volatile int running = 1;

static void sighandler(int sig)
{
    (void)sig;
    running = 0;
}

/* Pack an 8-bit-per-channel color according to the fbdev-reported
 * bitfields (truncate each channel to its field width). */
static uint32_t pack_rgb(const struct fb_var_screeninfo *v,
                         unsigned r, unsigned g, unsigned b)
{
    uint32_t px = 0;
    px |= (uint32_t)(r >> (8 - v->red.length))   << v->red.offset;
    px |= (uint32_t)(g >> (8 - v->green.length)) << v->green.offset;
    px |= (uint32_t)(b >> (8 - v->blue.length))  << v->blue.offset;
    return px;
}

/* Write one pixel of bytes_pp bytes, little-endian, using only the
 * fbdev-reported pitch. */
static void put_pixel(uint8_t *fb, const struct fb_fix_screeninfo *f,
                      int bytes_pp, int x, int y, uint32_t px)
{
    uint8_t *p = fb + (size_t)y * f->line_length + (size_t)x * bytes_pp;
    for (int i = 0; i < bytes_pp; i++)
        p[i] = (uint8_t)(px >> (8 * i));
}

static void fill_rect(uint8_t *fb, const struct fb_fix_screeninfo *f,
                      int bytes_pp, int x, int y, int w, int h, uint32_t px)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            put_pixel(fb, f, bytes_pp, i, j, px);
}

int main(void)
{
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) < 0) {
        perror("FBIOGET_*SCREENINFO");
        close(fd);
        return 1;
    }

    /* Report everything fbdev claims BEFORE drawing -- finfo.id names the
     * kernel driver ("S3 Virge" = s3fb, "VESA VGA" = vesafb, ...), which
     * decides whether mode switches (V8) can work at all. */
    printf("fbdev claims:\n");
    printf("  fix: id=\"%.16s\"  smem_len=%u  line_length=%u\n",
           finfo.id, finfo.smem_len, finfo.line_length);
    printf("  var: %ux%u visible, %ux%u virtual, offset %u,%u, %u bpp\n",
           vinfo.xres, vinfo.yres, vinfo.xres_virtual, vinfo.yres_virtual,
           vinfo.xoffset, vinfo.yoffset, vinfo.bits_per_pixel);
    printf("  var: red %u@%u  green %u@%u  blue %u@%u  transp %u@%u\n",
           vinfo.red.length, vinfo.red.offset,
           vinfo.green.length, vinfo.green.offset,
           vinfo.blue.length, vinfo.blue.offset,
           vinfo.transp.length, vinfo.transp.offset);

    int bytes_pp = (vinfo.bits_per_pixel + 7) / 8;  /* 15bpp -> 2 bytes */
    int w = vinfo.xres;
    int h = vinfo.yres;

    if (bytes_pp < 1 || bytes_pp > 4 || w < 64 || h < 64) {
        fprintf(stderr, "Mode too odd to draw a pattern (%dx%d, %d B/px)\n",
                w, h, bytes_pp);
        close(fd);
        return 1;
    }
    if ((size_t)finfo.line_length * h > finfo.smem_len) {
        fprintf(stderr, "Inconsistent: line_length*yres (%zu) > smem_len "
                "(%u) -- fbdev is lying about something already\n",
                (size_t)finfo.line_length * h, finfo.smem_len);
        close(fd);
        return 1;
    }

    uint8_t *fb = mmap(NULL, finfo.smem_len, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap /dev/fb0");
        close(fd);
        return 1;
    }

    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);

    uint32_t white = pack_rgb(&vinfo, 255, 255, 255);
    uint32_t black = pack_rgb(&vinfo, 0, 0, 0);

    /* Background */
    fill_rect(fb, &finfo, bytes_pp, 0, 0, w, h, black);

    /* Color bands in the central region (leave a margin so the border
     * stays visible). At 16bpp add the two raw format-discriminator
     * bands; at other depths only the bitfield-packed bands. */
    struct band {
        const char *name;
        uint32_t px;
        int raw;  /* 1 = hardcoded raw value, not bitfield-packed */
    } bands[6];
    int nbands = 0;
    bands[nbands++] = (struct band){ "RED   (bitfield-packed)",
                                     pack_rgb(&vinfo, 255, 0, 0), 0 };
    bands[nbands++] = (struct band){ "GREEN (bitfield-packed)",
                                     pack_rgb(&vinfo, 0, 255, 0), 0 };
    bands[nbands++] = (struct band){ "BLUE  (bitfield-packed)",
                                     pack_rgb(&vinfo, 0, 0, 255), 0 };
    bands[nbands++] = (struct band){ "WHITE (bitfield-packed)", white, 0 };
    if (bytes_pp == 2) {
        bands[nbands++] = (struct band){
            "raw 0x03E0 (green IF scanout is RGB555)", 0x03E0, 1 };
        bands[nbands++] = (struct band){
            "raw 0x07E0 (green IF scanout is RGB565)", 0x07E0, 1 };
    }

    int margin = 16;
    int band_h = (h - 2 * margin) / nbands;
    printf("pattern (top to bottom):\n");
    for (int i = 0; i < nbands; i++) {
        int y0 = margin + i * band_h;
        fill_rect(fb, &finfo, bytes_pp, margin, y0,
                  w - 2 * margin, band_h - 4, bands[i].px);
        printf("  rows %4d-%4d: %s\n", y0, y0 + band_h - 5, bands[i].name);
    }

    /* 1px white border around the full visible screen -- drawn last so
     * nothing overpaints it. */
    fill_rect(fb, &finfo, bytes_pp, 0, 0,     w, 1, white);
    fill_rect(fb, &finfo, bytes_pp, 0, h - 1, w, 1, white);
    fill_rect(fb, &finfo, bytes_pp, 0, 0,     1, h, white);
    fill_rect(fb, &finfo, bytes_pp, w - 1, 0, 1, h, white);

    /* Corner markers: N 10x10 white squares, 6px apart, inset 8px.
     * TL=1, TR=2, BL=3, BR=4. */
    for (int n = 0; n < 4; n++) {
        int count = n + 1;
        for (int k = 0; k < count; k++) {
            int off = 8 + k * 16;
            int x = (n == 0 || n == 2) ? off : w - 8 - 10 - k * 16;
            int y = (n == 0 || n == 1) ? 8 : h - 8 - 10;
            fill_rect(fb, &finfo, bytes_pp, x, y, 10, 10, white);
        }
    }

    printf("\nwhat CORRECT looks like:\n"
           "  - crisp 1px white rectangle hugging all 4 screen edges\n"
           "    (diagonal shear = pitch wrong; crop/margin = resolution wrong)\n"
           "  - white squares in corners: 1=top-left 2=top-right\n"
           "    3=bottom-left 4=bottom-right\n"
           "  - solid bands: red, green, blue, white%s\n",
           bytes_pp == 2 ?
           ",\n    then two more bands -- whichever looks PURE GREEN names\n"
           "    the real scanout format (555 vs 565)" : "");
    printf("\nPattern drawn. Photograph, then Ctrl-C to exit.\n");

    while (running)
        usleep(100000);

    munmap(fb, finfo.smem_len);
    close(fd);
    return 0;
}
