/*
 * virge.h - Register definitions and API for the S3 ViRGE (86C325)
 *           integrated 3D graphics accelerator.
 *
 * Based on: S3 ViRGE Integrated 3D Accelerator datasheet
 *           Document DB019-B, August 1996
 *
 * This is a userspace fbdev driver — no DRM, no DRI, no kernel module.
 * We talk directly to the hardware via PCI MMIO (new MMIO method,
 * offset 0x1000000+ from BAR0).
 *
 * The ViRGE's S3d Engine provides a native 3D triangle primitive with
 * hardware Gouraud shading, Z-buffering, texture mapping, perspective
 * correction, bilinear/trilinear filtering, MIP-mapping, alpha blending,
 * and fogging. This far exceeds the Matrox MGA-1064SG's capabilities.
 *
 * Target: 32-bit x86 (i686+), Linux fbdev console, PCI bus.
 */

#ifndef VIRGE_H
#define VIRGE_H

#include <stdint.h>

/* ========================================================================
 * PCI Identification
 * ======================================================================== */

/*
 * S3 Incorporated PCI vendor ID.  All S3 graphics devices use 0x5333.
 */
#define S3_PCI_VENDOR_ID   0x5333

/*
 * ViRGE PCI device IDs. The core S3d Engine register interface is
 * compatible across all of these variants.
 */
#define S3_PCI_DEVICE_VIRGE        0x5631  /* ViRGE (initial stepping) */
#define S3_PCI_DEVICE_VIRGE_VX     0x883d  /* ViRGE/VX */
#define S3_PCI_DEVICE_VIRGE_DXGX   0x8a01  /* ViRGE/DX, ViRGE/GX */
#define S3_PCI_DEVICE_VIRGE_GX2    0x8a10  /* ViRGE/GX2 */
#define S3_PCI_DEVICE_VIRGE_MX     0x8c00  /* ViRGE/MX */
#define S3_PCI_DEVICE_VIRGE_MXPLUS 0x8c01  /* ViRGE/MX+ */

/* All known ViRGE device IDs, for probing. */
#define S3_PCI_DEVICE_VIRGE_ALL { \
    S3_PCI_DEVICE_VIRGE, S3_PCI_DEVICE_VIRGE_VX, S3_PCI_DEVICE_VIRGE_DXGX, \
    S3_PCI_DEVICE_VIRGE_GX2, S3_PCI_DEVICE_VIRGE_MX, S3_PCI_DEVICE_VIRGE_MXPLUS }

/* ========================================================================
 * PCI Configuration Space
 * ======================================================================== */

/*
 * BAR0: 64MB memory-mapped aperture.
 *   Bits 31-26 are relocatable (64MB granularity).
 *   Contains:
 *     0x0000000 - 0x0FFFFFF  Linear framebuffer (up to 4MB usable)
 *     0x1000000 - 0x1007FFF  Image data transfer (32KB)
 *     0x1008000 +             MMIO registers
 *
 * We mmap resource0 and access registers at offset 0x1000000+.
 */
#define PCI_BAR0            0x10

/* Offset from BAR0 start to the MMIO register region */
#define VIRGE_MMIO_OFFSET   0x1000000

/* ========================================================================
 * S3d Engine Register Offsets
 *
 * All offsets are relative to the MMIO register region (BAR0 + 0x1000000).
 * Register addresses are shown as in the datasheet (e.g., B500H) with
 * the 0x10 prefix implied by the MMIO offset.
 *
 * Each command type (BitBLT, 2D Line, 3D Triangle, etc.) has its own
 * register bank at a unique offset. Shared registers (DEST_BASE, CLIP,
 * STRIDE) appear in every bank at the same sub-offset but different base.
 * ======================================================================== */

/* --- Subsystem Status / Advanced Function Control --- */

#define VIRGE_SUBSYS_STATUS     0x8504  /* Subsystem Status (RO) */
#define VIRGE_ADV_FUNC_CTRL     0x850C  /* Advanced Function Control (R/W) */

/* Subsystem Status bit definitions */
/* S3d Engine status: bit 13 SET = idle, CLEAR = busy (datasheet §22).
 * Inverted from what the name might suggest -- do not read this as
 * "busy when set". */
#define VIRGE_STATUS_3DIDLE     (1 << 13)
/* Bit 0 is VSY INT — a LATCHED vertical-sync interrupt status, NOT a
 * live "in retrace" level (DB019-B §22, PDF p.299). Once vsync occurs
 * it reads 1 forever until software writes VSY CLR (Subsystem Control
 * bit 0). Polling it for a 0->1 edge without clearing first spins
 * forever — the scantest phase-2 hang on real hardware (2026-07-06). */
#define VIRGE_STATUS_VSYNC      (1 << 0)

/* Advanced Function Control bit definitions.
 * Bit 0 enables the 8514/A-compatible accelerated register interface —
 * inherited unchanged from the whole S3 8514/A-compatible lineage
 * (86C8xx through ViRGE). Until it's set, the 2D/3D command register
 * bank does not respond to writes at all; only the linear framebuffer
 * aperture is live. Bit 0 here is OR'd with CR66 bit 0 (either enables
 * it), per datasheet §15.1. */
#define VIRGE_AFC_ENABLE        (1 << 0)

/* Advanced Function Control bit 4: Enable Linear Addressing (DB019-B
 * PDF p. 22-4 / Appendix B.8 register table). Separate from CR31's
 * ENH_MAP: that bit governs how the S3d engine's Memory Port
 * Controller addresses VRAM for engine writes, while this bit governs
 * whether the CPU-side BAR0 aperture itself is a true linear window
 * into VRAM or the legacy segmented/banked VGA view. Never previously
 * set -- if clear, CPU reads/writes through ctx->fb could be hitting a
 * banked window while the S3d engine's DEST_BASE writes land in real
 * linear VRAM addresses the CPU view never sees, which would explain
 * engine fills "completing" (per SUBSYS_STATUS) while never appearing
 * in the linear-aperture readback used by every diagnostic so far. */
