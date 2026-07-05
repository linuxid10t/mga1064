/*
 * mga1064.h - Register definitions and API for the Matrox MGA-1064SG
 *            (Mystique) drawing engine.
 *
 * Based on: Matrox MGA-1064SG Developer Specification, Feb 10, 1997
 *           Document 10524-MS-0100
 *
 * This is a userspace fbdev driver — no DRM, no DRI, no kernel module.
 * We talk directly to the hardware via /dev/fb0 (framebuffer) and
 * /sys/bus/pci/devices/.../resource0 (MMIO aperture).
 *
 * Target: 32-bit x86 (i686+), Linux fbdev console.
 */

#ifndef MGA1064_H
#define MGA1064_H

#include <stdint.h>

/* ========================================================================
 * PCI Identification
 * ======================================================================== */

/*
 * Matrox PCI vendor ID.  All Matrox graphics devices use 0x102B.
 */
#define MGA_PCI_VENDOR_ID  0x102B

/*
 * The MGA-1064SG (Mystique) PCI device IDs:
 *   0x051A - Mystique PCI (this is the one we want)
 *   0x051E - Mystique AGP
 *   0x0100 - Mystique (alternate ID seen on some boards)
 *
 * NOTE: 0x0519 is the MILLENNIUM (MGA-2064W), NOT the Mystique!
 *
 * The PCI subsystem ID distinguishes board variants (SGRAM vs SDRAM,
 * 2MB vs 4MB) but the core drawing engine is identical.
 */
#define MGA_PCI_DEVICE_1064SG_PCI   0x051A
#define MGA_PCI_DEVICE_1064SG_AGP   0x051E
#define MGA_PCI_DEVICE_1064SG_ALT   0x0100

/* ========================================================================
 * PCI Configuration Space (offsets within config header)
 * ======================================================================== */

/* BAR0: MGA Control Aperture (MGABASE1) — registers */
#define PCI_BAR0  0x10
/* BAR1: MGA Frame Buffer Aperture (MGABASE2) — framebuffer */
#define PCI_BAR1  0x14
/* BAR2: MGA ILOAD Aperture (MGABASE3) — pseudo-DMA window */
#define PCI_BAR2  0x18

/* ========================================================================
 * Register Offsets within MGABASE1 (Control Aperture)
 *
 * All registers are 32-bit (dword) aligned.  Register addresses marked
 * (4) in the datasheet require dword access.
 * ========================================================================
 */

/* --- Drawing Engine Registers (1C00h - 1CFFh) --- */

#define MGA_DWGCTL     0x1C00  /* Drawing Control (WO, FIFO, STATIC) */
#define MGA_MACCESS    0x1C04  /* Memory Access (pixel format, dither) */
#define MGA_MCTLWTST   0x1C08  /* Memory Control Wait State */
#define MGA_ZORG       0x1C0C  /* Z-Depth Origin (base addr of Z buffer) */

#define MGA_PAT0       0x1C10  /* Pattern register 0 */
#define MGA_PAT1       0x1C14  /* Pattern register 1 */
/* 0x1C18 - Reserved */
#define MGA_PLNWT      0x1C1C  /* Plane Write Mask */

#define MGA_BCOL       0x1C20  /* Background Color / Blit Color Mask */
#define MGA_FCOL       0x1C24  /* Foreground Color / Blit Color Key */
/* 0x1C28 - Reserved */
/* 0x1C2C - Reserved (SRCBLT) */

#define MGA_SRC0       0x1C30  /* Source 0 */
#define MGA_SRC1       0x1C34  /* Source 1 */
#define MGA_SRC2       0x1C38  /* Source 2 */
#define MGA_SRC3       0x1C3C  /* Source 3 */

#define MGA_XYSTRT     0x1C40  /* XY Start Address */
#define MGA_XYEND      0x1C44  /* XY End Address */
/* 0x1C48-0x1C4C - Reserved */

