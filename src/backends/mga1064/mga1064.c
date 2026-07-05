/*
 * mga1064.c - Userspace drawing engine driver for Matrox MGA-1064SG (Mystique)
 *
 * Talks directly to the hardware via PCI MMIO — no DRM, no DRI, no kernel
 * module.  Requires a working fbdev console (the kernel matroxfb or VESA
 * framebuffer driver must have already established the video mode).
 *
 * Based on MGA-1064SG Developer Specification, Feb 10, 1997.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/io.h>
#include <stdint.h>
#include <linux/fb.h>

#include "mga1064.h"

/* ========================================================================
 * PCI Discovery — find the MGA-1064SG via /sys/bus/pci/devices
 * ========================================================================
 */

struct pci_bdf {
    int domain;
    int bus;
    int dev;
    int func;
    uint32_t bar[6];
    int irq;
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

static int pci_find_device(struct pci_bdf *dev, uint16_t vendor, uint16_t device)
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
        if (pci_read_hex(path) != device)
            continue;

        /* Found it — parse BDF */
        unsigned int domain, bus, devnum, func;
        if (sscanf(line, "%x:%x:%x.%x", &domain, &bus, &devnum, &func) != 4)
            continue;

        dev->domain = domain;
        dev->bus = bus;
        dev->dev = devnum;
        dev->func = func;

