/*
 * virge.c - Userspace drawing engine driver for S3 ViRGE (86C325)
 *
 * Talks directly to the hardware via PCI MMIO — no DRM, no DRI, no kernel
 * module.  Requires a working fbdev console (the kernel savagefb or VESA
 * framebuffer driver must have already established the video mode).
 *
 * Based on S3 ViRGE Integrated 3D Accelerator datasheet, DB019-B, Aug 1996.
 *
 * The ViRGE uses the "new MMIO" method: registers are at offset 0x1000000
 * from BAR0. The first 16MB of the BAR0 aperture is linear framebuffer
 * memory. The S3d Engine registers for each command type (2D, 3D line,
 * 3D triangle) are at fixed offsets within the 0x1000000+ region.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/io.h>
#include <stdint.h>
#include <linux/fb.h>

#include "virge.h"

/* ========================================================================
 * Legacy VGA CRTC Register Access
 * ======================================================================== */

static uint8_t vga_crtc_read(uint8_t index)
{
    outb(index, VIRGE_VGA_CRTC_INDEX);
    return inb(VIRGE_VGA_CRTC_DATA);
}

static void vga_crtc_write(uint8_t index, uint8_t value)
{
    outb(index, VIRGE_VGA_CRTC_INDEX);
    outb(value, VIRGE_VGA_CRTC_DATA);
}

/*
 * Make sure the MMIO aperture at BAR0 + VIRGE_MMIO_OFFSET is actually
 * routed to silicon before anything tries to use it. See the CR53
 * comment in virge.h for why this is necessary.
 */
static int vga_ensure_new_mmio(void)
{
    if (ioperm(0x3C0, 0x20, 1) != 0)
        return -errno;

    /* Extended CRTC registers are locked by default; unlock them. */
    vga_crtc_write(VIRGE_CR38_UNLOCK_REG, VIRGE_CR38_UNLOCK_KEY);
    vga_crtc_write(VIRGE_CR39_UNLOCK_REG, VIRGE_CR39_UNLOCK_KEY);

    uint8_t cr53 = vga_crtc_read(VIRGE_CR53);
    printf("S3 ViRGE: CR53 = 0x%02x (MMIO_SELECT = %u)\n",
           cr53, (cr53 & VIRGE_CR53_MMIO_MASK) >> 3);
    uint8_t cr53_new = (cr53 & ~VIRGE_CR53_MMIO_MASK) | VIRGE_CR53_MMIO_NEW;
    if (cr53_new != cr53) {
        printf("S3 ViRGE: writing CR53 = 0x%02x\n", cr53_new);
        vga_crtc_write(VIRGE_CR53, cr53_new);
    }
    uint8_t cr53_check = vga_crtc_read(VIRGE_CR53);
    printf("S3 ViRGE: CR53 after write = 0x%02x (%s)\n", cr53_check,
           cr53_check == cr53_new ? "matches target" : "DOES NOT MATCH TARGET");

    /* Belt-and-suspenders: CR66 bit 0 is OR'd with AFC bit 0 (either one
     * enables enhanced functions), so set it too. */
    uint8_t cr66 = vga_crtc_read(VIRGE_CR66);
    printf("S3 ViRGE: CR66 = 0x%02x before write\n", cr66);
    vga_crtc_write(VIRGE_CR66, cr66 | VIRGE_CR66_ENB_EHFC);
    printf("S3 ViRGE: CR66 = 0x%02x after write\n", vga_crtc_read(VIRGE_CR66));

    return 0;
}

/* ========================================================================
 * PCI Discovery — find the S3 ViRGE via /sys/bus/pci/devices
 *
 * Same approach as the MGA-1064 driver: enumerate /sys/bus/pci/devices/,
 * read vendor/device hex files, parse BDF, read BARs from resource file.
 * ======================================================================== */

struct pci_bdf {
    int domain;
    int bus;
    int dev;
    int func;
    uint32_t bar[6];
    uint32_t bar_size[6];
    int irq;
    uint16_t device_id;
};

static int pci_read_hex(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    unsigned int val = 0;
    if (fscanf(f, "%x", &val) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return (int)val;
}

static int pci_find_device(struct pci_bdf *dev, uint16_t vendor,
                            const uint16_t *devices, int num_devices)
{
    FILE *f = popen("ls /sys/bus/pci/devices/", "r");
    if (!f)
        return -errno;

    char line[256];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';

        char path[512];

        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/vendor", line);
        if (pci_read_hex(path) != vendor)
            continue;

        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/device", line);
        int dev_id = pci_read_hex(path);
        int matched = 0;
        for (int k = 0; k < num_devices; k++) {
            if (dev_id == devices[k]) {
                matched = 1;
                break;
            }
        }
        if (!matched)
            continue;

        /* Found it — parse BDF */
        unsigned int domain, bus, devnum, func;
        if (sscanf(line, "%x:%x:%x.%x", &domain, &bus, &devnum, &func) != 4)
            continue;

        dev->domain = domain;
        dev->bus = bus;
        dev->dev = devnum;
        dev->func = func;
        dev->device_id = (uint16_t)dev_id;

        /* Read all BARs from the resource file */
        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/resource", line);
        FILE *rf = fopen(path, "r");
        if (rf) {
            for (int j = 0; j < 6; j++) {
                unsigned long start, end, flags;
                if (fscanf(rf, "%lx %lx %lx", &start, &end, &flags) == 3 &&
                    end > start) {
                    dev->bar[j] = (uint32_t)start;
                    dev->bar_size[j] = (uint32_t)(end - start + 1);
                }
            }
            fclose(rf);
        }

        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/irq", line);
        dev->irq = pci_read_hex(path);

        found = 1;
        break;
    }

    pclose(f);
    return found ? 0 : -ENODEV;
}