#define MGA_SHIFT      0x1C50  /* Funnel Shifter Control */
#define MGA_DMAPAD     0x1C54  /* DMA Padding */
#define MGA_SGN        0x1C58  /* Sign */
#define MGA_LEN        0x1C5C  /* Length */

#define MGA_AR0        0x1C60  /* Multi-Purpose Address 0 */
#define MGA_AR1        0x1C64  /* Multi-Purpose Address 1 */
#define MGA_AR2        0x1C68  /* Multi-Purpose Address 2 */
#define MGA_AR3        0x1C6C  /* Multi-Purpose Address 3 */
#define MGA_AR4        0x1C70  /* Multi-Purpose Address 4 */
#define MGA_AR5        0x1C74  /* Multi-Purpose Address 5 */
#define MGA_AR6        0x1C78  /* Multi-Purpose Address 6 */
/* 0x1C7C - Reserved */

#define MGA_CXBNDRY    0x1C80  /* Clipper X Boundary */
#define MGA_FXBNDRY    0x1C84  /* X Address (Boundary) — FXRIGHT|FXLEFT */
#define MGA_YDSTLEN    0x1C88  /* Y Destination and Length — yval|length */
#define MGA_PITCH      0x1C8C  /* Memory Pitch */
#define MGA_YDST       0x1C90  /* Y Address */
#define MGA_YDSTORG    0x1C94  /* Memory Origin (screen start in VRAM) */
#define MGA_YTOP       0x1C98  /* Clipper Y Top Boundary */
#define MGA_YBOT       0x1C9C  /* Clipper Y Bottom Boundary */

#define MGA_CXLEFT     0x1CA0  /* Clipper X Minimum Boundary */
#define MGA_CXRIGHT    0x1CA4  /* Clipper X Maximum Boundary */
#define MGA_FXLEFT     0x1CA8  /* X Address (Left) */
#define MGA_FXRIGHT    0x1CAC  /* X Address (Right) */
#define MGA_XDST       0x1CB0  /* X Destination Address */
/* 0x1CB4-0x1CBC - Reserved */

/* --- Data ALU Registers (DR0 - DR15) --- */
/* For Gouraud-shaded TRAP with Z:                                   */
/*   DR0  = Z start value at left edge (17.15 fixed point)           */
/*   DR2  = dZ/dx (Z increment per pixel in X, 17.15 fixed point)    */
/*   DR3  = dZ/dy (Z increment per line in Y, 17.15 fixed point)     */
/*   DR4  = Red start value at left edge (9.15 fixed point)          */
/*   DR6  = dR/dx (red increment per pixel in X, 9.15 fixed point)   */
/*   DR7  = dR/dy (red increment per line in Y, 9.15 fixed point)    */
/*   DR8  = Green start value at left edge (9.15 fixed point)        */
/*   DR10 = dG/dx (green increment per pixel in X, 9.15 fixed point) */
/*   DR11 = dG/dy (green increment per line in Y, 9.15 fixed point)  */
/*   DR12 = Blue start value at left edge (9.15 fixed point)         */
/*   DR14 = dB/dx (blue increment per pixel in X, 9.15 fixed point)  */
/*   DR15 = dB/dy (blue increment per line in Y, 9.15 fixed point)  */

#define MGA_DR0        0x1CC0  /* Z start / current Z value */
/* 0x1CC4 - Reserved (DR1) */
#define MGA_DR2        0x1CC8  /* dZ/dx */
#define MGA_DR3        0x1CCC  /* dZ/dy */
#define MGA_DR4        0x1CD0  /* Red start */
/* 0x1CD4 - Reserved (DR5) */
#define MGA_DR6        0x1CD8  /* dR/dx */
#define MGA_DR7        0x1CDC  /* dR/dy */
#define MGA_DR8        0x1CE0  /* Green start */
/* 0x1CE4 - Reserved (DR9) */
#define MGA_DR10       0x1CE8  /* dG/dx */
#define MGA_DR11       0x1CEC  /* dG/dy */
#define MGA_DR12       0x1CF0  /* Blue start */
/* 0x1CF4 - Reserved (DR13) */
#define MGA_DR14       0x1CF8  /* dB/dx */
#define MGA_DR15       0x1CFC  /* dB/dy */

