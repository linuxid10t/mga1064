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
#include <time.h>
#include <linux/fb.h>

#include "../../fbdev.h"
#include "../../pci_scan.h"
#include "mga1064.h"

static const uint16_t mga_devices[] = {
    MGA_PCI_DEVICE_1064SG_PCI,
    MGA_PCI_DEVICE_1064SG_AGP,
    MGA_PCI_DEVICE_1064SG_ALT,
};

static uint8_t mga_indexed_read(struct mga1064_ctx *ctx,
                                uint32_t index_reg, uint32_t data_reg,
                                uint8_t index)
{
    volatile uint8_t *mmio = (volatile uint8_t *)ctx->mmio;

    mmio[index_reg] = index;
    return mmio[data_reg];
}

static void mga_indexed_write(struct mga1064_ctx *ctx,
                              uint32_t index_reg, uint32_t data_reg,
                              uint8_t index, uint8_t value)
{
    volatile uint8_t *mmio = (volatile uint8_t *)ctx->mmio;

    mmio[index_reg] = index;
    mmio[data_reg] = value;
}

static int regions_overlap(uint32_t a, uint32_t b, uint32_t size)
{
    return (uint64_t)a < (uint64_t)b + size &&
           (uint64_t)b < (uint64_t)a + size;
}

static int find_free_surface(uint32_t vram_size, uint32_t surface_size,
                             const uint32_t *used, size_t used_count,
                             uint32_t *result)
{
    uint64_t candidate = 0;

    for (;;) {
        int moved = 0;

        candidate = (candidate + 7u) & ~UINT64_C(7);
        if (candidate + surface_size > vram_size)
            return -ENOSPC;
        for (size_t i = 0; i < used_count; i++) {
            if (regions_overlap((uint32_t)candidate, used[i], surface_size)) {
                candidate = (uint64_t)used[i] + surface_size;
                moved = 1;
                break;
            }
        }
        if (!moved) {
            *result = (uint32_t)candidate;
            return 0;
        }
    }
}

int mga1064_plan_double_buffer(uint32_t front_bytes, uint32_t stride,
                               uint32_t height, uint32_t bytes_per_pixel,
                               uint32_t vram_size,
                               struct mga1064_buffer_layout *layout)
{
    uint64_t surface_size = (uint64_t)stride * height;
    uint32_t used[2];
    int ret;

    if (!layout || !stride || !height ||
        (bytes_per_pixel != 1 && bytes_per_pixel != 2 &&
         bytes_per_pixel != 4) || stride % bytes_per_pixel ||
        front_bytes % 8u || front_bytes % bytes_per_pixel ||
        surface_size > UINT32_MAX ||
        (uint64_t)front_bytes + surface_size > vram_size)
        return -EINVAL;

    memset(layout, 0, sizeof(*layout));
    layout->front_bytes = front_bytes;
    layout->surface_bytes = (uint32_t)surface_size;
    used[0] = front_bytes;
    ret = find_free_surface(vram_size, layout->surface_bytes, used, 1,
                            &layout->back_bytes);
    if (ret)
        return ret;
    used[1] = layout->back_bytes;
    ret = find_free_surface(vram_size, layout->surface_bytes, used, 2,
                            &layout->z_bytes);
    if (ret)
        return ret;
    return 0;
}

int mga1064_encode_start_address(uint32_t byte_offset, uint8_t *high,
                                 uint8_t *low, uint8_t *extended)
{
    uint32_t start;

    if (!high || !low || !extended || byte_offset % 8u)
        return -EINVAL;
    start = byte_offset / 8u;
    if (start > 0xfffffu)
        return -ERANGE;
    *high = (uint8_t)(start >> 8);
    *low = (uint8_t)start;
    *extended = (uint8_t)(start >> 16);
    return 0;
}

static uint32_t mga1064_read_start_bytes(struct mga1064_ctx *ctx)
{
    uint8_t high = mga_indexed_read(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                                    MGA_CRTC_START_HIGH);
    uint8_t low = mga_indexed_read(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                                   MGA_CRTC_START_LOW);
    uint8_t extended = mga_indexed_read(ctx, MGA_CRTCEXT_INDEX,
                                        MGA_CRTCEXT_DATA, MGA_CRTCEXT0);
    uint32_t start = ((uint32_t)(extended & 0x0f) << 16)
                   | ((uint32_t)high << 8) | low;

    return start * 8u;
}