#define VIRGE_AFC_LINEAR_ADDR   (1 << 4)

/* ========================================================================
 * Legacy VGA CRTC Registers (accessed via I/O ports 0x3D4/0x3D5)
 *
 * CR53's MMIO_SELECT field is the master gate for the entire MMIO
 * aperture at BAR0 + VIRGE_MMIO_OFFSET: until it selects "new MMIO",
 * that whole region is dead silicon and every register write there
 * (including VIRGE_ADV_FUNC_CTRL) is silently dropped. PCI ViRGE cards
 * are supposed to power up with this already selected, but firmware,
 * a prior driver, or a warm reset can leave it disabled — datasheet
 * §15.1.2.
 *
 * Extended CRTC registers (CR30+, which includes CR53/CR66) are locked
 * against writes by default on this chip family and must be unlocked
 * via CR38/CR39 first.
 * ======================================================================== */

#define VIRGE_VGA_CRTC_INDEX    0x3D4
#define VIRGE_VGA_CRTC_DATA     0x3D5

#define VIRGE_CR38_UNLOCK_REG   0x38   /* Unlocks CR30-CR3F */
#define VIRGE_CR38_UNLOCK_KEY   0x48
#define VIRGE_CR39_UNLOCK_REG   0x39   /* Unlocks CR40-CRFF */
#define VIRGE_CR39_UNLOCK_KEY   0xA5

/* CR36 "Configuration Register 1" — samples reset/strap state from the
 * PD bus at reset. Readable without unlock; writable only after the
 * CR39 key above.
 *
 * The MEM SIZE field is in a DIFFERENT bit position per chip family:
 *   base 86C325, DX, GX: bits 7-5 (DB019-B PDF p.197) — 000 = 4MB,
 *       100 = 2MB, all other values reserved.
 *   ViRGE/VX (86C375): bits 6-5 (DB025-A §18) — 00 = 2MB, 01 = 4MB,
 *       10 = 6MB, 11 = 8MB. Bit 7 on the VX is a separate strap
 *       (8-column block-write support), so its field is 2 bits, not 3.
 * virge_detect_vram dispatches on device id and reads the right field;
 * GX2/MX/MX+ layouts are undocumented (no datasheet) and fall back. */
#define VIRGE_CR36                  0x36
#define VIRGE_CR36_MEM_SIZE_SHIFT   5            /* both fields start at bit 5 */
#define VIRGE_CR36_MEM_SIZE_MASK    (0x7u << 5)  /* base/DX/GX: bits 7-5 */
#define VIRGE_CR36_VX_MEM_SIZE_MASK (0x3u << 5)  /* VX: bits 6-5 (DB025-A) */

#define VIRGE_CR53              0x53   /* Extended Memory Control 1 */
#define VIRGE_CR53_MMIO_MASK    (0x3 << 3)
#define VIRGE_CR53_MMIO_NONE    (0x0 << 3)  /* Disabled (VL-Bus power-on default) */
#define VIRGE_CR53_MMIO_NEW     (0x1 << 3)  /* New MMIO — PCI power-on default */
#define VIRGE_CR53_MMIO_OLD     (0x2 << 3)  /* Old/Trio64-style legacy window */
#define VIRGE_CR53_MMIO_BOTH    (0x3 << 3)

#define VIRGE_CR66               0x66   /* Mirrors AFC bit 0 (ENB EHFC) */
#define VIRGE_CR66_ENB_EHFC      (1 << 0)

/* CR31 "Memory Configuration Register" bit 3: ENH MAP (Use Enhanced
 * Mode Memory Mapping). 0 = IBM VGA planar/chain-4 memory mapping;
 * 1 = linear doubleword addressing. The datasheet is explicit that
 * enhanced mode operation requires BOTH CR66 bit 0 = 1 AND this bit
 * set. If clear, the S3d engine's Memory Port Controller applies
 * legacy VGA planar address decode to engine writes, while the CPU's
 * linear framebuffer aperture (BAR0+0) may still read out fine via a
 * separate linear addressing path -- meaning engine fills can
 * "succeed" while landing in a completely different physical layout
 * than what's visible through the linear aperture. */
#define VIRGE_CR31               0x31
#define VIRGE_CR31_ENH_MAP       (1 << 3)

/* CR40 "System Configuration Register" bit 0: Enable Enhanced Register
 * Access. This is upstream of CR53/CR66 -- if clear, the entire S3d
 * register bank (0xA400-0xB5FF, and likely 0x8500) doesn't exist as
 * far as the chip's register decode is concerned: reads are undefined
 * and writes are dropped, regardless of what CR53/CR66/AFC say.
 * Power-on default is 0 -- BIOS normally sets it during POST, but a
 * warm reset or a boot path that skips real VGA BIOS execution (e.g.
 * UEFI GOP handoff) can leave it clear. */
#define VIRGE_CR40               0x40
#define VIRGE_CR40_EN_ENH        (1 << 0)

/* The register at MMIO offset 0x8504 is Subsystem STATUS on read, but
 * a completely different write-only Subsystem CONTROL register on
 * write. Bits 15-14 of the control register are "S3d RST": writing
 * 10b resets the S3d engine, 01b enables it. This is a required
 * one-time init step, separate from CR40/CR53/CR66/AFC. */
#define VIRGE_SUBSYS_CONTROL     0x8504
#define VIRGE_SSC_S3D_RESET      (2 << 14)
#define VIRGE_SSC_S3D_ENABLE     (1 << 14)
/* Writing bits 15-14 = 00b means "no change" (DB019-B §22, PDF p.301),
 * so interrupt-clear writes below don't disturb the engine enable. */