        /* Read all BARs from the resource file */
        snprintf(path, sizeof(path),
                 "/sys/bus/pci/devices/%s/resource", line);
        FILE *rf = fopen(path, "r");
        if (rf) {
            for (int j = 0; j < 6; j++) {
                unsigned long start, end, flags;
                if (fscanf(rf, "%lx %lx %lx", &start, &end, &flags) == 3)
                    dev->bar[j] = (uint32_t)start;
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
 * ========================================================================
 */

static void *map_mmio(struct pci_bdf *dev, size_t *size_out)
{
    char path[256];
    snprintf(path, sizeof(path),
             "/sys/bus/pci/devices/%04x:%02x:%02x.%x/resource0",
             dev->domain, dev->bus, dev->dev, dev->func);

    /* Control aperture is 16KB (0x4000) */
    size_t size = 0x4000;

    int fd = open(path, O_RDWR | O_SYNC);
    if (fd < 0) {
        perror(path);
        return NULL;
    }

    void *ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (ptr == MAP_FAILED) {
        perror("mmap MMIO");
        close(fd);
        return NULL;
    }

    *size_out = size;
    return ptr;
}

/* ========================================================================
 * Engine Synchronization
 * ========================================================================
 */

void mga1064_wait_engine(struct mga1064_ctx *ctx)
{
    while (mga_read32(ctx, MGA_STATUS) & MGA_STATUS_DWGENGSTS)
        ;
}

void mga1064_wait_vsync(struct mga1064_ctx *ctx)
{
    /* Wait for vsync to end (not in retrace) */
    while (mga_read32(ctx, MGA_STATUS) & MGA_STATUS_VSYNCSTS)
        ;
    /* Wait for vsync to start (entering retrace) */
    while (!(mga_read32(ctx, MGA_STATUS) & MGA_STATUS_VSYNCSTS))
        ;
}

/* ========================================================================
 * Global Engine Initialization (section 5.5.3)
 * ========================================================================
 */

static void engine_init_global(struct mga1064_ctx *ctx)
{
    /* PITCH: distance between scanlines in pixels */
    mga_write32(ctx, MGA_PITCH, ctx->width);

    /* YDSTORG: screen origin at VRAM pixel offset 0 */
    mga_write32(ctx, MGA_YDSTORG, ctx->fb_offset);

    /* MACCESS: pixel format */
    uint32_t maccess = 0;
    if (ctx->bpp == 2)
        maccess |= MGA_PWIDTH_16;   /* 5:6:5 */
    else if (ctx->bpp == 4)
        maccess |= MGA_PWIDTH_32;
    else if (ctx->bpp == 1)
        maccess |= MGA_PWIDTH_8;
    mga_write32(ctx, MGA_MACCESS, maccess);

    /* CXBNDRY: (right << 16) | left */
    mga_write32(ctx, MGA_CXBNDRY,
                ((ctx->width - 1) << 16) | 0);

    mga_write32(ctx, MGA_YTOP, 0);
    mga_write32(ctx, MGA_YBOT, ctx->height - 1);
    mga_write32(ctx, MGA_PLNWT, 0xFFFFFFFF);

    /* ZORG: Z buffer base address (pixel offset in VRAM) */
    mga_write32(ctx, MGA_ZORG, ctx->z_offset);
}

/* ========================================================================
 * Rectangle Fill
 * ========================================================================
 */

void mga1064_fill_rect(struct mga1064_ctx *ctx, int x, int y, int w, int h,
                       uint32_t color)
{
    mga1064_wait_engine(ctx);

    mga_write32(ctx, MGA_FCOL, color);

    uint32_t dwgctl = MGA_OPCOD_TRAP
                    | MGA_ATYPE_RPL
                    | MGA_ZMODE_NOZCMP
                    | MGA_SOLID
                    | MGA_ARZERO
                    | MGA_SGNZERO
                    | MGA_SHFTZERO
                    | MGA_TRANS_DISABLE;
    mga_write32(ctx, MGA_DWGCTL, dwgctl);

    mga_write32(ctx, MGA_FXBNDRY, ((x + w) << 16) | (x & 0xFFFF));

    /* Write YDSTLEN to exec range to start the engine */
    mga_write32(ctx, MGA_YDSTLEN + MGA_EXEC_OFFSET,
                (y << 16) | h);
}

/* ========================================================================
 * Z Buffer Clear
 * ========================================================================
 */

void mga1064_clear_z(struct mga1064_ctx *ctx, float z)
{
    mga1064_wait_engine(ctx);

    int32_t z_fixed = MGA_Z_FIXED(z);

    mga_write32(ctx, MGA_DR0, z_fixed);
    mga_write32(ctx, MGA_DR2, 0);  /* dZ/dx = 0 */
    mga_write32(ctx, MGA_DR3, 0);  /* dZ/dy = 0 */

    /* TRAP with ZI atype (depth mode), NOZCMP (always write Z) */
    uint32_t dwgctl = MGA_OPCOD_TRAP
                    | (MGA_ATYPE_VAL_ZI << 4)
                    | MGA_ZMODE_NOZCMP
                    | MGA_SOLID
                    | MGA_ARZERO
                    | MGA_SGNZERO
                    | MGA_SHFTZERO;
    mga_write32(ctx, MGA_DWGCTL, dwgctl);

    mga_write32(ctx, MGA_FXBNDRY, (ctx->width << 16) | 0);
    mga_write32(ctx, MGA_YDSTLEN + MGA_EXEC_OFFSET,
                (0 << 16) | ctx->height);
}

/* ========================================================================
 * Gouraud-Shaded Triangle
 * ========================================================================
 */

struct edge_params {
    int32_t dY;       /* AR0 (left) or AR6 (right) */
    int32_t dX_neg;   /* AR2 (left) or AR5 (right): -|dX| */
    int32_t err;      /* AR1 (left) or AR4 (right) */
    int sdx;          /* sign of dX: 0=positive, 1=negative */
};

static void compute_edge(int x_start, int y_start,
                          int x_end, int y_end,
                          struct edge_params *e)
{
    int dY = y_end - y_start;
    int dX = x_end - x_start;

    if (dY < 0) dY = -dY;
    e->dY = dY;

    if (dX < 0) {
        e->dX_neg = dX;
        e->err = dX + dY - 1;
        e->sdx = 1;
    } else {
        e->dX_neg = -dX;
        e->err = -dX;
        e->sdx = 0;
    }
}

static void draw_gouraud_trap(struct mga1064_ctx *ctx,
                               int y_start, int num_lines,
                               int x_left_start, int x_right_start,
                               struct edge_params *left,
                               struct edge_params *right,
                               float z_start, float dzdx, float dzdy,
                               float r_start, float drdx, float drdy,
                               float g_start, float dgdx, float dgdy,
                               float b_start, float dbdx, float dbdy)
{
    mga1064_wait_engine(ctx);

    /* Z interpolation (17.15 fixed point) */
    mga_write32(ctx, MGA_DR0, MGA_Z_FIXED(z_start));
    mga_write32(ctx, MGA_DR2, MGA_Z_FIXED(dzdx));
    mga_write32(ctx, MGA_DR3, MGA_Z_FIXED(dzdy));

    /* Gouraud color interpolation (9.15 fixed point) */
    mga_write32(ctx, MGA_DR4,  MGA_COLOR_FIXED(r_start));
    mga_write32(ctx, MGA_DR6,  MGA_COLOR_FIXED(drdx));
    mga_write32(ctx, MGA_DR7,  MGA_COLOR_FIXED(drdy));
    mga_write32(ctx, MGA_DR8,  MGA_COLOR_FIXED(g_start));
    mga_write32(ctx, MGA_DR10, MGA_COLOR_FIXED(dgdx));
    mga_write32(ctx, MGA_DR11, MGA_COLOR_FIXED(dgdy));
    mga_write32(ctx, MGA_DR12, MGA_COLOR_FIXED(b_start));
    mga_write32(ctx, MGA_DR14, MGA_COLOR_FIXED(dbdx));
    mga_write32(ctx, MGA_DR15, MGA_COLOR_FIXED(dbdy));

    /* Edge parameters */
    mga_write32(ctx, MGA_AR0, left->dY);
    mga_write32(ctx, MGA_AR1, left->err);
    mga_write32(ctx, MGA_AR2, left->dX_neg);
    mga_write32(ctx, MGA_AR4, right->err);
    mga_write32(ctx, MGA_AR5, right->dX_neg);
    mga_write32(ctx, MGA_AR6, right->dY);

    /* SGN register: bit1=sdxl, bit3=sdxr, scanleft=0 (scan right) */
    uint32_t sgn = (left->sdx << 1) | (right->sdx << 3);
    mga_write32(ctx, MGA_SGN, sgn);

    /* DWGCTL: TRAP + ZI (Gouraud with depth) + ZLTE */
    uint32_t dwgctl = MGA_OPCOD_TRAP
                    | (MGA_ATYPE_VAL_ZI << 4)
                    | MGA_ZMODE_ZLTE
                    | MGA_SOLID
                    | MGA_SHFTZERO;
    mga_write32(ctx, MGA_DWGCTL, dwgctl);

    mga_write32(ctx, MGA_FXBNDRY,
                (x_right_start << 16) | (x_left_start & 0xFFFF));

    mga_write32(ctx, MGA_YDSTLEN + MGA_EXEC_OFFSET,
                (y_start << 16) | (num_lines & 0xFFFF));
}

/*
 * Draw a Gouraud-shaded, Z-tested triangle.
 *
 * Vertices are sorted by Y, then split into flat-bottom and flat-top
 * trapezoids.  Color and Z use plane-equation gradients (dFdx, dFdy)
 * computed from vertex attributes.
 */
void mga1064_draw_triangle_gouraud(struct mga1064_ctx *ctx,
                                    struct mga_vertex v0,
                                    struct mga_vertex v1,
                                    struct mga_vertex v2)
{
    /* Sort vertices by Y */
    if (v0.y > v1.y) { struct mga_vertex t = v0; v0 = v1; v1 = t; }
    if (v1.y > v2.y) { struct mga_vertex t = v1; v1 = v2; v2 = t; }
    if (v0.y > v1.y) { struct mga_vertex t = v0; v0 = v1; v1 = t; }

    int y_top = (int)v0.y;
    int y_mid = (int)v1.y;
    int y_bot = (int)v2.y;

    /* Compute plane equation gradients.
     *
     * For attribute F at vertices F0,F1,F2 at positions (x0,y0), etc:
     *   det = (x1-x0)(y2-y0) - (x2-x0)(y1-y0)
     *   dFdx = [(F1-F0)(y2-y0) - (F2-F0)(y1-y0)] / det
     *   dFdy = [(F2-F0)(x1-x0) - (F1-F0)(x2-x0)] / det
     */
    float dx10 = v1.x - v0.x;
    float dy10 = v1.y - v0.y;
    float dx20 = v2.x - v0.x;
    float dy20 = v2.y - v0.y;
    float det = dx10 * dy20 - dx20 * dy10;

    if (det == 0.0f || y_bot == y_top)
        return;  /* degenerate */

    float inv_det = 1.0f / det;

    /* Compute gradients for Z, R, G, B */
    float dzdx = ((v1.z - v0.z) * dy20 - (v2.z - v0.z) * dy10) * inv_det;
    float dzdy = ((v2.z - v0.z) * dx10 - (v1.z - v0.z) * dx20) * inv_det;
    float drdx = ((v1.r - v0.r) * dy20 - (v2.r - v0.r) * dy10) * inv_det;
    float drdy = ((v2.r - v0.r) * dx10 - (v1.r - v0.r) * dx20) * inv_det;
    float dgdx = ((v1.g - v0.g) * dy20 - (v2.g - v0.g) * dy10) * inv_det;
    float dgdy = ((v2.g - v0.g) * dx10 - (v1.g - v0.g) * dx20) * inv_det;
    float dbdx = ((v1.b - v0.b) * dy20 - (v2.b - v0.b) * dy10) * inv_det;
    float dbdy = ((v2.b - v0.b) * dx10 - (v1.b - v0.b) * dx20) * inv_det;

    /* Evaluate attribute F at (px, py) using the plane equation from v0.
     * We use an inline function instead of a macro to avoid token-pasting
     * issues with the struct member names x and y. */
    #define EVAL_AT(f0, dfdx, dfdy, px, py) \
        ((f0) + (dfdx) * ((float)(px) - v0.x) + (dfdy) * ((float)(py) - v0.y))

    /* --- Top half: flat-bottom trapezoid (y_top → y_mid) --- */
    if (y_mid > y_top) {
        int num_lines = y_mid - y_top;

        struct edge_params edge_short, edge_long;
        compute_edge((int)v0.x, y_top, (int)v1.x, y_mid, &edge_short);
        compute_edge((int)v0.x, y_top, (int)v2.x, y_bot, &edge_long);

        /* X of the long edge at y_mid */
        float x_long_at_mid = v0.x + (float)(y_mid - y_top) *
                              ((v2.x - v0.x) / (float)(y_bot - y_top));

        struct edge_params *left_e, *right_e;

        if (v1.x < x_long_at_mid) {
            left_e  = &edge_short;   /* v0→v1 */
            right_e = &edge_long;    /* v0→v2 */
        } else {
            left_e  = &edge_long;
            right_e = &edge_short;
        }

        float z_s = EVAL_AT(v0.z, dzdx, dzdy, (int)v0.x, y_top);
        float r_s = EVAL_AT(v0.r, drdx, drdy, (int)v0.x, y_top);
        float g_s = EVAL_AT(v0.g, dgdx, dgdy, (int)v0.x, y_top);
        float b_s = EVAL_AT(v0.b, dbdx, dbdy, (int)v0.x, y_top);

        draw_gouraud_trap(ctx, y_top, num_lines,
                          (int)v0.x, (int)v0.x,
                          left_e, right_e,
                          z_s, dzdx, dzdy,
                          r_s, drdx, drdy,
                          g_s, dgdx, dgdy,
                          b_s, dbdx, dbdy);
    }

    /* --- Bottom half: flat-top trapezoid (y_mid → y_bot) --- */
    if (y_bot > y_mid) {
        int num_lines = y_bot - y_mid;

        struct edge_params edge_short, edge_long;
        compute_edge((int)v1.x, y_mid, (int)v2.x, y_bot, &edge_short);
        compute_edge((int)v0.x, y_top, (int)v2.x, y_bot, &edge_long);

        float x_long_at_mid = v0.x + (float)(y_mid - y_top) *
                              ((v2.x - v0.x) / (float)(y_bot - y_top));

        struct edge_params *left_e, *right_e;
        int x_l_start, x_r_start;

        if (v1.x < x_long_at_mid) {
            left_e  = &edge_short;
            right_e = &edge_long;
            x_l_start = (int)v1.x;
            x_r_start = (int)x_long_at_mid;
        } else {
            left_e  = &edge_long;
            right_e = &edge_short;
            x_l_start = (int)x_long_at_mid;
            x_r_start = (int)v1.x;
        }

        float z_s = EVAL_AT(v0.z, dzdx, dzdy, x_l_start, y_mid);
        float r_s = EVAL_AT(v0.r, drdx, drdy, x_l_start, y_mid);
        float g_s = EVAL_AT(v0.g, dgdx, dgdy, x_l_start, y_mid);
        float b_s = EVAL_AT(v0.b, dbdx, dbdy, x_l_start, y_mid);

        draw_gouraud_trap(ctx, y_mid, num_lines,
                          x_l_start, x_r_start,
                          left_e, right_e,
                          z_s, dzdx, dzdy,
                          r_s, drdx, drdy,
                          g_s, dgdx, dgdy,
                          b_s, dbdx, dbdy);
    }

    #undef EVAL_AT
}

/* ========================================================================
 * Line Drawing
 * ========================================================================
 */

void mga1064_draw_line(struct mga1064_ctx *ctx,
                        int x0, int y0, int x1, int y1, uint32_t color)
{
    mga1064_wait_engine(ctx);

    int dX = x1 - x0;
    int dY = y1 - y0;
    int a = abs(dX) > abs(dY) ? abs(dX) : abs(dY);  /* major axis */
    int b = abs(dX) > abs(dY) ? abs(dY) : abs(dX);  /* minor axis */

    if (a == 0) return;

    /* Bresenham: AR0=2b, AR1=2b-a, AR2=2b-2a */
    mga_write32(ctx, MGA_AR0, 2 * b);
    mga_write32(ctx, MGA_AR1, 2 * b - a);
    mga_write32(ctx, MGA_AR2, 2 * b - 2 * a);

    /* SGN */
    uint32_t sgn = 0;
    if (abs(dX) > abs(dY)) sgn |= 1;     /* major axis = X */
    if (dX < 0) sgn |= (1 << 1);         /* sdxl: negative X */
    if (dY < 0) sgn |= (1 << 2);         /* sdy: negative Y */
    mga_write32(ctx, MGA_SGN, sgn);

    mga_write32(ctx, MGA_FCOL, color);

    mga_write32(ctx, MGA_DWGCTL,
                MGA_OPCOD_LINE_CLOSE
                | MGA_ATYPE_RPL
                | MGA_ZMODE_NOZCMP
                | MGA_SOLID
                | MGA_SHFTZERO);

    mga_write32(ctx, MGA_XDST, x0);

    mga_write32(ctx, MGA_YDSTLEN + MGA_EXEC_OFFSET,
                (y0 << 16) | (a + 1));
}

/* ========================================================================
 * Initialization
 * ========================================================================
 */

int mga1064_init(struct mga1064_ctx *ctx, int width, int height, int bpp)
{
    memset(ctx, 0, sizeof(*ctx));

    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;
    ctx->pitch = width;
    ctx->fb_fd = -1;
    ctx->mem_fd = -1;

    /* Find the MGA-1064SG on the PCI bus.
     * The Mystique uses several PCI device IDs:
     *   0x051A - Mystique PCI (most common)
     *   0x051E - Mystique AGP
     *   0x0100 - Mystique (alternate ID on some boards)
     */
    struct pci_bdf pci;
    int ret = pci_find_device(&pci, MGA_PCI_VENDOR_ID,
                              MGA_PCI_DEVICE_1064SG_PCI);
    if (ret < 0)
        ret = pci_find_device(&pci, MGA_PCI_VENDOR_ID,
                              MGA_PCI_DEVICE_1064SG_AGP);
    if (ret < 0)
        ret = pci_find_device(&pci, MGA_PCI_VENDOR_ID,
                              MGA_PCI_DEVICE_1064SG_ALT);
    if (ret < 0) {
        fprintf(stderr, "MGA-1064SG: PCI device not found\n");
        return ret;
    }

    printf("MGA-1064SG: Found at %04x:%02x:%02x.%x\n",
           pci.domain, pci.bus, pci.dev, pci.func);
    printf("  BAR0 (MGABASE1): 0x%08x (control aperture)\n", pci.bar[0]);
    printf("  BAR1 (MGABASE2): 0x%08x (frame buffer)\n", pci.bar[1]);
    printf("  BAR2 (MGABASE3): 0x%08x (ILOAD aperture)\n", pci.bar[2]);

    ctx->mgabase1 = pci.bar[0];
    ctx->mgabase2 = pci.bar[1];
    ctx->mgabase3 = pci.bar[2];

    /* Map MMIO (control aperture) */
    ctx->mmio = map_mmio(&pci, &ctx->mmio_size);
    if (!ctx->mmio) {
        fprintf(stderr, "MGA-1064SG: Failed to map MMIO\n");
        return -ENOMEM;
    }
    printf("  MMIO mapped: %zu bytes at %p\n", ctx->mmio_size, ctx->mmio);

    /* Map framebuffer via /dev/fb0 */
    ctx->fb_fd = open("/dev/fb0", O_RDWR);
    if (ctx->fb_fd >= 0) {
        struct fb_fix_screeninfo finfo;
        if (ioctl(ctx->fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            ctx->fb_size = finfo.smem_len;
        } else {
            ctx->fb_size = width * height * bpp;
        }
        ctx->fb = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fb_fd, 0);
    } else {
        perror("open /dev/fb0");
        /* Fallback: /dev/mem with MGABASE2 physical address */
        ctx->fb_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (ctx->fb_fd < 0) {
            perror("open /dev/mem");
            return -errno;
        }
        ctx->fb_size = width * height * bpp;
        ctx->fb = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fb_fd,
                       (off_t)ctx->mgabase2);
    }

    if (ctx->fb == MAP_FAILED) {
        perror("mmap framebuffer");
        ctx->fb = NULL;
        return -ENOMEM;
    }
    printf("  Framebuffer mapped: %zu bytes at %p\n", ctx->fb_size, ctx->fb);

    /* Compute memory layout */
    ctx->fb_offset = 0;
    ctx->z_offset = width * height;  /* Z buffer after screen, in pixels */
    ctx->vram_size = ctx->fb_size;

    /* Initialize drawing engine */
    engine_init_global(ctx);

    printf("MGA-1064SG: Drawing engine initialized.\n");
    printf("  Screen: %dx%d, %d bpp\n", width, height, bpp * 8);
    printf("  FB offset: %u px, Z offset: %u px\n",
           ctx->fb_offset, ctx->z_offset);

    return 0;
}

void mga1064_cleanup(struct mga1064_ctx *ctx)
{
    mga1064_wait_engine(ctx);

    if (ctx->fb) {
        munmap(ctx->fb, ctx->fb_size);
        ctx->fb = NULL;
    }
    if (ctx->mmio) {
        munmap(ctx->mmio, ctx->mmio_size);
        ctx->mmio = NULL;
    }
    if (ctx->fb_fd >= 0) {
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
    }
}