/* ========================================================================
 * Memory Mapping
 * ======================================================================== */

/*
 * Map BAR0 via /sys/bus/pci/devices/<bdf>/resource0
 *
 * The ViRGE BAR0 is a 64MB aperture. We map enough to cover both the
 * linear framebuffer (first 4MB) and the MMIO registers (at 0x1000000).
 * Mapping the full 64MB is simplest.
 */
static void *map_bar0(struct pci_bdf *dev, size_t *size_out)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource0",
             dev->domain, dev->bus, dev->dev, dev->func);

    /* The BAR0 aperture size varies by ViRGE variant and VRAM fitted
     * (originals are commonly 64MB, but DX/GX/MX parts are often
     * smaller). The kernel's resourceN mmap rejects lengths longer
     * than the BAR's actual size, so use what sysfs reported. */
    size_t size = dev->bar_size[0];
    if (size == 0)
        size = 0x4000000;

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror(path);
        return NULL;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap BAR0");
        close(fd);
        return NULL;
    }

    *size_out = size;
    return ptr;
}

/* ========================================================================
 * Engine Synchronization
 * ======================================================================== */

void virge_wait_engine(struct virge_ctx *ctx)
{
    while (!(virge_read32(ctx, VIRGE_SUBSYS_STATUS) & VIRGE_STATUS_3DIDLE))
        ;
}

void virge_wait_vsync(struct virge_ctx *ctx)
{
    /* Wait for vsync to end (not in retrace) */
    while (virge_read32(ctx, VIRGE_SUBSYS_STATUS) & VIRGE_STATUS_VSYNC)
        ;
    /* Wait for vsync to start (entering retrace) */
    while (!(virge_read32(ctx, VIRGE_SUBSYS_STATUS) & VIRGE_STATUS_VSYNC))
        ;
}

/* ========================================================================
 * Global Engine Initialization
 *
 * Sets up the 3D register bank's static registers: destination base,
 * Z-base, strides, and clipping rectangle. These are set once and don't
 * need to be reprogrammed for each triangle.
 * ======================================================================== */

static void engine_init_3d(struct virge_ctx *ctx)
{
    uint32_t dest_stride = ctx->width * ctx->bpp;
    uint32_t z_stride = ctx->width * 2;  /* Z is always 16-bit */

    /* DEST_BASE: framebuffer origin in VRAM */
    virge_write32(ctx, VIRGE_3D_DEST_BASE, ctx->fb_base);

    /* Z_BASE: Z-buffer origin (after framebuffer), quadword aligned */
    uint32_t z_base = ctx->z_base;
    z_base &= ~0x7;  /* force quadword alignment */
    virge_write32(ctx, VIRGE_3D_Z_BASE, z_base);

    /* DEST_SRC_STR: destination stride [27:16], source stride [11:0] */
    virge_write32(ctx, VIRGE_3D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));

    /* Z_STRIDE */
    virge_write32(ctx, VIRGE_3D_Z_STRIDE, z_stride & 0xFFF);

    /* Clip to full screen */
    virge_write32(ctx, VIRGE_3D_CLIP_L_R,
                  ((ctx->width - 1) << 16) | 0);
    virge_write32(ctx, VIRGE_3D_CLIP_T_B,
                  ((ctx->height - 1) << 16) | 0);
}

/* ========================================================================
 * Rectangle Fill (2D)
 *
 * Uses the 2D rectangle fill command. The ViRGE rectangle fill writes
 * a solid color rectangle using the S3d Engine's 2D path.
 * ======================================================================== */

void virge_fill_rect(struct virge_ctx *ctx, int x, int y, int w, int h,
                     uint32_t color)
{
    virge_wait_engine(ctx);

    uint32_t dest_stride = ctx->width * ctx->bpp;

    /* Set destination base and stride for the 2D bank */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  ((ctx->width - 1) << 16) | 0);
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  ((ctx->height - 1) << 16) | 0);

    /* Foreground color = fill color */
    virge_write32(ctx, VIRGE_2D_PAT_FG_CLR, color);

    /* Mono pattern data: all 64 bits set selects the foreground color
     * for every pixel in the pattern tile. Without this, the pattern
     * bits are whatever was last left in these registers, and some
     * pixels silently get PAT_BG_CLR (also uninitialized) instead. */
    virge_write32(ctx, VIRGE_2D_MONO_PAT_0, 0xFFFFFFFF);
    virge_write32(ctx, VIRGE_2D_MONO_PAT_1, 0xFFFFFFFF);

    /* Width and height: W-1 in [26:16], H in [10:0] */
    virge_write32(ctx, VIRGE_2D_RWIDTH_HEIGHT,
                  (((w - 1) & 0x7FF) << 16) | (h & 0x7FF));

    /* Destination XY: X in [26:16], Y in [10:0] */
    virge_write32(ctx, VIRGE_2D_RDEST_XY,
                  ((x & 0x7FF) << 16) | (y & 0x7FF));

    /*
     * Command Set for rectangle fill:
     *   bit 28: rectangle fill
     *   bit 8: mono pattern (forced, selects PAT_FG_CLR)
     *   bits 7-5: destination format
     *   bits 24-17: ROP = PATCOPY (0xF0)
     *   bit 1: clipping enabled
     */
    uint32_t cmd = VIRGE_2D_CMD_RECT_FILL
                 | VIRGE_2D_MONO_PATTERN
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17)
                 | VIRGE_CMD_CLIP_ENABLE;
    /* bit 31 = 0 for 2D */

    virge_write32(ctx, VIRGE_2D_CMD_SET, cmd);
}