#define VIRGE_SSC_VSY_CLR        (1 << 0)   /* clear latched VSY INT */
/* Bit 8 VSY ENB - enable the vertical-sync interrupt. The VSY INT status
 * latch (Subsystem Status bit 0) only reports an interrupt that is ENABLED:
 * with this bit clear the latch never sets, so a clear-then-poll wait_vsync
 * spins to its 250ms timeout on every call (DB019-B §22, PDF p.300).
 * Confirmed on DX hardware: VSY INT sets within ~60ms only with VSY ENB set
 * (fliptest, 2026-07-07). OR it onto any control write -- bits 15-14 = 00b
 * mean "no S3d reset/change" (PDF p.301). (Asserts the PCI IRQ line; safe
 * here because no kernel driver owns the card on the takeover path.) */
#define VIRGE_SSC_VSY_ENB        (1 << 8)

/* ========================================================================
 * 2D Register Bank — BitBLT / Rectangle Fill
 * Base offset: 0xA400 (from MMIO region)
 * ======================================================================== */

#define VIRGE_2D_DEST_BASE      0xA4D8
#define VIRGE_2D_CLIP_L_R       0xA4DC
#define VIRGE_2D_CLIP_T_B       0xA4E0
#define VIRGE_2D_DEST_SRC_STR   0xA4E4
#define VIRGE_2D_MONO_PAT_0     0xA4E8
#define VIRGE_2D_MONO_PAT_1     0xA4EC
#define VIRGE_2D_PAT_BG_CLR     0xA4F0
#define VIRGE_2D_PAT_FG_CLR     0xA4F4
#define VIRGE_2D_CMD_SET        0xA500
#define VIRGE_2D_RWIDTH_HEIGHT  0xA504  /* width-1 in [26:16], height in [10:0] */
#define VIRGE_2D_RSRC_XY        0xA508
#define VIRGE_2D_RDEST_XY       0xA50C  /* dest X in [26:16], dest Y in [10:0] */

/* ========================================================================
 * 3D Register Bank — 3D Triangle
 * Base offset: 0xB400 (from MMIO region)
 *
 * The kick register for 3D triangles is TY01_Y12 (B57C), which is the
 * highest-address register in this bank. With autoexecute off, writing
 * CMD_SET (B500) executes the command. With autoexecute on, writing
 * TY01_Y12 (B57C) executes.
 * ======================================================================== */

/* --- Base addresses and stride (set once at init) --- */

#define VIRGE_3D_Z_BASE         0xB4D4  /* Z-buffer base in VRAM (byte offset, QW aligned) */
#define VIRGE_3D_DEST_BASE      0xB4D8  /* Destination (framebuffer) base in VRAM */
#define VIRGE_3D_CLIP_L_R       0xB4DC  /* Right clip [10:0], Left clip [26:16] */
#define VIRGE_3D_CLIP_T_B       0xB4E0  /* Bottom clip [10:0], Top clip [26:16] */
#define VIRGE_3D_DEST_SRC_STR   0xB4E4  /* Dest stride [27:16], Src(stride) [11:0] */
#define VIRGE_3D_Z_STRIDE       0xB4E8  /* Z stride [11:0], always 16-bit Z */

/* --- Texture setup (not used for Gouraud triangles) --- */

#define VIRGE_3D_TEX_BASE       0xB4EC
#define VIRGE_3D_TEX_BDR_CLR    0xB4F0
#define VIRGE_3D_FOG_CLR        0xB4F4
#define VIRGE_3D_COLOR0         0xB4F8
#define VIRGE_3D_COLOR1         0xB4FC

/* --- Command Set (the master control register) --- */

#define VIRGE_3D_CMD_SET        0xB500

/* --- Texture coordinates (start + deltas) --- */
/* Not used for Gouraud triangles, but defined for future texturing */

#define VIRGE_3D_TBV            0xB504  /* Base V */
#define VIRGE_3D_TBU            0xB508  /* Base U */
#define VIRGE_3D_TdWdX          0xB50C  /* dW/dX (S12.19) */
#define VIRGE_3D_TdWdY          0xB510  /* dW/dY (S12.19) */
#define VIRGE_3D_TWS            0xB514  /* W start (S12.19) */
#define VIRGE_3D_TdDdX          0xB518  /* dD/dX (S4.27, mipmap level) */
#define VIRGE_3D_TdVdX          0xB51C  /* dV/dX */
#define VIRGE_3D_TdUdX          0xB520  /* dU/dX */
#define VIRGE_3D_TdDdY          0xB524  /* dD/dY */
#define VIRGE_3D_TdVdY          0xB528  /* dV/dY */
#define VIRGE_3D_TdUdY          0xB52C  /* dU/dY */
#define VIRGE_3D_TDS            0xB530  /* D start */
#define VIRGE_3D_TVS            0xB534  /* V start */
#define VIRGE_3D_TUS            0xB538  /* U start */

/* --- Color coordinates (Gouraud RGBA) --- */
/*
 * Color format: S8.7 (1 sign + 8 integer + 7 fractional = 16 bits).
 * Two channels packed per register.
 * Start values must have sign bit = 0 (positive).
 */

#define VIRGE_3D_TdGdX_dBdX     0xB53C  /* Green dX [31:16], Blue dX [15:0] */
#define VIRGE_3D_TdAdX_dRdX     0xB540  /* Alpha dX [31:16], Red dX [15:0] */
#define VIRGE_3D_TdGdY_dBdY     0xB544  /* Green dY [31:16], Blue dY [15:0] */
#define VIRGE_3D_TdAdY_dRdY     0xB548  /* Alpha dY [31:16], Red dY [15:0] */
#define VIRGE_3D_TGS_BS         0xB54C  /* Green start [31:16], Blue start [15:0] */
#define VIRGE_3D_TAS_RS         0xB550  /* Alpha start [31:16], Red start [15:0] */

/* --- Z buffer interpolation --- */
/*
 * Z format: S16.15 (1 sign + 16 integer + 15 fractional = 32 bits).
 * Z start must have sign bit = 0.
 */