/*
 * Register range 0x1D00-0x1DFF mirrors 0x1C00-0x1CFF.
 * Writing to this range also STARTS the drawing engine.
 * The last register write of any drawing operation MUST go here.
 */
#define MGA_EXEC_OFFSET 0x0100  /* Add to register offset to trigger exec */

/* --- Status / Interrupt Registers --- */

#define MGA_FIFOSTATUS 0x1E10  /* Bus FIFO Status (RO) */
#define MGA_STATUS     0x1E14  /* Status (RO) */
#define MGA_ICLEAR     0x1E18  /* Interrupt Clear (WO) */
#define MGA_IEN        0x1E1C  /* Interrupt Enable (R/W) */
#define MGA_VCOUNT     0x1E20  /* Vertical Count (RO) */

#define MGA_OPMODE     0x1E54  /* Operating Mode (R/W) */

/* --- Pseudo-DMA / Indirect Write Registers --- */

#define MGA_DWG_INDIR_WT_BASE 0x1E80  /* DWG_INDIR_WT<0..15> */

/* ========================================================================
 * DWGCTL Register Bit Field Definitions
 * ======================================================================== */

/* opcod field (bits 3:0) — Operation Code */
#define MGA_OPCOD_LINE_OPEN       0x0
#define MGA_OPCOD_AUTOLINE_OPEN   0x1
#define MGA_OPCOD_LINE_CLOSE      0x2
#define MGA_OPCOD_AUTOLINE_CLOSE  0x3
#define MGA_OPCOD_TRAP            0x4   /* Trapezoid / Rectangle fill */
#define MGA_OPCOD_TRAP_ILOAD      0x5   /* Trapezoid with host data (textured) */
#define MGA_OPCOD_BITBLT          0x8
#define MGA_OPCOD_ILOAD           0x9   /* Host -> RAM image load */
#define MGA_OPCOD_ILOAD_SCALE     0xA
#define MGA_OPCOD_ILOAD_FILTER    0xB
#define MGA_OPCOD_IDUMP           0xC   /* RAM -> Host image dump */
#define MGA_OPCOD_ILOAD_HIQH      0xD
#define MGA_OPCOD_ILOAD_HIQHV     0xE

/* atype field (bits 6:4) — Access Type (how pixels are written to RAM) */
#define MGA_ATYPE_RPL             (0x0 << 4)  /* Write (replace) */
#define MGA_ATYPE_RSTR            (0x1 << 4)  /* Read-modify-write (raster OP) */
#define MGA_ATYPE_ZI              (0x4 << 4)  /* Depth mode with Gouraud */
#define MGA_ATYPE_BLK             (0x6 << 4)  /* Block write mode (SGRAM) */
#define MGA_ATYPE_I               (0x6 << 4)  /* Gouraud with depth compare */
       /* NOTE: ZI=100b, I=110b — datasheet table is 3-bit value at bits 6:4 */

/* The datasheet's atype table values are the raw 3-bit codes: */
#define MGA_ATYPE_VAL_RPL   0x0  /* 000: Replace */
#define MGA_ATYPE_VAL_RSTR  0x1  /* 001: Raster OP (read-modify-write) */
#define MGA_ATYPE_VAL_ZI    0x4  /* 100: Depth mode with Gouraud */
#define MGA_ATYPE_VAL_BLK   0x5  /* 101: Block write */
#define MGA_ATYPE_VAL_I     0x6  /* 110: Gouraud with depth compare */

/* linear field (bit 7) */
#define MGA_LINEAR         (1 << 7)