static int mga1064_program_start(struct mga1064_ctx *ctx,
                                 uint32_t byte_offset)
{
    uint8_t high, low, extended;
    uint8_t crtcext0;
    int ret = mga1064_encode_start_address(byte_offset, &high, &low,
                                           &extended);

    if (ret)
        return ret;
    crtcext0 = mga_indexed_read(ctx, MGA_CRTCEXT_INDEX, MGA_CRTCEXT_DATA,
                                MGA_CRTCEXT0);
    mga_indexed_write(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                      MGA_CRTC_START_HIGH, high);
    mga_indexed_write(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                      MGA_CRTC_START_LOW, low);
    /* Datasheet 5.6.5: CRTCEXT0 contains the high nibble and must be last;
     * this write is what latches the new start address. */
    mga_indexed_write(ctx, MGA_CRTCEXT_INDEX, MGA_CRTCEXT_DATA,
                      MGA_CRTCEXT0,
                      (uint8_t)((crtcext0 & 0xf0) | extended));
    return 0;
}

static void mga1064_restore_start(struct mga1064_ctx *ctx)
{
    mga_indexed_write(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                      MGA_CRTC_START_HIGH, ctx->saved_start_high);
    mga_indexed_write(ctx, MGA_CRTC_INDEX, MGA_CRTC_DATA,
                      MGA_CRTC_START_LOW, ctx->saved_start_low);
    mga_indexed_write(ctx, MGA_CRTCEXT_INDEX, MGA_CRTCEXT_DATA,
                      MGA_CRTCEXT0, ctx->saved_crtcext0);
}

static int mga1064_find_device(struct l10gl_pci_device *device)
{
    return l10gl_pci_find(device, MGA_PCI_VENDOR_ID, mga_devices,
                          sizeof(mga_devices) / sizeof(mga_devices[0]));
}

int mga1064_probe(void)
{
    struct l10gl_pci_device device;

    return mga1064_find_device(&device) == 0;
}

/* ========================================================================
 * Memory Mapping
 * ========================================================================
 */

static void *map_mmio(struct l10gl_pci_device *dev, size_t *size_out)
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
    struct timespec start, now;
    int64_t elapsed_ns;

    clock_gettime(CLOCK_MONOTONIC, &start);
    /* Wait for vsync to end (not in retrace) */
    while (mga_read32(ctx, MGA_STATUS) & MGA_STATUS_VSYNCSTS) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000
                   + (int64_t)now.tv_nsec - start.tv_nsec;
        if (elapsed_ns >= 250000000)
            goto timeout;
    }
    /* Wait for vsync to start (entering retrace) */
    while (!(mga_read32(ctx, MGA_STATUS) & MGA_STATUS_VSYNCSTS)) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        elapsed_ns = (int64_t)(now.tv_sec - start.tv_sec) * 1000000000
                   + (int64_t)now.tv_nsec - start.tv_nsec;
        if (elapsed_ns >= 250000000)
            goto timeout;
    }
    return;

timeout:
    if (!ctx->vsync_timeout_warned) {
        fprintf(stderr, "MGA-1064SG: vsync wait timed out after 250ms; "
                        "page flips will continue unsynchronized\n");
        ctx->vsync_timeout_warned = 1;
    }
}

void mga1064_swap_buffers(struct mga1064_ctx *ctx)
{
    uint32_t new_scanout;
    uint32_t new_render;
    int ret;

    mga1064_wait_engine(ctx);
    if (!ctx->double_buffered)
        return;

    new_scanout = ctx->fb_offset;
    new_render = ctx->scanout_offset;
    mga1064_wait_vsync(ctx);
    ret = mga1064_program_start(ctx, new_scanout * (uint32_t)ctx->bpp);
    if (ret) {
        fprintf(stderr, "MGA-1064SG: cannot encode page-flip address: %s\n",
                strerror(-ret));
        return;
    }
    ctx->scanout_offset = new_scanout;
    ctx->fb_offset = new_render;
    mga_write32(ctx, MGA_YDSTORG, ctx->fb_offset);
}

/* ========================================================================
 * Global Engine Initialization (section 5.5.3)
 * ========================================================================
 */