#define VIRGE_3D_TdZdX          0xB554  /* dZ/dX */
#define VIRGE_3D_TdZdY          0xB558  /* dZ/dY */
#define VIRGE_3D_TZS            0xB55C  /* Z start */

/* --- Edge geometry (triangle setup) --- */
/*
 * All edge slopes are dX/dY in S11.20 format (1 sign + 11 integer
 * + 20 fractional = 32 bits). The sign convention follows the 2D line:
 *   dXdY = -(ΔX / ΔY) * (1 << 20)
 * which is positive when the edge moves right going up.
 *
 * Vertices are sorted by Y descending: V0 (bottom, largest Y),
 * V1 (middle), V2 (top, smallest Y).
 *
 * Sides:
 *   02: V0→V2 (bottom to top, the long edge — always determines total height)
 *   01: V0→V1 (bottom to middle)
 *   12: V1→V2 (middle to top)
 */

#define VIRGE_3D_TdXdY12        0xB560  /* dX/dY for side 12 (mid→top) */
#define VIRGE_3D_TXEND12        0xB564  /* X at top vertex (S11.20) */
#define VIRGE_3D_TdXdY01        0xB568  /* dX/dY for side 01 (bot→mid) */
#define VIRGE_3D_TXEND01        0xB56C  /* X at middle vertex (S11.20) */
#define VIRGE_3D_TdXdY02        0xB570  /* dX/dY for side 02 (bot→top) */
#define VIRGE_3D_TXS            0xB574  /* X start at bottom vertex (S11.20) */
#define VIRGE_3D_TYS            0xB578  /* Y start (scan line of bottom vertex) */

/* --- KICK register (highest address — triggers execution with autoexecute) --- */

#define VIRGE_3D_TY01_Y12       0xB57C  /* Scan counts + L/R direction */
    /* Bits  10-0: SCAN_LINE_COUNT_12 (top half height: V1.y - V2.y)   */
    /* Bits 26-16: SCAN_LINE_COUNT_01 (bottom half height: V0.y - V1.y) */
    /* Bit  31:    L/R (1=left-to-right when side 02 is on the left)    */

/* ========================================================================
 * CMD_SET Register Bit Field Definitions
 *
 * The Command Set register is a 32-bit control word that selects the
 * drawing command, pixel formats, Z-buffer mode, blending, texturing,
 * and other options. Different bit encodings apply for 2D vs 3D.
 * ======================================================================== */

/* Bit 0: AE - Autoexecute */
#define VIRGE_CMD_AUTOEXEC      (1 << 0)

/* Bit 1: HC - Hardware Clipping Enable */
#define VIRGE_CMD_CLIP_ENABLE   (1 << 1)

/* NOT currently OR'd into any CMD_SET write in virge.c. Confirmed on
 * real hardware (2026-07): CLIP_L_R/CLIP_T_B (MMxxDC/MMxxE0) exhibit
 * the same "doesn't reliably hold a freshly written value" behavior
 * independently proven for DEST_BASE -- with HC enabled, every fill
 * degenerated to painting exactly pixel (0,0) regardless of the
 * requested rectangle, matching a stale/degenerate clip window rather
 * than the full-screen one every call site programs. Disabling HC
 * (leaving this bit clear) makes fills cover the full requested
 * rectangle. Root cause of why CLIP_L_R/CLIP_T_B don't stick is still
 * open -- until it's found, do not set this bit; the frontend/demos
 * are relied upon to only submit on-screen coordinates. */

/* Bits 4-2: DEST FORMAT - Destination Color Format */
#define VIRGE_DEST_8BPP         (0 << 2)   /* 8 bits/pixel palettized */
/* 16bpp destination. The 3D engine writes ZRGB1555 ONLY (DB019-B p.250);
 * there is no 565 triangle path. Scanout must therefore be CR67 Mode 9
 * (15-bit/RGB555) or triangles decode wrong — virge_init enforces this
 * (V8). The 2D engine at this format is RGB1555-or-RGB565 agnostic (p.232). */
#define VIRGE_DEST_16BPP        (1 << 2)
#define VIRGE_DEST_24BPP        (2 << 2)   /* 24 bits/pixel (RGB888) */

/* Bits 7-5: TEX CLR FORMAT (texture — not used for Gouraud) */

/* Bits 11-8: MIPMAP LEVEL SIZE (texture — not used for Gouraud) */

/* Bits 14-12: TEX FLTR MODE (texture — not used for Gouraud) */

/* Bits 16-15: TB - Texture Blending Mode (texture — not used for Gouraud) */

/* Bit 17: FE - Fog Enable */
#define VIRGE_CMD_FOG_ENABLE    (1 << 17)

/* Bits 19-18: ABC - Alpha Blending Control */
#define VIRGE_BLEND_NONE        (0 << 18)
#define VIRGE_BLEND_NONE_2      (1 << 18)
#define VIRGE_BLEND_TEX_ALPHA   (2 << 18)  /* Use texture alpha */
#define VIRGE_BLEND_SRC_ALPHA   (3 << 18)  /* Use source (vertex) alpha */

/* Bits 22-20: ZB COMP - Z-buffer Compare Mode */
#define VIRGE_ZBC_NEVER         (0 << 20)
#define VIRGE_ZBC_GREATER       (1 << 20)  /* Pass if Zs > Zzb */
#define VIRGE_ZBC_EQUAL         (2 << 20)  /* Pass if Zs = Zzb */
#define VIRGE_ZBC_GEQUAL        (3 << 20)  /* Pass if Zs >= Zzb */
#define VIRGE_ZBC_LESS          (4 << 20)  /* Pass if Zs < Zzb */
#define VIRGE_ZBC_NOTEQUAL      (5 << 20)  /* Pass if Zs != Zzb */
#define VIRGE_ZBC_LEQUAL        (6 << 20)  /* Pass if Zs <= Zzb */
#define VIRGE_ZBC_ALWAYS        (7 << 20)  /* Always passes */