/* zmode field (bits 10:8) — Z comparison function */
#define MGA_ZMODE_NOZCMP   (0x0 << 8)  /* Always write (no Z test) */
#define MGA_ZMODE_ZE       (0x2 << 8)  /* Draw when Z ==  */
#define MGA_ZMODE_ZNE      (0x3 << 8)  /* Draw when Z !=  */
#define MGA_ZMODE_ZLT      (0x4 << 8)  /* Draw when Z <   */
#define MGA_ZMODE_ZLTE     (0x5 << 8)  /* Draw when Z <=  */
#define MGA_ZMODE_ZGT      (0x6 << 8)  /* Draw when Z >   */
#define MGA_ZMODE_ZGTE     (0x7 << 8)  /* Draw when Z >=  */

/* solid field (bit 11) — force all SRC = 0xFFFFFFFF */
#define MGA_SOLID          (1 << 11)

/* arzero field (bit 12) — zero all AR registers */
#define MGA_ARZERO         (1 << 12)

/* sgnzero field (bit 13) — zero the SGN register */
#define MGA_SGNZERO        (1 << 13)

/* shftzero field (bit 14) — zero the SHIFT register */
#define MGA_SHFTZERO       (1 << 14)

/* bop field (bits 19:16) — Boolean operation (raster op) */
#define MGA_BOP_ZERO       (0x0 << 16)  /* 0 */
#define MGA_BOP_AND        (0x1 << 16)  /* S & D */
#define MGA_BOP_ANDNOT     (0x2 << 16)  /* ~S & D (D & ~S) */
#define MGA_BOP_NOTCOPY    (0x3 << 16)  /* ~S */
#define MGA_BOP_NOTAND     (0x4 << 16)  /* ~D & S ((~D) & S) */
#define MGA_BOP_XOR        (0x6 << 16)  /* D ^ S */
#define MGA_BOP_AND_R      (0x8 << 16)  /* S & D */
#define MGA_BOP_NOTXOR     (0x9 << 16)  /* ~(D ^ S) */
#define MGA_BOP_NOP        (0xA << 16)  /* D (no-op) */
#define MGA_BOP_MERGESRC   (0xB << 16)  /* (~D) | S */
#define MGA_BOP_COPY       (0xC << 16)  /* S (copy source) */
#define MGA_BOP_MERGEDST   (0xD << 16)  /* (~S) | D */
#define MGA_BOP_OR         (0xE << 16)  /* S | D */
#define MGA_BOP_ONE        (0xF << 16)  /* 1 */

/* trans field (bits 23:20) — Translucency (screen-door pattern) */
#define MGA_TRANS_DISABLE  (0x0 << 20)  /* Fully opaque */

/* bltmod field (bits 29:28) — Blit mode / source format */
#define MGA_BLTMOD_BM0MONO  (0x0 << 28) /* Monochrome source expansion */
#define MGA_BLTMOD_BM0CHAR  (0x1 << 28) /* Character source */
#define MGA_BLTMOD_BFCOL    (0x2 << 28) /* Color source (24/32 bpp) */
#define MGA_BLTMOD_BUCOL    (0x3 << 28) /* Packed color source */

/* pattern field (bit 30) */
#define MGA_PATTERN        (1 << 30)

/* transc field (bit 31) — transparency control */
#define MGA_TRANSC         (1 << 31)

/* ========================================================================
 * MACCESS Register Bit Field Definitions
 * ======================================================================== */

/* pwidth field (bits 1:0) — Pixel Width */
#define MGA_PWIDTH_8       0x0   /* PW8:  8 bpp */
#define MGA_PWIDTH_16      0x1   /* PW16: 16 bpp */
#define MGA_PWIDTH_32      0x2   /* PW32: 32 bpp */
#define MGA_PWIDTH_24      0x3   /* PW24: 24 bpp */

/* rcdelay field (bit 8) */
#define MGA_RCDELAY        (1 << 8)

/* jedecrst field (bit 14) */
#define MGA_JEDECRST       (1 << 14)

/* memreset field (bit 15) */
#define MGA_MEMRESET       (1 << 15)

/* dit555 field (bit 31) — Dither 5:5:5 mode */
#define MGA_DIT555         (1 << 31)

/* nodither field (bit 30) */
#define MGA_NODITHER       (1 << 30)