/* ========================================================================
 * Z Buffer Clear
 *
 * Fills the Z-buffer region with a constant Z value. We use a 2D
 * rectangle fill targeting the Z-buffer's VRAM address.
 * ======================================================================== */

void virge_clear_z(struct virge_ctx *ctx, float z)
{
    virge_wait_engine(ctx);

    /* The Z value as a 16-bit fixed-point word.
     * ViRGE Z is S16.15. For the MUX scheme bit 15 has special meaning,
     * but for normal Z-buffering we just write the value directly. */
    uint16_t z_val = (uint16_t)(z * 65535.0f);
    uint32_t z_color = (z_val) | (z_val << 16);  /* pack for stride fill */

    uint32_t dest_stride = ctx->width * ctx->bpp;
    uint32_t z_stride = ctx->width * 2;  /* 16-bit Z */

    /* Reprogram 2D registers to point at Z buffer instead of framebuffer */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->z_base & ~0x7);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((z_stride & 0xFFF) << 16) | (z_stride & 0xFFF));
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  ((ctx->width * 2 - 1) << 16) | 0);
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  ((ctx->height - 1) << 16) | 0);

    /* Fill color = the Z value repeated */
    virge_write32(ctx, VIRGE_2D_PAT_FG_CLR, z_color);

    /* See virge_fill_rect() for why this is required. */
    virge_write32(ctx, VIRGE_2D_MONO_PAT_0, 0xFFFFFFFF);
    virge_write32(ctx, VIRGE_2D_MONO_PAT_1, 0xFFFFFFFF);

    /* Width = width * 2 bytes (since Z is 16-bit), height = screen height */
    int z_width_bytes = ctx->width * 2;
    virge_write32(ctx, VIRGE_2D_RWIDTH_HEIGHT,
                  (((z_width_bytes - 1) & 0x7FF) << 16) |
                  (ctx->height & 0x7FF));

    virge_write32(ctx, VIRGE_2D_RDEST_XY, 0);

    /* Use 8bpp dest format for the fill since we're writing raw bytes */
    uint32_t cmd = VIRGE_2D_CMD_RECT_FILL
                 | VIRGE_2D_MONO_PATTERN
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17)
                 | VIRGE_CMD_CLIP_ENABLE;

    virge_write32(ctx, VIRGE_2D_CMD_SET, cmd);

    /* Restore 2D DEST_BASE to framebuffer for subsequent operations */
    virge_wait_engine(ctx);
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
}

/* ========================================================================
 * Gouraud-Shaded Triangle
 *
 * The ViRGE's native 3D triangle primitive handles the entire triangle
 * in one engine kick — no software trapezoid decomposition needed.
 *
 * Setup:
 *   1. Sort vertices by Y (descending — ViRGE renders bottom-to-top)
 *   2. Compute edge slopes (dX/dY) for sides 02, 01, 12
 *   3. Compute Gouraud color gradients (dR/dX, dR/dY, etc.) via plane eq
 *   4. Compute Z gradients
 *   5. Program CMD_SET, color/Z registers, edge registers
 *   6. Kick by writing CMD_SET (autoexecute off)
 * ======================================================================== */

/*
 * Compute dX/dY as S11.20 fixed point.
 * The ViRGE convention (from the 2D line programming examples):
 *   dXdY = -(ΔX / ΔY) * (1 << 20)
 * This gives the slope with inverted sign, matching the engine's
 * bottom-to-top rendering direction.
 */
static int32_t compute_dxdy(int x_start, int y_start, int x_end, int y_end)
{
    int dx = x_end - x_start;
    int dy = y_end - y_start;

    if (dy == 0) dy = 1;  /* avoid division by zero */

    /* dXdY = -(ΔX << 20) / ΔY */
    return -(int32_t)(((int64_t)dx << VIRGE_X_FRAC_BITS) / dy);
}

void virge_draw_triangle_gouraud(struct virge_ctx *ctx,
                                  struct virge_vertex v0,
                                  struct virge_vertex v1,
                                  struct virge_vertex v2)
{
    /*
     * Sort vertices by Y descending: v0 = bottom (largest Y),
     * v1 = middle, v2 = top (smallest Y).
     * The ViRGE always renders from bottom to top.
     */
    if (v0.y < v1.y) { struct virge_vertex t = v0; v0 = v1; v1 = t; }
    if (v1.y < v2.y) { struct virge_vertex t = v1; v1 = v2; v2 = t; }
    if (v0.y < v1.y) { struct virge_vertex t = v0; v0 = v1; v1 = t; }

    int y_bot = (int)v0.y;
    int y_mid = (int)v1.y;
    int y_top = (int)v2.y;

    /* Degenerate triangle */
    if (y_bot == y_top)
        return;

    /*
     * Compute plane-equation gradients for Z, R, G, B, A.
     *
     * For attribute F at vertices F0,F1,F2 at positions (x0,y0), etc:
     *   det = (x1-x0)(y2-y0) - (x2-x0)(y1-y0)
     *   dFdx = [(F1-F0)(y2-y0) - (F2-F0)(y1-y0)] / det
     *   dFdy = [(F2-F0)(x1-x0) - (F1-F0)(x2-x0)] / det
     *
     * Note: v0 is the BOTTOM vertex (largest Y), not the top.
     * The plane equation is still computed from these three points.
     */
    float dx10 = v1.x - v0.x;
    float dy10 = v1.y - v0.y;
    float dx20 = v2.x - v0.x;
    float dy20 = v2.y - v0.y;
    float det = dx10 * dy20 - dx20 * dy10;

    if (det == 0.0f)
        return;

    float inv_det = 1.0f / det;