/* Bit 23: ZUP - Z Update Enable */
#define VIRGE_ZUP_ENABLE        (1 << 23)  /* Update Z-buffer on pass */
#define VIRGE_ZUP_DISABLE       (0 << 23)

/* Bits 25-24: ZB MODE - Z-buffering Mode */
#define VIRGE_ZB_NORMAL         (0 << 24)  /* Normal Z-buffering */
#define VIRGE_ZB_MUX_ZPASS      (1 << 24)  /* MUX buffering, Z pass */
#define VIRGE_ZB_MUX_DRAW       (2 << 24)  /* MUX buffering, draw pass */
#define VIRGE_ZB_NONE           (3 << 24)  /* No Z-buffering */

/* Bit 26: TWE - Texture Wrap Enable */
#define VIRGE_CMD_TEX_WRAP      (1 << 26)

/* Bits 30-27: 3D COMMAND (only when bit 31 = 1) */
#define VIRGE_3D_GOURAUD        (0 << 27)   /* 0000: Gouraud shaded triangle */
#define VIRGE_3D_LIT_TEX        (1 << 27)   /* 0001: Lit texture triangle */
#define VIRGE_3D_UNLIT_TEX      (2 << 27)   /* 0010: Unlit texture triangle */
#define VIRGE_3D_LIT_TEX_PERSP  (5 << 27)   /* 0101: Lit texture w/ perspective */
#define VIRGE_3D_UNLIT_TEX_PERSP (6 << 27)  /* 0110: Unlit texture w/ perspective */
#define VIRGE_3D_LINE           (8 << 27)   /* 1000: 3D line */
#define VIRGE_3D_NOP            (0xF << 27) /* 1111: NOP (turn off autoexecute) */

/* Bit 31: 23D - 2D/3D Select */
#define VIRGE_CMD_2D            (0 << 31)
#define VIRGE_CMD_3D            (1U << 31)

/* ========================================================================
 * 2D CMD_SET Command Types
 *
 * For 2D commands (bit 31 = 0), the command type is encoded differently
 * from 3D. The datasheet shows these patterns in the programming examples:
 *   BitBLT:      bits [31:27] = 00000
 *   Rect Fill:   bit 28 = 1, bit 8 = 1 (mono pattern forced)
 *   2D Line:     bits [28:27] = 11
 *   2D Polygon:  bits [29:27] = 101
 *
 * For rectangle fill, the key bits are:
 *   bits 30-27: 2D command type, 0010 = rectangle fill (0x10000000, which
 *               happens to equal "bit 28 set" only because the other 3
 *               bits of this field are 0 for this particular command --
 *               it is a 4-bit enum, not an independent flag bit)
 *   bits 24-17: ROP (raster operation, 8-bit)
 *   bit 8: mono pattern (forced to 1, selects PAT_FG_CLR)
 *   bit 5: DE (Draw Enable) -- 0 = compute only, do not write any
 *          pixels; 1 = actually write pixels. Without this bit set,
 *          the engine dispatches and completes the command (visible
 *          in SUBSYS_STATUS) but VRAM is never touched.
 *   bits 4-2: destination format
 * ======================================================================== */

#define VIRGE_2D_CMD_RECT_FILL  (2 << 27)
/* 2D line command: bits [30:27] = 0011 (DB019-B §15.4.4.3, PDF p.124,
 * which writes 0001 100... to MMA900; 86Box decodes line = 3). */
#define VIRGE_2D_CMD_LINE       (3 << 27)
#define VIRGE_2D_CMD_DRAW_ENABLE (1 << 5)
#define VIRGE_2D_MONO_PATTERN   (1 << 8)
/* 2D draw direction (DB019-B sec.19.3 "Command Set", PDF pp.232-235):
 *   bit 25 XP - X Positive: 0 = right-to-left (X negative),
 *                          1 = left-to-right (X positive)
 *   bit 26 YP - Y Positive: 0 = bottom-to-top (Y negative),
 *                          1 = top-to-bottom (Y positive)
 * Both default to 0 (negative). With them unset a rectangle fill draws
 * right-to-left/bottom-to-top: DEST_XY is treated as the BOTTOM-RIGHT
 * corner, so the rect lands up-and-left of the intended position (X wraps
 * in linear memory, Y<0 goes off-screen). Verified via filltest readback:
 * a 100x100 rect at (50,50) landed at y=[0,50] with X wrapped full-width,
 * and a 64x600 strip at (736,0) collapsed to a single row -- both exactly
 * the negative-direction signature. The datasheet's own rect-fill example
 * (sec.15.4.4.2, p.121) leaves these bits 0 and is misleading; set both
 * for a normal top-left origin extending right and down. */
#define VIRGE_2D_CMD_X_POSITIVE (1 << 25)
#define VIRGE_2D_CMD_Y_POSITIVE (1 << 26)

/* ROP codes (Microsoft ROP3 convention) */
#define VIRGE_ROP_BLACKNESS     0x00    /* 0 (black) */
#define VIRGE_ROP_PATCOPY       0xF0    /* P (pattern) */
#define VIRGE_ROP_SRCCOPY       0xCC    /* S (source) */
#define VIRGE_ROP_DSTCOPY       0xAA    /* D (destination unchanged) */
#define VIRGE_ROP_WHITENESS     0xFF    /* 1 (white) */