/* tlutload field (bit 29) — Texture LUT load enable */
#define MGA_TLUTLOAD       (1 << 29)

/* ========================================================================
 * STATUS Register Bit Definitions
 * ======================================================================== */

#define MGA_STATUS_VSYNCSTS    (1 << 3)   /* In vertical sync */
#define MGA_STATUS_DWGENGSTS   (1 << 16)  /* Drawing engine busy */

/* ========================================================================
 * Fixed-Point Format Helpers
 *
 * The 1064 uses fixed-point for interpolation values:
 *   Z values (DR0, DR2, DR3): signed 17.15 (32-bit, 15 fractional bits)
 *   Color values (DR4-DR15):  signed 9.15  (24-bit value in bits 23:0,
 *                                            15 fractional bits)
 *
 * To convert a float to the fixed-point representation:
 *   fixed = (int32_t)(floatval * (1 << 15))
 * ======================================================================== */

#define MGA_Z_FRAC_BITS    15
#define MGA_Z_FIXED(x)     ((int32_t)((x) * (float)(1 << MGA_Z_FRAC_BITS)))

#define MGA_COLOR_FRAC_BITS 15
#define MGA_COLOR_FIXED(x)  ((int32_t)((x) * (float)(1 << MGA_COLOR_FRAC_BITS)))

/* ========================================================================
 * Framebuffer / Screen Geometry
 *
 * The 1064 addressing in Power Graphic mode:
 *   - The screen is addressed as a linear sequence of lines in VRAM.
 *   - YDSTORG specifies the VRAM offset (in pixels) of the top-left corner.
 *   - PITCH specifies the distance (in pixels) between consecutive lines.
 *   - YDST (via YDSTLEN) specifies the Y coordinate of the line to draw.
 *   - pixel_addr = YDSTORG + YDST * PITCH + X
 *
 * For a standard framebuffer at VRAM offset 0:
 *   YDSTORG = 0
 *   PITCH   = screen_width_in_pixels
 *
 * Z buffer goes at the end of the visible framebuffer memory:
 *   ZORG = (pitch * height_in_pixels)
 * ======================================================================== */

/* ========================================================================
 * API
 * ======================================================================== */

struct mga1064_ctx {
    /* PCI / memory mapping */
    int     fb_fd;          /* /dev/fb0 file descriptor */
    int     mem_fd;         /* /dev/mem or resource0 fd for MMIO */
    void   *fb;             /* mmap'd framebuffer (linear) */
    void   *mmio;           /* mmap'd MGABASE1 control aperture */
    size_t  fb_size;        /* framebuffer mapping size */
    size_t  mmio_size;      /* MMIO mapping size */

    /* PCI BAR addresses (physical) */
    uint32_t mgabase1;      /* Control aperture (registers) */
    uint32_t mgabase2;      /* Frame buffer aperture */
    uint32_t mgabase3;      /* ILOAD aperture */

    /* Screen geometry */
    int     width;          /* Screen width in pixels */
    int     height;         /* Screen height in pixels */
    int     bpp;            /* Bytes per pixel (NOT bits!) */
    int     pitch;          /* Pitch in pixels (= width for linear) */

    /* Memory layout */
    uint32_t vram_size;     /* Total VRAM in bytes */
    uint32_t fb_offset;     /* Pixel offset of framebuffer origin in VRAM */
    uint32_t z_offset;      /* Pixel offset of Z buffer origin in VRAM */
};

/*
 * mga1064_init - Find the Mystique, map memory, initialize drawing engine.
 * @ctx:     Context to initialize.
 * @width:   Desired screen width (should match current fbdev mode).
 * @height:  Desired screen height.
 * @bpp:     Bytes per pixel (2 = 16bpp, 4 = 32bpp).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * This function:
 *   1. Finds the MGA-1064SG via /sys/bus/pci/devices.
 *   2. Reads BAR0/BAR1/BAR2 from PCI config space.
 *   3. mmaps the framebuffer (via /dev/fb0) and MMIO (via /dev/mem).
 *   4. Sets up the drawing engine global registers for 3D rendering.
 *
 * The caller is responsible for having a working fbdev console at the
 * requested resolution.  This driver does NOT do modesetting — it assumes
 * the BIOS or kernel fbdev driver has already established the video mode.
 */