    float dzdx = ((v1.z - v0.z) * dy20 - (v2.z - v0.z) * dy10) * inv_det;
    float dzdy = ((v2.z - v0.z) * dx10 - (v1.z - v0.z) * dx20) * inv_det;

    float drdx = ((v1.r - v0.r) * dy20 - (v2.r - v0.r) * dy10) * inv_det;
    float drdy = ((v2.r - v0.r) * dx10 - (v1.r - v0.r) * dx20) * inv_det;

    float dgdx = ((v1.g - v0.g) * dy20 - (v2.g - v0.g) * dy10) * inv_det;
    float dgdy = ((v2.g - v0.g) * dx10 - (v1.g - v0.g) * dx20) * inv_det;

    float dbdx = ((v1.b - v0.b) * dy20 - (v2.b - v0.b) * dy10) * inv_det;
    float dbdy = ((v2.b - v0.b) * dx10 - (v1.b - v0.b) * dx20) * inv_det;

    float dadx = ((v1.a - v0.a) * dy20 - (v2.a - v0.a) * dy10) * inv_det;
    float dady = ((v2.a - v0.a) * dx10 - (v1.a - v0.a) * dx20) * inv_det;

    /* Edge slopes (dX/dY in S11.20)
     *
     * Side 02: v0(bottom) → v2(top) — the long edge
     * Side 01: v0(bottom) → v1(middle)
     * Side 12: v1(middle) → v2(top)
     */
    int32_t dXdY02 = compute_dxdy((int)v0.x, y_bot, (int)v2.x, y_top);
    int32_t dXdY01 = compute_dxdy((int)v0.x, y_bot, (int)v1.x, y_mid);
    int32_t dXdY12 = compute_dxdy((int)v1.x, y_mid, (int)v2.x, y_top);

    /* Determine L/R direction: which side of the triangle is the 02 edge?
     * If v1 is to the left of the v0→v2 line, then side 02 is on the
     * right, so we render right-to-left (L/R bit = 0).
     * If v1 is to the right, side 02 is on the left → render left-to-right
     * (L/R bit = 1).
     *
     * The cross product of (v2-v0) × (v1-v0) tells us:
     *   positive → v1 is to the right of v0→v2 → 02 is on the left → L/R=1
     *   negative → v1 is to the left of v0→v2 → 02 is on the right → L/R=0
     */
    int lr_direction;
    float cross = (v2.x - v0.x) * (v1.y - v0.y) - (v1.x - v0.x) * (v2.y - v0.y);
    if (cross > 0)
        lr_direction = 1;  /* side 02 on left, render left-to-right */
    else
        lr_direction = 0;  /* side 02 on right, render right-to-left */

    /* Scan line counts */
    int scan_01 = y_bot - y_mid;  /* bottom half (side 01) */
    int scan_12 = y_mid - y_top;  /* top half (side 12) */

    /* Edge X end values (S11.20) */
    int32_t x_end01 = VIRGE_X_FIXED(v1.x);  /* X at middle vertex */
    int32_t x_end12 = VIRGE_X_FIXED(v2.x);  /* X at top vertex */

    /* Evaluate color and Z at the bottom vertex (the start point) */
    float z_s = v0.z;
    float r_s = v0.r;
    float g_s = v0.g;
    float b_s = v0.b;
    float a_s = v0.a;

    virge_wait_engine(ctx);