/* ========================================================================
 * Fixed-Point Format Helpers
 *
 * The ViRGE uses fixed-point for interpolation values:
 *   Color (RGBA):  S8.7  — 1 sign + 8 integer + 7 fraction = 16 bits.
 *                  The 8-bit integer part IS the 0–255 channel value:
 *                  the engine computes channel = value >> 7 and clamps to
 *                  0–255 (86Box dest_pixel_gouraud_shaded_triangle). So a
 *                  normalized 0.0–1.0 channel maps to 0–(255<<7) = 0–32640,
 *                  NOT 0–128 — there is no internal scaling. Full intensity
 *                  is 32640, which is why the old ×128 scale rendered the
 *                  cube at ~0.4% brightness (the all-black-cube bug).
 *   Z values:      S16.15 — 1 sign + 16 integer + 15 fraction = 32 bits.
 *                  The 16-bit integer part IS the full Z value 0-65535: the
 *                  compare is accum>>15 as unsigned 16-bit (86Box tri():
 *                  z=(base_z>0)?base_z<<1:0, compared as z>>16 -- net >>15),
 *                  and ZUP writes that same 16-bit word. So a normalized
 *                  0.0-1.0 depth maps to 0-(65535<<15), NOT 0-(1<<15) -- no
 *                  internal normalization, the exact trap the S8.7 color
 *                  scale hit (V10). The old x2^15 scale made every z<1.0
 *                  compare as 0, which (a) turned ZBC_LESS into "pass iff the
 *                  fetched Z word is nonzero" -- the 3D cutoff bug -- and
 *                  (b) left ZUP writing 0, so depth tests could never order
 *                  triangles. Same x65535 factor on the deltas.
 *   X coordinates: S11.20 — 1 sign + 11 integer + 20 fraction = 32 bits.
 *                  Provides sub-pixel precision for edge interpolation.
 *   W (perspective): S12.19 — for perspective-correct texturing.
 * ======================================================================== */

#define VIRGE_COLOR_FRAC_BITS   7
/* S8.7 color: scale by 255·2^7 = 32640 (the integer part is the 8-bit
 * channel value). Clamp to int16: at this scale a color delta of
 * |dColor/dpixel| >= ~1.0 — steep or near-degenerate triangles where the
 * plane-equation gradient blows up via a small determinant — would
 * overflow/wrap int16, so saturate instead. Color starts in [0,1] map to
 * [0,32640] and never clamp. Used for both starts and deltas in the
 * Gouraud and textured triangle paths. */
static inline int16_t virge_color_fixed(float x)
{
    float v = x * (float)(255 * (1 << VIRGE_COLOR_FRAC_BITS));  /* ×32640 */
    if (v >  32767.0f) v =  32767.0f;
    if (v < -32768.0f) v = -32768.0f;
    return (int16_t)v;
}
#define VIRGE_COLOR_FIXED(x)    virge_color_fixed((float)(x))

#define VIRGE_Z_FRAC_BITS       15
/* S16.15 Z: scale by 65535*2^15 so the integer part (accum>>15) is the full
 * 0-65535 depth word the Z unit compares and writes -- the Z-analog of
 * VIRGE_COLOR_FIXED's x255 (V10). The old x2^15 made every z<1 compare as 0.
 * TODO(86Box quirk): TdZdX is accumulated in the post-shift domain (~16 frac
 * bits) while TZS/TdZdY are pre-shift (15); irrelevant at zero gradients
 * (all current demos) but revisit when real per-triangle Z ramps land. */
#define VIRGE_Z_FIXED(x)        ((int32_t)((x) * 65535.0f * (float)(1 << VIRGE_Z_FRAC_BITS)))

#define VIRGE_X_FRAC_BITS       20
#define VIRGE_X_FIXED(x)        ((int32_t)((x) * (float)(1 << VIRGE_X_FRAC_BITS)))

/* ========================================================================
 * Context Structure
 * ======================================================================== */

struct virge_ctx {
    /* PCI / memory mapping */
    int     fb_fd;          /* /dev/fb0 file descriptor */
    int     bar_fd;         /* resource0 file descriptor */
    void   *fb;             /* mmap'd framebuffer (linear) */
    void   *mmio;           /* mmap'd MMIO registers (BAR0 + 0x1000000) */
    size_t  fb_size;        /* framebuffer mapping size */
    size_t  mmio_size;      /* MMIO mapping size */

    /* PCI BAR address (physical) */
    uint32_t bar0;          /* BAR0 physical base (64MB aperture) */

    /* Screen geometry */
    int     width;          /* Screen width in pixels */
    int     height;         /* Screen height in pixels */
    int     bpp;            /* Bytes per pixel (NOT bits!) */
    uint32_t stride;        /* Framebuffer pitch (bytes/scanline), from fbdev
                             * finfo.line_length (P1). The engine's dest
                             * stride MUST equal the scanout pitch or the
                             * image shears/tiles ("multiple triangles").
                             * width*bpp is only right when the console
                             * resolution and pitch padding happen to match
                             * the caller's request. */

    /* Native scanout takeover (no-fbdev machines): original CRTC
     * values (parallel to virge_scanout_regs[] in virge.c), restored
     * by virge_cleanup. Includes CR0C/CR0D/CR69 so cleanup also restores
     * the display-start address that virge_swap_buffers may have moved. */
    int      scanout_owned;
    uint8_t  saved_scanout[20];
    /* Snapshot of ALL VRAM taken at scanout takeover and memcpy'd back at
     * cleanup. On a no-fbdev box the console is the bootloader's VBE
     * framebuffer living in VRAM; our rendering overwrites it, and with no
     * kernel fbdev nothing redraws it -- so after Ctrl-C the screen shows our
     * last 3D frame (garbage) even though the CRTC mode is restored. Saving the
     * whole aperture (not just the likely-offset-0 console region) covers the
     * console regardless of its display-start. NULL when no takeover happened. */
    uint8_t *saved_console_vram;
    size_t   saved_console_vram_size;

    /* Memory layout (byte offsets in VRAM). Two color buffers back the
     * double-buffer page flip: buffer 0 at offset 0 (the scanout at init),
     * buffer 1 at fb_base_back. fb_base is always the CURRENT render
     * target (the buffer the 2D/3D engine draws into) and starts at 0, so
     * single-buffer callers and the offset-0 readback diagnostics are
     * unchanged; virge_swap_buffers flips it between 0 and fb_base_back. */
    uint32_t fb_base;       /* Current render-target base (engine draws here) */
    uint32_t fb_base_back;  /* Second color buffer base (== stride*height) */
    int      current_back;  /* 0 = rendering buffer 0, 1 = buffer fb_base_back */
    uint32_t z_base;        /* Z-buffer base (after BOTH color buffers) */
    uint32_t vram_size;     /* Total VRAM in bytes */