static void engine_init_global(struct mga1064_ctx *ctx)
{
    /* PITCH: distance between scanlines in pixels */
    mga_write32(ctx, MGA_PITCH, ctx->pitch);

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
    uint32_t fbdev_offset_bytes = 0;

    if (width <= 0 || height <= 0 ||
        (bpp != 1 && bpp != 2 && bpp != 4)) {
        fprintf(stderr, "MGA-1064SG: requires positive geometry and "
                        "8-, 16-, or 32-bit pixels\n");
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;
    ctx->pitch = width;
    ctx->stride = (uint32_t)width * (uint32_t)bpp;
    ctx->bits_per_pixel = bpp * 8;
    ctx->fb_fd = -1;
    ctx->mem_fd = -1;

    /* Find the MGA-1064SG on the PCI bus.
     * The Mystique uses several PCI device IDs:
     *   0x051A - Mystique PCI (most common)
     *   0x051E - Mystique AGP
     *   0x0100 - Mystique (alternate ID on some boards)
     */
    struct l10gl_pci_device pci;
    int ret = mga1064_find_device(&pci);
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
    ctx->saved_start_high = mga_indexed_read(ctx, MGA_CRTC_INDEX,
                                             MGA_CRTC_DATA,
                                             MGA_CRTC_START_HIGH);
    ctx->saved_start_low = mga_indexed_read(ctx, MGA_CRTC_INDEX,
                                            MGA_CRTC_DATA,
                                            MGA_CRTC_START_LOW);
    ctx->saved_crtcext0 = mga_indexed_read(ctx, MGA_CRTCEXT_INDEX,
                                           MGA_CRTCEXT_DATA,
                                           MGA_CRTCEXT0);

    /* Map framebuffer via /dev/fb0 */
    ctx->fb_fd = open("/dev/fb0", O_RDWR);
    if (ctx->fb_fd >= 0) {
        struct l10gl_fbdev_mode mode;
        struct l10gl_pixel_format required;
        const struct l10gl_pixel_format *required_ptr = NULL;

        if (bpp != 1) {
            l10gl_pixel_format_standard(bpp * 8, &required);
            required_ptr = &required;
        }
        ret = l10gl_fbdev_negotiate(ctx->fb_fd, "MGA-1064SG", width,
                                    height, bpp * 8, required_ptr, &mode);
        if (ret) {
            close(ctx->fb_fd);
            ctx->fb_fd = -1;
            munmap(ctx->mmio, ctx->mmio_size);
            ctx->mmio = NULL;
            return ret;
        }
        ctx->using_fbdev = 1;
        ctx->width = (int)mode.var.xres;
        ctx->height = (int)mode.var.yres;
        ctx->bpp = (int)((mode.var.bits_per_pixel + 7u) / 8u);
        ctx->bits_per_pixel = (int)mode.var.bits_per_pixel;
        ctx->stride = mode.fix.line_length;
        if (ctx->stride % (uint32_t)ctx->bpp) {
            fprintf(stderr, "MGA-1064SG: fbdev stride %u is not a whole "
                            "number of %d-byte pixels\n",
                    ctx->stride, ctx->bpp);
            close(ctx->fb_fd);
            ctx->fb_fd = -1;
            munmap(ctx->mmio, ctx->mmio_size);
            ctx->mmio = NULL;
            return -ENOTSUP;
        }
        ctx->pitch = (int)(ctx->stride / (uint32_t)ctx->bpp);
        {
            uint64_t fbdev_offset = (uint64_t)mode.var.yoffset * ctx->stride
                                  + (uint64_t)mode.var.xoffset * ctx->bpp;

            if (fbdev_offset > UINT32_MAX) {
                fprintf(stderr, "MGA-1064SG: fbdev scanout offset is too "
                                "large\n");
                close(ctx->fb_fd);
                ctx->fb_fd = -1;
                munmap(ctx->mmio, ctx->mmio_size);
                ctx->mmio = NULL;
                return -EOVERFLOW;
            }
            fbdev_offset_bytes = (uint32_t)fbdev_offset;
        }
        ctx->fb_size = mode.fix.smem_len;
        ctx->fb = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fb_fd, 0);
    } else {
        perror("open /dev/fb0");
        /* Fallback: /dev/mem with MGABASE2 physical address */
        ctx->fb_fd = open("/dev/mem", O_RDWR | O_SYNC);
        if (ctx->fb_fd < 0) {
            perror("open /dev/mem");
            munmap(ctx->mmio, ctx->mmio_size);
            ctx->mmio = NULL;
            return -errno;
        }
        ctx->fb_size = pci.bar_size[1] ? pci.bar_size[1]
                                       : (size_t)width * height * bpp * 2u;
        ctx->fb = mmap(NULL, ctx->fb_size, PROT_READ | PROT_WRITE,
                       MAP_SHARED, ctx->fb_fd,
                       (off_t)ctx->mgabase2);
    }

    if (ctx->fb == MAP_FAILED) {
        perror("mmap framebuffer");
        ctx->fb = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        munmap(ctx->mmio, ctx->mmio_size);
        ctx->mmio = NULL;
        return -ENOMEM;
    }
    printf("  Framebuffer mapped: %zu bytes at %p\n", ctx->fb_size, ctx->fb);

    /* Compute a layout around the buffer the CRTC is actually scanning.
     * CRTC start addresses are 20-bit quantities in 8-byte units for the
     * 8/16/32bpp modes this driver accepts (datasheet 5.6.5). */
    {
        struct mga1064_buffer_layout layout;
        uint32_t front_bytes = mga1064_read_start_bytes(ctx);
        uint64_t surface_size64 = (uint64_t)ctx->stride * ctx->height;
        uint32_t surface_size;
        uint32_t used;
        uint32_t z_bytes;
        int layout_ret;

        if (ctx->fb_size > UINT32_MAX || surface_size64 > UINT32_MAX) {
            ret = -EOVERFLOW;
            goto layout_fail;
        }
        ctx->vram_size = (uint32_t)ctx->fb_size;
        surface_size = (uint32_t)surface_size64;
        if (front_bytes % 8u || front_bytes % (uint32_t)ctx->bpp ||
            (uint64_t)front_bytes + surface_size > ctx->vram_size) {
            if (ctx->using_fbdev && fbdev_offset_bytes % 8u == 0 &&
                fbdev_offset_bytes % (uint32_t)ctx->bpp == 0 &&
                (uint64_t)fbdev_offset_bytes + surface_size <=
                    ctx->vram_size) {
                fprintf(stderr, "MGA-1064SG: CRTC start 0x%x is outside "
                                "mapped VRAM; using fbdev offset 0x%x\n",
                        front_bytes, fbdev_offset_bytes);
                front_bytes = fbdev_offset_bytes;
            } else {
                fprintf(stderr, "MGA-1064SG: live scanout at 0x%x does not "
                                "fit the framebuffer mapping\n", front_bytes);
                ret = -EINVAL;
                goto layout_fail;
            }
        } else if (ctx->using_fbdev && front_bytes != fbdev_offset_bytes) {
            fprintf(stderr, "MGA-1064SG: CRTC scanout 0x%x differs from "
                            "fbdev offset 0x%x; trusting the CRTC\n",
                    front_bytes, fbdev_offset_bytes);
        }

        ctx->console_offset = front_bytes / (uint32_t)ctx->bpp;
        ctx->scanout_offset = ctx->console_offset;
        layout_ret = ctx->using_fbdev
            ? mga1064_plan_double_buffer(front_bytes, ctx->stride,
                                         (uint32_t)ctx->height,
                                         (uint32_t)ctx->bpp, ctx->vram_size,
                                         &layout)
            : -ENOTSUP;
        if (layout_ret == 0) {
            ctx->saved_console = malloc(layout.surface_bytes);
            if (!ctx->saved_console) {
                ret = -ENOMEM;
                goto layout_fail;
            }
            memcpy(ctx->saved_console, (uint8_t *)ctx->fb + front_bytes,
                   layout.surface_bytes);
            ctx->saved_console_size = layout.surface_bytes;
            ctx->fb_offset = layout.back_bytes / (uint32_t)ctx->bpp;
            ctx->z_offset = layout.z_bytes / (uint32_t)ctx->bpp;
            ctx->double_buffered = 1;
            printf("MGA-1064SG: double buffering enabled: front 0x%x, "
                   "back 0x%x, Z 0x%x (%u bytes each)\n",
                   layout.front_bytes, layout.back_bytes, layout.z_bytes,
                   layout.surface_bytes);
        } else {
            used = front_bytes;
            ret = find_free_surface(ctx->vram_size, surface_size, &used, 1,
                                    &z_bytes);
            if (ret) {
                fprintf(stderr, "MGA-1064SG: mode needs color+Z storage "
                                "beyond %u-byte framebuffer mapping\n",
                        ctx->vram_size);
                goto layout_fail;
            }
            ctx->fb_offset = ctx->console_offset;
            ctx->z_offset = z_bytes / (uint32_t)ctx->bpp;
            printf("MGA-1064SG: double buffering unavailable (%s); using "
                   "single-buffered scanout\n", strerror(-layout_ret));
        }
        goto layout_done;

layout_fail:
        free(ctx->saved_console);
        ctx->saved_console = NULL;
        munmap(ctx->fb, ctx->fb_size);
        ctx->fb = NULL;
        close(ctx->fb_fd);
        ctx->fb_fd = -1;
        munmap(ctx->mmio, ctx->mmio_size);
        ctx->mmio = NULL;
        return ret;
layout_done:
        ;
    }

    /* Initialize drawing engine */
    engine_init_global(ctx);

    printf("MGA-1064SG: Drawing engine initialized.\n");
    printf("  Screen: %dx%d, %d bpp (stride %u, pitch %d pixels)\n",
           ctx->width, ctx->height, ctx->bits_per_pixel, ctx->stride,
           ctx->pitch);
    printf("  Render offset: %u px, scanout offset: %u px, Z offset: %u px\n",
           ctx->fb_offset, ctx->scanout_offset, ctx->z_offset);

    return 0;
}

void mga1064_cleanup(struct mga1064_ctx *ctx)
{
    mga1064_wait_engine(ctx);

    if (ctx->double_buffered && ctx->saved_console) {
        memcpy((uint8_t *)ctx->fb
                   + (size_t)ctx->console_offset * (size_t)ctx->bpp,
               ctx->saved_console, ctx->saved_console_size);
        mga1064_wait_vsync(ctx);
        mga1064_restore_start(ctx);
    }
    free(ctx->saved_console);
    ctx->saved_console = NULL;

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