    /* --- Program color start values (S8.7 packed) --- */
    virge_write32(ctx, VIRGE_3D_TGS_BS,
                  ((uint16_t)VIRGE_COLOR_FIXED(g_s) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(b_s));
    virge_write32(ctx, VIRGE_3D_TAS_RS,
                  ((uint16_t)VIRGE_COLOR_FIXED(a_s) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(r_s));

    /* --- Program color deltas (S8.7 packed) --- */
    virge_write32(ctx, VIRGE_3D_TdGdX_dBdX,
                  ((uint16_t)VIRGE_COLOR_FIXED(dgdx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(dbdx));
    virge_write32(ctx, VIRGE_3D_TdAdX_dRdX,
                  ((uint16_t)VIRGE_COLOR_FIXED(dadx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(drdx));
    virge_write32(ctx, VIRGE_3D_TdGdY_dBdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(dgdy) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(dbdy));
    virge_write32(ctx, VIRGE_3D_TdAdY_dRdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(dady) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(drdy));

    /* --- Program Z start and deltas (S16.15) --- */
    virge_write32(ctx, VIRGE_3D_TZS, (uint32_t)VIRGE_Z_FIXED(z_s));
    virge_write32(ctx, VIRGE_3D_TdZdX, (uint32_t)VIRGE_Z_FIXED(dzdx));
    virge_write32(ctx, VIRGE_3D_TdZdY, (uint32_t)VIRGE_Z_FIXED(dzdy));

    /* --- Program edge geometry --- */
    virge_write32(ctx, VIRGE_3D_TdXdY02, (uint32_t)dXdY02);
    virge_write32(ctx, VIRGE_3D_TdXdY01, (uint32_t)dXdY01);
    virge_write32(ctx, VIRGE_3D_TdXdY12, (uint32_t)dXdY12);

    virge_write32(ctx, VIRGE_3D_TXEND01, (uint32_t)x_end01);
    virge_write32(ctx, VIRGE_3D_TXEND12, (uint32_t)x_end12);

    virge_write32(ctx, VIRGE_3D_TXS,
                  (uint32_t)VIRGE_X_FIXED(v0.x));
    virge_write32(ctx, VIRGE_3D_TYS, (uint32_t)y_bot);

    /* --- Program scan counts and L/R direction --- */
    virge_write32(ctx, VIRGE_3D_TY01_Y12,
                  ((uint32_t)lr_direction << 31) |
                  ((scan_01 & 0x7FF) << 16) |
                  (scan_12 & 0x7FF));

    /* --- Program Command Set and execute --- */
    uint32_t cmd = VIRGE_CMD_3D
                 | VIRGE_3D_GOURAUD
                 | ctx->dest_format
                 | VIRGE_ZB_NORMAL
                 | VIRGE_ZBC_LEQUAL     /* default: pass if Zs <= Zzb */
                 | VIRGE_ZUP_ENABLE     /* update Z on pass */
                 | VIRGE_CMD_CLIP_ENABLE;

    virge_write32(ctx, VIRGE_3D_CMD_SET, cmd);
}

/* ========================================================================
 * 2D Line Drawing
 *
 * The ViRGE 2D line engine draws from bottom to top. We compute the
 * Bresenham parameters and program the 2D line registers.
 * ======================================================================== */

void virge_draw_line(struct virge_ctx *ctx,
                      int x0, int y0, int x1, int y1, uint32_t color)
{
    virge_wait_engine(ctx);

    /*
     * The ViRGE 2D line engine always draws from bottom to top.
     * The starting point must be the endpoint with the largest Y.
     * If y0 == y1, use the largest X as start (horizontal line special case).
     */
    if (y0 < y1 || (y0 == y1 && x0 < x1)) {
        int tx = x0; x0 = x1; x1 = tx;
        int ty = y0; y0 = y1; y1 = ty;
    }

    int dX = x1 - x0;
    int dY = y1 - y0;
    int abs_dX = dX < 0 ? -dX : dX;
    int abs_dY = dY < 0 ? -dY : dY;

    if (abs_dY == 0 && abs_dX == 0)
        return;

    /*
     * X DELTA = -(ΔX << 20) / ΔY (integer divide)
     * For horizontal lines (ΔY = 0), X DELTA = 0 and the engine uses
     * the endpoint registers.
     */
    int32_t x_delta;
    if (abs_dY == 0) {
        x_delta = 0;  /* horizontal line special case */
    } else {
        x_delta = -(int32_t)(((int64_t)dX << 20) / abs_dY);
    }

    /*
     * X START = (xSTART << 20) + (X DELTA/2) for X-major lines
     *         = (xSTART << 20) for Y-major lines
     *
     * A line is X-major if |ΔX| > |ΔY|.
     */
    int32_t x_start;
    int x_major = (abs_dX > abs_dY);
    if (x_major && x_delta > 0) {
        x_start = (x0 << 20) + (x_delta / 2);
    } else if (x_major && x_delta < 0) {
        x_start = (x0 << 20) + (x_delta / 2) + ((1 << 20) - 1);
    } else {
        x_start = (x0 << 20);
    }

    int y_count = abs_dY + 1;

    /* Direction bit: 1 = left to right, 0 = right to left */
    uint32_t dir = (dX >= 0) ? 1 : 0;

    /* Y START = y0 (the bottom point, which has the largest Y) */

    uint32_t dest_stride = ctx->width * ctx->bpp;

    /* Set up 2D registers for line draw bank */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  ((ctx->width - 1) << 16) | 0);
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  ((ctx->height - 1) << 16) | 0);

    /* Foreground color */
    virge_write32(ctx, VIRGE_2D_PAT_FG_CLR, color);

    /* Line endpoints: END0 [31:16], END1 [15:0] */
    /* END0 = first pixel drawn for bottommost scanline
     * END1 = last pixel drawn for topmost scanline
     * For a simple line, both are the endpoints' X coordinates. */
    uint32_t endpoints = ((x0 & 0xFFFF) << 16) | (x1 & 0xFFFF);
    virge_write32(ctx, 0xA96C, endpoints);  /* L_XEND0_END1 */

    /* X DELTA and X START */
    virge_write32(ctx, 0xA970, (uint32_t)x_delta);  /* L_XDELTA */
    virge_write32(ctx, 0xA974, (uint32_t)x_start);  /* L_XSTART */
    virge_write32(ctx, 0xA978, (uint32_t)y0);       /* L_YSTART */

    /* Command Set for 2D line:
     *   bits [31:27] = 00110 (2D line)
     *   bit 8: mono pattern
     *   bits 7-5: dest format
     *   bits 24-17: ROP = PATCOPY (0xF0)
     *   bit 1: clip enable
     */
    uint32_t cmd = ((0x06 << 27) & 0x1F)  /* 2D line = bits [30:27] = 0110 */
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17)
                 | VIRGE_CMD_CLIP_ENABLE;

    /* Y count + direction — this is the kick register for 2D lines */
    virge_write32(ctx, 0xA900, cmd);  /* L_CMD_SET */

    uint32_t ycnt_dir = (dir << 31) | (y_count & 0x7FF);
    virge_write32(ctx, 0xA97C, ycnt_dir);  /* L_YCNT_DIR — kicks engine */
}

/* ========================================================================
 * Texture Upload
 *
 * Copies texture pixel data into offscreen VRAM using the CPU directly
 * through the linear addressing window. This is simpler than a 2D BitBLT
 * for uploading a contiguous texture image — we just memcpy into the
 * framebuffer aperture since the first 4MB of BAR0 IS the linear VRAM.
 *
 * The texture heap starts after the framebuffer + Z buffer and grows
 * upward. A simple bump allocator is sufficient for demos.
 * ======================================================================== */

void virge_upload_texture(struct virge_ctx *ctx, uint32_t dest,
                           const void *data, size_t size)
{
    virge_wait_engine(ctx);

    /* dest is a byte offset in VRAM. The linear framebuffer aperture
     * (ctx->fb) maps VRAM linearly, so we can write directly. */
    char *dst = (char *)ctx->fb + dest;
    memcpy(dst, data, size);
}

/* ========================================================================
 * Textured Triangle
 *
 * Draws a perspective-correct, lit textured triangle using the ViRGE's
 * native 3D triangle primitive with 3D command = 0101 (lit texture
 * with perspective).
 *
 * The key difference from the Gouraud triangle is that we must program:
 *   - Texture base address (TEX_BASE)
 *   - U/V/W start values and deltas (perspective-correct)
 *   - D (mipmap level interpolation, set to 0 for non-mipmapped)
 *   - CMD_SET with texture format, filter mode, and blend mode
 *
 * Texture coordinate interpolation with perspective:
 *   The ViRGE interpolates U/W, V/W, and 1/W across the triangle, then
 *   divides to get perspective-correct U,V. We provide U, V, W as:
 *     U_texel = u * (tex_width - 1)
 *     V_texel = v * (tex_height - 1)
 *     W = 1.0 / Z_eye (or 1.0 if perspective correction is disabled)
 *
 * The color values modulate the texel (modulate blending mode).
 * ======================================================================== */

/*
 * The ViRGE texture U/V format with perspective enabled is:
 *   S(4+s).(27-s) where s = mipmap level size from CMD_SET bits 11-8.
 * For a single-level texture (s=0, meaning 1×1 base — not useful),
 * or more commonly, we use s = log2(tex_width) for a power-of-2 texture.
 *
 * For a 64×64 texture (s=6): format is S10.21.
 * For a 128×128 texture (s=7): format is S11.20.
 * For a 256×256 texture (s=8): format is S12.19.
 *
 * We convert float U/V to the hardware format by left-shifting by
 * (27 - s) fractional bits.
 */

/* Helper: convert float to ViRGE texture coordinate with perspective.
 * s_val = mipmap level parameter from CMD_SET bits 11-8.
 * frac_bits = 27 - s_val.
 */
static int32_t tex_coord_fixed(float val, int s_val)
{
    int frac_bits = 27 - s_val;
    return (int32_t)(val * (float)(1 << frac_bits));
}

void virge_draw_textured_triangle(struct virge_ctx *ctx,
                                   struct virge_vertex v0,
                                   struct virge_vertex v1,
                                   struct virge_vertex v2)
{
    /* Sort vertices by Y descending: v0 = bottom (largest Y) */
    if (v0.y < v1.y) { struct virge_vertex t = v0; v0 = v1; v1 = t; }
    if (v1.y < v2.y) { struct virge_vertex t = v1; v1 = v2; v2 = t; }
    if (v0.y < v1.y) { struct virge_vertex t = v0; v0 = v1; v1 = t; }

    int y_bot = (int)v0.y;
    int y_mid = (int)v1.y;
    int y_top = (int)v2.y;

    if (y_bot == y_top)
        return;

    /* Edge slopes (same as Gouraud triangle) */
    int32_t dXdY02 = compute_dxdy((int)v0.x, y_bot, (int)v2.x, y_top);
    int32_t dXdY01 = compute_dxdy((int)v0.x, y_bot, (int)v1.x, y_mid);
    int32_t dXdY12 = compute_dxdy((int)v1.x, y_mid, (int)v2.x, y_top);

    /* L/R direction */
    int lr_direction;
    float cross = (v2.x - v0.x) * (v1.y - v0.y) - (v1.x - v0.x) * (v2.y - v0.y);
    if (cross > 0)
        lr_direction = 1;
    else
        lr_direction = 0;

    int scan_01 = y_bot - y_mid;
    int scan_12 = y_mid - y_top;

    /*
     * Texture coordinate gradients.
     *
     * The ViRGE interpolates U, V, W linearly across the triangle
     * (in screen space) when perspective correction is enabled.
     * We compute dU/dX, dU/dY, dV/dX, dV/dY, dW/dX, dW/dY using
     * the same plane-equation approach as color gradients.
     *
     * The caller provides W = 1/Z_eye (perspective) or W = 1.0 (disable).
     * U and V are in texel units.
     */
    float dx10 = v1.x - v0.x;
    float dy10 = v1.y - v0.y;
    float dx20 = v2.x - v0.x;
    float dy20 = v2.y - v0.y;
    float det = dx10 * dy20 - dx20 * dy10;

    if (det == 0.0f)
        return;

    float inv_det = 1.0f / det;

    /* Plane equation gradient helper */
    #define DFDX(f) (((v1.f - v0.f) * dy20 - (v2.f - v0.f) * dy10) * inv_det)
    #define DFDY(f) (((v2.f - v0.f) * dx10 - (v1.f - v0.f) * dx20) * inv_det)

    float dudx = DFDX(u), dudy = DFDY(u);
    float dvdx = DFDX(v), dvdy = DFDY(v);
    float dwdx = DFDX(w), dwdy = DFDY(w);

    /* Color gradients (for modulation) */
    float drdx = DFDX(r), drdy = DFDY(r);
    float dgdx = DFDX(g), dgdy = DFDY(g);
    float dbdx = DFDX(b), dbdy = DFDY(b);
    float dadx = DFDX(a), dady = DFDY(a);

    /* Z gradients */
    float dzdx = DFDX(z), dzdy = DFDY(z);

    #undef DFDX
    #undef DFDY

    /* Determine the mipmap level size parameter.
     * The texture's power-of-2 dimension determines s where 2^s = tex_size.
     * We use the cached s value from tex_cmd_bits (set during bind_texture).
     * For now, extract s from the cached CMD_SET bits 11-8.
     * Default to s=6 (64×64) if no texture is properly configured. */
    int s_val = (ctx->tex_cmd_bits >> 8) & 0xF;
    if (s_val == 0) s_val = 6;  /* safe default */

    virge_wait_engine(ctx);

    /* --- Color starts + deltas (S8.7, modulation) --- */
    virge_write32(ctx, VIRGE_3D_TGS_BS,
                  ((uint16_t)VIRGE_COLOR_FIXED(v0.g) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(v0.b));
    virge_write32(ctx, VIRGE_3D_TAS_RS,
                  ((uint16_t)VIRGE_COLOR_FIXED(v0.a) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(v0.r));
    virge_write32(ctx, VIRGE_3D_TdGdX_dBdX,
                  ((uint16_t)VIRGE_COLOR_FIXED(dgdx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(dbdx));
    virge_write32(ctx, VIRGE_3D_TdAdX_dRdX,
                  ((uint16_t)VIRGE_COLOR_FIXED(dadx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(drdx));
    virge_write32(ctx, VIRGE_3D_TdGdY_dBdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(dgdy) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(dbdy));
    virge_write32(ctx, VIRGE_3D_TdAdY_dRdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(dady) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(drdy));

    /* --- Z starts + deltas (S16.15) --- */
    virge_write32(ctx, VIRGE_3D_TZS, (uint32_t)VIRGE_Z_FIXED(v0.z));
    virge_write32(ctx, VIRGE_3D_TdZdX, (uint32_t)VIRGE_Z_FIXED(dzdx));
    virge_write32(ctx, VIRGE_3D_TdZdY, (uint32_t)VIRGE_Z_FIXED(dzdy));

    /* --- Texture coordinates (U, V, W with perspective) --- */
    /* Convert to hardware fixed-point */
    int32_t u_start = tex_coord_fixed(v0.u, s_val);
    int32_t v_start = tex_coord_fixed(v0.v, s_val);
    int32_t w_start = (int32_t)(v0.w * (float)(1 << 19));  /* S12.19 */

    /* dU/dX in the same format */
    int32_t du_dx = tex_coord_fixed(dudx, s_val);
    int32_t du_dy = tex_coord_fixed(dudy, s_val);
    int32_t dv_dx = tex_coord_fixed(dvdx, s_val);
    int32_t dv_dy = tex_coord_fixed(dvdy, s_val);
    int32_t dw_dx = (int32_t)(dwdx * (float)(1 << 19));
    int32_t dw_dy = (int32_t)(dwdy * (float)(1 << 19));

    /* Base U/V are common offsets added to all U/V values (usually 0) */
    virge_write32(ctx, VIRGE_3D_TBU, 0);
    virge_write32(ctx, VIRGE_3D_TBV, 0);

    /* W start + deltas (S12.19) */
    virge_write32(ctx, VIRGE_3D_TWS, (uint32_t)w_start);
    virge_write32(ctx, VIRGE_3D_TdWdX, (uint32_t)dw_dx);
    virge_write32(ctx, VIRGE_3D_TdWdY, (uint32_t)dw_dy);

    /* D (mipmap level) — set to 0, no mipmap interpolation for now */
    virge_write32(ctx, VIRGE_3D_TDS, 0);
    virge_write32(ctx, VIRGE_3D_TdDdX, 0);
    virge_write32(ctx, VIRGE_3D_TdDdY, 0);

    /* V start + deltas */
    virge_write32(ctx, VIRGE_3D_TVS, (uint32_t)v_start);
    virge_write32(ctx, VIRGE_3D_TdVdX, (uint32_t)dv_dx);
    virge_write32(ctx, VIRGE_3D_TdVdY, (uint32_t)dv_dy);

    /* U start + deltas */
    virge_write32(ctx, VIRGE_3D_TUS, (uint32_t)u_start);
    virge_write32(ctx, VIRGE_3D_TdUdX, (uint32_t)du_dx);
    virge_write32(ctx, VIRGE_3D_TdUdY, (uint32_t)du_dy);

    /* --- Edge geometry (same as Gouraud) --- */
    virge_write32(ctx, VIRGE_3D_TdXdY02, (uint32_t)dXdY02);
    virge_write32(ctx, VIRGE_3D_TdXdY01, (uint32_t)dXdY01);
    virge_write32(ctx, VIRGE_3D_TdXdY12, (uint32_t)dXdY12);
    virge_write32(ctx, VIRGE_3D_TXEND01, (uint32_t)VIRGE_X_FIXED(v1.x));
    virge_write32(ctx, VIRGE_3D_TXEND12, (uint32_t)VIRGE_X_FIXED(v2.x));
    virge_write32(ctx, VIRGE_3D_TXS, (uint32_t)VIRGE_X_FIXED(v0.x));
    virge_write32(ctx, VIRGE_3D_TYS, (uint32_t)y_bot);

    virge_write32(ctx, VIRGE_3D_TY01_Y12,
                  ((uint32_t)lr_direction << 31) |
                  ((scan_01 & 0x7FF) << 16) |
                  (scan_12 & 0x7FF));

    /* --- Command Set: lit texture triangle with perspective --- */
    uint32_t cmd = VIRGE_CMD_3D
                 | VIRGE_3D_LIT_TEX_PERSP   /* 0101: lit texture + perspective */
                 | ctx->dest_format
                 | ctx->tex_cmd_bits         /* texture format, filter, blend, wrap */
                 | VIRGE_ZB_NORMAL
                 | VIRGE_ZBC_LEQUAL
                 | VIRGE_ZUP_ENABLE
                 | VIRGE_CMD_CLIP_ENABLE;

    virge_write32(ctx, VIRGE_3D_CMD_SET, cmd);
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

int virge_init(struct virge_ctx *ctx, int width, int height, int bpp)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;
    ctx->fb_fd = -1;
    ctx->bar_fd = -1;

    /* Find the S3 ViRGE on the PCI bus */
    struct pci_bdf pci = {0};
    static const uint16_t virge_devices[] = S3_PCI_DEVICE_VIRGE_ALL;
    int ret = pci_find_device(&pci, S3_PCI_VENDOR_ID, virge_devices,
                              sizeof(virge_devices) / sizeof(virge_devices[0]));
    if (ret < 0) {
        fprintf(stderr, "S3 ViRGE: PCI device not found\n");
        return ret;
    }

    printf("S3 ViRGE: Found at %04x:%02x:%02x.%x (device id 0x%04x)\n",
           pci.domain, pci.bus, pci.dev, pci.func, pci.device_id);
    printf("  BAR0: 0x%08x (%u MB aperture)\n", pci.bar[0],
           pci.bar_size[0] / (1024 * 1024));

    ctx->bar0 = pci.bar[0];

    /* Map BAR0 (64MB aperture containing framebuffer + MMIO) */
    ctx->mmio = map_bar0(&pci, &ctx->mmio_size);
    if (!ctx->mmio) {
        fprintf(stderr, "S3 ViRGE: Failed to map BAR0\n");
        return -ENOMEM;
    }
    printf("  BAR0 mapped: %zu bytes at %p\n", ctx->mmio_size, ctx->mmio);

    /* The framebuffer is at the start of BAR0 (offset 0).
     * The MMIO registers are at offset 0x1000000.
     * For register access, we set mmio to point at BAR0 + 0x1000000.
     * But we keep the full BAR0 mapping and just offset our writes.
     *
     * Actually, we need both: the framebuffer pointer for CPU access,
     * and the MMIO pointer for register access. Since both are in the
     * same BAR0 mapping, we set:
     *   ctx->fb   = BAR0 base + 0 (framebuffer)
     *   ctx->mmio = BAR0 base + VIRGE_MMIO_OFFSET (registers)
     */
    ctx->fb = ctx->mmio;                              /* framebuffer at offset 0 */
    ctx->mmio = (void *)((char *)ctx->mmio + VIRGE_MMIO_OFFSET);  /* regs at 0x1000000 */

    /* Force CR53 to select "new MMIO" — until this is set, the entire
     * aperture at BAR0 + VIRGE_MMIO_OFFSET is unrouted and every write
     * below (including the AFC enable) is silently dropped. */
    ret = vga_ensure_new_mmio();
    if (ret < 0) {
        fprintf(stderr, "S3 ViRGE: couldn't get VGA I/O port access "
                        "(need root): %s\n", strerror(-ret));
        return ret;
    }

    /* Enable the 8514/A-compatible accelerated register interface.
     * Without this, the 2D/3D command bank silently ignores all writes
     * — only the linear framebuffer aperture is live. */
    virge_write32(ctx, VIRGE_ADV_FUNC_CTRL, VIRGE_AFC_ENABLE);
    {
        uint32_t afc_check = virge_read32(ctx, VIRGE_ADV_FUNC_CTRL);
        printf("S3 ViRGE: AFC readback: 0x%08x (bit0 %s)\n", afc_check,
               (afc_check & VIRGE_AFC_ENABLE) ? "set" : "NOT SET");
        uint32_t status_check = virge_read32(ctx, VIRGE_SUBSYS_STATUS);
        printf("S3 ViRGE: SUBSYS_STATUS readback: 0x%08x\n", status_check);
    }

    /* Try to get framebuffer size from /dev/fb0 if available */
    ctx->fb_fd = open("/dev/fb0", O_RDWR);
    if (ctx->fb_fd >= 0) {
        struct fb_fix_screeninfo finfo;
        if (ioctl(ctx->fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            ctx->fb_size = finfo.smem_len;
        } else {
            ctx->fb_size = width * height * bpp;
        }
    } else {
        ctx->fb_size = width * height * bpp;
    }

    /* Compute memory layout (byte offsets in VRAM) */
    ctx->fb_base = 0;
    ctx->z_base = width * height * bpp;  /* Z buffer after framebuffer */
    ctx->vram_size = S3_VIRGE_VRAM_SIZE;

    /* Texture heap starts after framebuffer + Z buffer, quadword aligned */
    uint32_t z_size = width * height * 2;  /* Z is always 16-bit */
    ctx->tex_heap_next = ctx->z_base + z_size;
    ctx->tex_heap_next = (ctx->tex_heap_next + 7) & ~7;  /* QW align */
    ctx->tex_cmd_bits = 0;
    ctx->tex_bound = 0;

    /* Set destination format field based on bpp */
    if (bpp == 2)
        ctx->dest_format = VIRGE_DEST_16BPP;
    else if (bpp == 3)
        ctx->dest_format = VIRGE_DEST_24BPP;
    else
        ctx->dest_format = VIRGE_DEST_8BPP;

    /* Initialize the 3D register bank */
    engine_init_3d(ctx);

    printf("S3 ViRGE: S3d Engine initialized.\n");
    printf("  Screen: %dx%d, %d bpp\n", width, height, bpp * 8);
    printf("  FB base: 0x%x, Z base: 0x%x\n", ctx->fb_base, ctx->z_base);

    return 0;
}

void virge_cleanup(struct virge_ctx *ctx)
{
    virge_wait_engine(ctx);

    /* Unmap the full BAR0 (we offset mmio from the base, so we need
     * to unmap from the original base address).
     * ctx->fb still points at the BAR0 base. */
    if (ctx->fb) {
        munmap(ctx->fb, ctx->mmio_size);
        ctx->fb = NULL;
        ctx->mmio = NULL;
    }
    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
    }
}