    /* Cached CMD_SET destination format field */
    uint32_t dest_format;   /* VIRGE_DEST_16BPP, etc. */

    /* Cached CMD_SET Z-buffering bits for the 3D triangle paths:
     * ZB mode [25-24], Z compare code [22-20], Z update [23]. Built by
     * the glue (l10gl_virge.c) from the cached depth state (test on/off,
     * depth func, depth mask) -- mirrors how dest_format/tex_cmd_bits are
     * precomputed for the draw paths. virge_init seeds a sane default
     * (NORMAL | LEQUAL | ZUP) for direct callers that skip the glue. */
    uint32_t z_cmd_bits;

    /* Cached CMD_SET alpha-blending bits [19-18] (ABC) for the triangle
     * paths, built by the glue. gouraud_blend_bits applies to the Gouraud
     * path (SRC_ALPHA or NONE); textured_blend_bits applies to the textured
     * path and selects TEX_ALPHA vs SRC_ALPHA by whether the bound texture
     * has an alpha channel (DB019-B sec.15.4.8.5, PDF p.134). ViRGE blend
     * is fixed-function src*A + dst*(1-A); GL blend_func factors are
     * advisory (only SRC_ALPHA/ONE_MINUS_SRC_ALPHA is honored). */
    uint32_t gouraud_blend_bits;
    uint32_t textured_blend_bits;

    /* VRAM bump allocator for textures (offscreen heap after Z buffer) */
    uint32_t tex_heap_next; /* Next free byte offset for texture allocation */

    /* Cached texture state */
    uint32_t tex_cmd_bits;  /* Pre-shifted CMD_SET bits for current texture:
                               format [7-5], mipmap size [11-8],
                               filter [14-12], blend [16-15], wrap [26] */
    uint32_t tex_base;      /* TEX_BASE (0xB4EC) for the currently-bound
                               texture (quadword-aligned VRAM offset). Cached
                               so program_3d_state can RE-ARM it per primitive:
                               on real DX silicon, 2D commands (the clear/fill
                               between bind and draw) clobber 3D register
                               state -- the established Z_STRIDE lesson
                               (commit f0811f1). TEX_BASE is written only in
                               bind_texture otherwise, so without this re-arm
                               the first triangle after any clear reads from a
                               clobbered/default base -> off-texture garbage
                               (texprobe v3 confirmed: solid-red texture still
                               rendered 0x3436, the engine's off-texture texel). */
    int      tex_bound;     /* Non-zero if a texture is currently bound */

    /* Texture SOURCE stride: byte offset of vertically-adjacent texels = the
     * texture's row pitch (tex_width * bytes_per_texel) for a flat texture.
     * This goes in DEST_SRC_STR (0xB4E4) bits 11:0 -- the SOURCE STRIDE field
     * (datasheet 3d_regs.txt:292-294: "byte offset of vertically adjacent
     * pixels for a flat (not mipmapped) texture map", bits 2-0 must be 0).
     * Was wrongly set to the SCREEN stride -- texprobe traced every texel
     * resolving to TEX_BDR_CLR (border) to this (commit following v9).
     * Cached in bind_texture, re-armed per primitive in program_3d_state. */
    uint32_t tex_stride;

    /* DEBUG OVERRIDE for the texture-perspective U/V scale hunt (texprobe v7).
     * The driver normally encodes U/V with frac_bits = 27 - s_val (S(4+s).(27-s),
     * the datasheet format, correct for the NON-perspective path). But the
     * perspective sampler divides U by W with an internal shift, and on real DX
     * every texel resolves out-of-range -> TEX_BDR_CLR (texprobe v6 proved the
     * engine emits the border color, not any VRAM texel). 86Box's _375 math
     * disagrees with observed silicon, so the correct frac_bits must be found
     * empirically. When >= 0, tex_coord_fixed uses THIS many fractional bits
     * instead of (27 - s_val); -1 = default. Set by texprobe, ignored in
     * production. */
    int      tex_dbg_ufrac;

    /* Texture path select. 0 = PERSPECTIVE (command 0101), 1 = NON-perspective
     * (command 0001, affine). PRODUCTION default is 0 (perspective): the persp
     * divide was DECODED + FIXED on silicon 2026-07-09 (texprobe TEST 16/17) --
     * the engine computes texel = 128*TUS/TWS, so persp uses ufrac 12 (not 21)
     * and pre-multiplies U,V by W (see tex_dbg_nopremult). Was 1 (non-persp)
     * while the persp divide saturated; kept as the switch back to affine. */
    int      tex_dbg_nopersp;

    /* Perspective U,V pre-multiply by W. The engine interpolates TUS/TVS/TWS
     * linearly and divides per pixel (texel = TUS/TWS, silicon TEST 16/17); for
     * that divide to recover the true U, supply the homogeneous products U*W,
     * V*W so (U*W)/W = U is perspective-correct (no texture swim). PRODUCTION
     * default 0 = premult ON. Set 1 to ISOLATE the raw divide -- texprobe TEST
     * 16/17 do this to decode the 128*TUS/TWS relationship at constant W (where
     * premult would cancel W and hide the divide). */
    int      tex_dbg_nopremult;
};

/* ========================================================================
 * Vertex Type
 * ======================================================================== */

struct virge_vertex {
    float x, y;       /* Screen coordinates (pixels) */
    float z;          /* Depth value (0.0 = near, 1.0 = far) */
    float w;          /* 1/Z_eye for perspective correction (1.0 = disable) */
    float r, g, b, a; /* Color (0.0 to 1.0) */
    float u, v;       /* Texture coordinates */
};