int mga1064_init(struct mga1064_ctx *ctx, int width, int height, int bpp);

/*
 * mga1064_cleanup - Unmap memory, close fds.
 */
void mga1064_cleanup(struct mga1064_ctx *ctx);

/*
 * mga1064_wait_engine - Block until the drawing engine is idle.
 *
 * Polls STATUS<dwgengsts> until the engine, BFIFO, and memory controller
 * are all finished.  Must be called before reading the framebuffer
 * directly (e.g., for CPU fallbacks) after a draw.
 */
void mga1064_wait_engine(struct mga1064_ctx *ctx);

/*
 * mga1064_wait_vsync - Block until vertical retrace.
 *
 * Polls STATUS<vsyncsts>.  Useful for double-buffering or tear-free
 * page flips.
 */
void mga1064_wait_vsync(struct mga1064_ctx *ctx);

/*
 * mga1064_clear_z - Clear the Z buffer to a given value.
 * @ctx:  Driver context.
 * @z:    Z value to fill (0.0 to 1.0, where 1.0 = farthest).
 *
 * Uses a fast rectangle fill to write the Z buffer.
 * Must be called before rendering a new frame.
 */
void mga1064_clear_z(struct mga1064_ctx *ctx, float z);

/*
 * mga1064_fill_rect - Hardware-accelerated rectangle fill.
 * @ctx:    Driver context.
 * @x, y:   Top-left corner (in pixels).
 * @w, h:   Width and height (in pixels).
 * @color:  Color in the current pixel format.
 *
 * Draws a solid rectangle using the TRAP opcod with RPL atype.
 */
void mga1064_fill_rect(struct mga1064_ctx *ctx, int x, int y, int w, int h,
                       uint32_t color);

/*
 * mga1064_draw_triangle_gouraud - Draw a Gouraud-shaded, Z-tested triangle.
 * @ctx:       Driver context.
 * @v0, v1, v2: Triangle vertices (screen coords + color + depth).
 *
 * Uses the TRAP opcod with ZI atype for hardware Gouraud shading and
 * Z-buffering.  The triangle is decomposed into a flat-top + flat-bottom
 * pair of trapezoids (or a single trapezoid for axis-aligned cases).
 */
struct mga_vertex {
    float x, y;       /* Screen coordinates (pixels) */
    float z;          /* Depth value (0.0 = near, 1.0 = far) */
    float w;          /* 1/Z_eye (unused on 1064, kept for API compatibility) */
    float r, g, b;    /* Color (0.0 to 1.0) */
    float u, v;       /* Texture coords (unused on 1064, kept for compat) */
};

void mga1064_draw_triangle_gouraud(struct mga1064_ctx *ctx,
                                    struct mga_vertex v0,
                                    struct mga_vertex v1,
                                    struct mga_vertex v2);

/*
 * mga1064_draw_line - Hardware-accelerated line draw.
 */
void mga1064_draw_line(struct mga1064_ctx *ctx,
                        int x0, int y0, int x1, int y1, uint32_t color);

/*
 * Inline MMIO accessors.
 * mmio is mapped as a flat array of 32-bit words.
 */
static inline void mga_write32(struct mga1064_ctx *ctx,
                               uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)((char *)ctx->mmio + offset);
    *reg = value;
}

static inline uint32_t mga_read32(struct mga1064_ctx *ctx, uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)((char *)ctx->mmio + offset);
    return *reg;
}

/*
 * mga_exec - Write a register to the EXEC range to start drawing.
 *
 * The drawing engine only starts when the final register write lands
 * in the 0x1D00-0x1DFF range (which mirrors 0x1C00-0x1CFF).  We always
 * kick the engine by writing YDSTLEN to the exec range.
 */

#endif /* MGA1064_H */