/* ========================================================================
 * API
 * ======================================================================== */

/*
 * virge_init - Find the ViRGE, map memory, initialize the S3d Engine.
 * @ctx:     Context to initialize.
 * @width:   Desired screen width (should match current fbdev mode).
 * @height:  Desired screen height.
 * @bpp:     Bytes per pixel (2 = 16bpp, 3 = 24bpp).
 *
 * Returns 0 on success, negative errno on failure.
 *
 * Assumes a working fbdev console with new MMIO enabled (the default
 * for PCI ViRGE cards). Does NOT do modesetting.
 */
int virge_init(struct virge_ctx *ctx, int width, int height, int bpp);

/*
 * virge_cleanup - Unmap memory, close fds.
 */
void virge_cleanup(struct virge_ctx *ctx);

/*
 * virge_wait_engine - Block until the S3d Engine is idle.
 *
 * Polls the subsystem status register (bit 13) until the engine
 * is not busy.
 */
void virge_wait_engine(struct virge_ctx *ctx);

/*
 * virge_wait_vsync - Block until vertical retrace.
 */
void virge_wait_vsync(struct virge_ctx *ctx);

/*
 * virge_set_display_start - Repoint the CRTC scanout origin to byte_off.
 * The new start is latched at the next vertical blank (tear-free for the
 * next frame). Unit and registers: see virge.c (dword, CR0C/CR0D + CR69).
 */
void virge_set_display_start(struct virge_ctx *ctx, uint32_t byte_off);

/*
 * virge_swap_buffers - Publish the just-rendered buffer via a CRTC page
 * flip at the next vblank, then flip the render target to the other
 * buffer for the next frame. No-op-ish until a caller renders + swaps;
 * single-buffer code that never swaps is unaffected.
 */
void virge_swap_buffers(struct virge_ctx *ctx);

/*
 * virge_crtc_peek/poke - Raw CRTC register access for diagnostic tools
 * (demos/scantest.c). Only valid after virge_init() has run in this
 * process (ioperm grants + CR38/CR39 unlock happen there).
 */
uint8_t virge_crtc_peek(uint8_t index);
void virge_crtc_poke(uint8_t index, uint8_t value);

/*
 * virge_clear_z - Clear the Z buffer to a given value.
 * @ctx:  Driver context.
 * @z:    Z value to fill (0.0 = near, 1.0 = far).
 *
 * Uses a 2D rectangle fill targeting the Z buffer region.
 */
void virge_clear_z(struct virge_ctx *ctx, float z);

/*
 * virge_fill_rect - Hardware-accelerated rectangle fill.
 * @ctx:    Driver context.
 * @x, y:   Top-left corner (in pixels).
 * @w, h:   Width and height (in pixels).
 * @color:  Color in the current pixel format.
 */
void virge_fill_rect(struct virge_ctx *ctx, int x, int y, int w, int h,
                     uint32_t color);

/*
 * virge_draw_triangle_gouraud - Draw a Gouraud-shaded, Z-tested triangle.
 * @ctx:       Driver context.
 * @v0, v1, v2: Triangle vertices (screen coords + color + depth).
 *
 * Uses the native 3D Gouraud triangle primitive. One register write to
 * TY01_Y12 kicks the entire triangle — no trapezoid decomposition needed.
 *
 * Color gradients are computed via plane equations from the three vertices.
 * Z-buffer mode and depth function are set via cached state in the backend
 * glue layer (l10gl_virge.c).
 */
void virge_draw_triangle_gouraud(struct virge_ctx *ctx,
                                  struct virge_vertex v0,
                                  struct virge_vertex v1,
                                  struct virge_vertex v2);

/*
 * virge_draw_line - Hardware-accelerated 2D line draw.
 */
void virge_draw_line(struct virge_ctx *ctx,
                      int x0, int y0, int x1, int y1, uint32_t color);

/*
 * virge_upload_texture - Copy texture data into offscreen VRAM.
 * @ctx:      Driver context.
 * @dest:     Destination byte offset in VRAM (quadword aligned).
 * @data:     Source pixel data.
 * @size:     Size in bytes.
 *
 * Uses a 2D BitBLT (SRCCOPY) to write texture data into offscreen VRAM.
 */
void virge_upload_texture(struct virge_ctx *ctx, uint32_t dest,
                           const void *data, size_t size);

/*
 * virge_draw_textured_triangle - Perspective-correct textured, lit triangle.
 * @ctx:       Driver context.
 * @v0, v1, v2: Vertices with screen coords + UV + color.
 *
 * Uses the lit texture triangle with perspective correction.
 * The current texture (set via virge_bind_texture) provides texels.
 * The vertex RGBA modulates the texel color (modulate blending mode).
 */
void virge_draw_textured_triangle(struct virge_ctx *ctx,
                                   struct virge_vertex v0,
                                   struct virge_vertex v1,
                                   struct virge_vertex v2);

/* ========================================================================
 * Inline MMIO accessors.
 *
 * Registers are accessed as 32-bit words at their MMIO offset.
 * The mmio pointer is pre-offset to BAR0 + 0x1000000.
 * ======================================================================== */

static inline void virge_write32(struct virge_ctx *ctx,
                                  uint32_t offset, uint32_t value)
{
    volatile uint32_t *reg = (volatile uint32_t *)
        ((char *)ctx->mmio + offset);
    *reg = value;
}

static inline uint32_t virge_read32(struct virge_ctx *ctx, uint32_t offset)
{
    volatile uint32_t *reg = (volatile uint32_t *)
        ((char *)ctx->mmio + offset);
    return *reg;
}

/*
 * virge_kick_3d - Execute a 3D triangle command.
 *
 * With autoexecute off (CMD_SET bit 0 = 0), writing the CMD_SET register
 * itself triggers execution. This is the simplest approach for single
 * triangle draws.
 */

#endif /* VIRGE_H */
