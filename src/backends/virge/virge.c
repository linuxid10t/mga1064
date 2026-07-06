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

/* Exported CRTC access for diagnostic tools (demos/scantest.c). Only
 * valid after virge_init() has run in this process: ioperm() port
 * grants are per-process and the CR40+ range needs the CR38/CR39
 * unlock, both done in vga_ensure_new_mmio(). */
uint8_t virge_crtc_peek(uint8_t index)
{
    return vga_crtc_read(index);
}

void virge_crtc_poke(uint8_t index, uint8_t value)
{
    vga_crtc_write(index, value);
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

    /* CR40 bit 0 gates the existence of the entire S3d register bank
     * from the chip's register decode -- upstream of CR53/CR66/AFC.
     * If clear, everything past this point is a no-op regardless of
     * what CR53/CR66/AFC say. */
    uint8_t cr40 = vga_crtc_read(VIRGE_CR40);
    printf("S3 ViRGE: CR40 = 0x%02x (EN_ENH = %u)\n", cr40, cr40 & VIRGE_CR40_EN_ENH);
    if (!(cr40 & VIRGE_CR40_EN_ENH)) {
        printf("S3 ViRGE: CR40 bit 0 was CLEAR -- setting it\n");
        vga_crtc_write(VIRGE_CR40, cr40 | VIRGE_CR40_EN_ENH);
    }
    printf("S3 ViRGE: CR40 after write = 0x%02x\n", vga_crtc_read(VIRGE_CR40));

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

    /* The datasheet requires CR31 bit 3 alongside CR66 bit 0 for
     * enhanced mode to actually use linear memory mapping. Without it,
     * the S3d engine's Memory Port Controller applies legacy VGA
     * planar/chain-4 address decode to engine writes even though the
     * CPU's linear framebuffer aperture reads out fine -- fills can
     * "succeed" while landing in a completely different physical
     * layout than what's visible through the linear aperture. */
    uint8_t cr31 = vga_crtc_read(VIRGE_CR31);
    printf("S3 ViRGE: CR31 = 0x%02x (ENH_MAP = %u)\n",
           cr31, (cr31 & VIRGE_CR31_ENH_MAP) ? 1 : 0);
    if (!(cr31 & VIRGE_CR31_ENH_MAP)) {
        printf("S3 ViRGE: CR31 bit 3 was CLEAR -- setting it\n");
        vga_crtc_write(VIRGE_CR31, cr31 | VIRGE_CR31_ENH_MAP);
    }
    printf("S3 ViRGE: CR31 after write = 0x%02x\n", vga_crtc_read(VIRGE_CR31));

    return 0;
}

/*
 * Detect installed VRAM from the CR36 MEM SIZE power-on straps. The MEM
 * SIZE field is NOT in the same bit position across the family, so
 * dispatch on PCI device id and read the field each chip documents:
 *
 *   base ViRGE (86C325, 0x5631) — bits 7-5 (DB019-B PDF p.197):
 *       000 = 4MB, 100 = 2MB, others reserved.
 *   ViRGE/DX + /GX (0x8a01) — bits 7-5: 86Box programs these exact
 *       strap values for a 4MB (000) and 2MB (100) DX/GX
 *       (vid_s3_virge.c s3_virge_init), matching the base table.
 *       Needs HW confirmation: the VX sibling uses bits 6-5, so if a
 *       4MB DX/GX does NOT read bits 7-5 = 000, switch DX/GX to 6-5.
 *   ViRGE/VX (0x883d) — bits 6-5 (DB025-A §18, CR36 "Configuration 1"):
 *       00 = 2MB, 01 = 4MB, 10 = 6MB, 11 = 8MB. Bit 7 on the VX is a
 *       separate strap (8-column block-write support), NOT MEM SIZE —
 *       hence the 2-bit field, unlike the base/DX/GX 3-bit field.
 *
 * ViRGE/GX2 (0x8a10), MX/MX+ (0x8c00/0x8c01): no datasheet documents
 * their MEM SIZE layout (DB019-B is base-only, DB025-A is VX-only, and
 * 86Box models GX2 only at its hardcoded 4MB point), so fall back to 2MB
 * (smallest shipped config) rather than mis-decode.
 *
 * Straps are power-on and readable without unlock (CR39 unlock in
 * vga_ensure_new_mmio covers writes anyway). Must be called after
 * vga_ensure_new_mmio() has set up I/O port access.
 */
static uint32_t virge_detect_vram(uint16_t device_id)
{
    uint8_t cr36 = vga_crtc_read(VIRGE_CR36);

    switch (device_id) {
    case S3_PCI_DEVICE_VIRGE:        /* base 86C325: bits 7-5 (DB019-B) */
    case S3_PCI_DEVICE_VIRGE_DXGX: { /* DX/GX: bits 7-5 (86Box) */
        uint8_t ms = (cr36 & VIRGE_CR36_MEM_SIZE_MASK)
                     >> VIRGE_CR36_MEM_SIZE_SHIFT;  /* bits 7-5 */
        uint32_t vram;
        const char *label;
        switch (ms) {
        case 0x0: vram = 4u * 1024 * 1024; label = "4MB";            break;
        case 0x4: vram = 2u * 1024 * 1024; label = "2MB";            break;
        default:  vram = 2u * 1024 * 1024; label = "reserved -> 2MB"; break;
        }
        printf("S3 ViRGE: CR36=0x%02x (bits 7-5=%u) -> detected %s VRAM\n",
               cr36, ms, label);
        return vram;
    }
    case S3_PCI_DEVICE_VIRGE_VX: {   /* VX: bits 6-5 (DB025-A §18) */
        uint8_t ms = (cr36 & VIRGE_CR36_VX_MEM_SIZE_MASK)
                     >> VIRGE_CR36_MEM_SIZE_SHIFT;  /* bits 6-5 */
        static const uint32_t mb[4] = { 2, 4, 6, 8 };
        uint32_t vram = mb[ms] * 1024u * 1024u;
        printf("S3 ViRGE/VX: CR36=0x%02x (bits 6-5=%u) -> detected %uMB VRAM\n",
               cr36, ms, mb[ms]);
        return vram;
    }
    default: {
        printf("S3 ViRGE: CR36=0x%02x; device 0x%04x MEM SIZE strap not "
               "documented (no datasheet), assuming 2MB\n", cr36, device_id);
        return 2u * 1024 * 1024;
    }
    }
}

/*
 * Dump what the CRTC is ACTUALLY scanning out, decoded from the chip's
 * own registers, and compare it against what fbdev claimed earlier in
 * init. Reads only — safe for the proven bring-up path.
 *
 * Motivation: fbdev reported a 640x480 console on the same day a direct
 * CRTC probe read VDE=599 (600 active lines). Both cannot be true, and
 * every engine test so far was interpreted THROUGH fbdev's geometry.
 * This makes the chip's side of the story a permanent part of the boot
 * log so the two can be reconciled in one run.
 *
 * Register decode, all verified against DB019-B (VGA CRTC §16, PDF
 * pp. 148-153; extended CRTC §18):
 *   - Active lines: CR12 + CR07 bit 1 (VDE:8) + CR07 bit 6 (VDE:9) +
 *     CR5E bit 1 (VDE:10); value = lines - 1.
 *   - Active width: CR01 + CR5D bit 1 (bit 8), in character clocks;
 *     value = clocks - 1, one clock = 8 pixels (at 1 pixel/VCLK).
 *   - Pitch: CR13 "logical screen width", extended by CR51 bits 5-4
 *     (bits 9-8); if those are 00b, CR43 bit 2 is bit 8 instead. The
 *     byte multiplier is x2 (word), x4 (doubleword) or x8 (quadword)
 *     per CR14 bit 6 / CR17 bit 3 / CR31 bit 3 (bit 3 of CR31 forces
 *     doubleword) — print all three candidates rather than over-trust
 *     the mode-bit decode, and flag whichever matches fbdev.
 *   - Display start: CRC<<8 | CRD, high bits from CR69 bits 3-0 when
 *     non-zero (superseding CR31 bits 5-4 = addr 17-16 and CR51 bits
 *     1-0 = addr 19-18, PDF p.193). The engine draws at fb_base = 0 and
 *     assumes scanout starts there too — a non-zero start displaces
 *     everything we draw.
 *   - Scanout format: CR67 bits 7-4 (PDF p.216). Mode 9 = 15-bit 555,
 *     Mode 10 = 16-bit 565 — ground truth on whether V8's fbdev depth
 *     switch actually reached the RAMDAC.
 *
 * Call after vga_ensure_new_mmio() (I/O port access + CR39 unlock for
 * the CR40+ range).
 */
static void virge_dump_crtc_truth(struct virge_ctx *ctx)
{
    uint8_t cr01 = vga_crtc_read(0x01);
    uint8_t cr07 = vga_crtc_read(0x07);
    uint8_t cr0c = vga_crtc_read(0x0C);
    uint8_t cr0d = vga_crtc_read(0x0D);
    uint8_t cr12 = vga_crtc_read(0x12);
    uint8_t cr13 = vga_crtc_read(0x13);
    uint8_t cr14 = vga_crtc_read(0x14);
    uint8_t cr17 = vga_crtc_read(0x17);
    uint8_t cr31 = vga_crtc_read(VIRGE_CR31);
    uint8_t cr43 = vga_crtc_read(0x43);
    uint8_t cr51 = vga_crtc_read(0x51);
    uint8_t cr5d = vga_crtc_read(0x5D);
    uint8_t cr5e = vga_crtc_read(0x5E);
    uint8_t cr67 = vga_crtc_read(0x67);
    uint8_t cr69 = vga_crtc_read(0x69);

    printf("S3 ViRGE: CRTC raw: CR01=%02x CR07=%02x CR0C=%02x CR0D=%02x "
           "CR12=%02x CR13=%02x CR14=%02x CR17=%02x\n",
           cr01, cr07, cr0c, cr0d, cr12, cr13, cr14, cr17);
    printf("                    CR31=%02x CR43=%02x CR51=%02x CR5D=%02x "
           "CR5E=%02x CR67=%02x CR69=%02x\n",
           cr31, cr43, cr51, cr5d, cr5e, cr67, cr69);

    uint32_t vde = cr12 | (uint32_t)((cr07 >> 1) & 1) << 8
                        | (uint32_t)((cr07 >> 6) & 1) << 9
                        | (uint32_t)((cr5e >> 1) & 1) << 10;
    uint32_t hde_clk = cr01 | (uint32_t)((cr5d >> 1) & 1) << 8;

    uint32_t lsw = cr13;
    if (cr51 & 0x30)
        lsw |= (uint32_t)((cr51 >> 4) & 3) << 8;
    else
        lsw |= (uint32_t)((cr43 >> 2) & 1) << 8;

    uint32_t start = (uint32_t)cr0c << 8 | cr0d;
    if (cr69 & 0x0F)
        start |= (uint32_t)(cr69 & 0x0F) << 16;
    else
        start |= (uint32_t)((cr31 >> 4) & 3) << 16
               | (uint32_t)(cr51 & 3) << 18;

    static const char *const color_modes[16] = {
        [0x0] = "Mode 0: 8-bit",   [0x1] = "Mode 8: 8-bit 2px/clk",
        [0x3] = "Mode 9: 15-bit (555)", [0x5] = "Mode 10: 16-bit (565)",
        [0xD] = "Mode 13: 24-bit",
    };
    uint8_t cm = cr67 >> 4;
    const char *fmt = color_modes[cm] ? color_modes[cm] : "RESERVED";

    printf("S3 ViRGE: CRTC truth: %u active lines, %u char clocks "
           "(x8 = %u px) wide,\n", vde + 1, hde_clk + 1, (hde_clk + 1) * 8);
    printf("  pitch: LSW=%u -> x2=%u / x4=%u / x8=%u bytes "
           "(mode bits: CR14.6=%u CR17.3=%u CR31.3=%u),\n",
           lsw, lsw * 2, lsw * 4, lsw * 8,
           (cr14 >> 6) & 1, (cr17 >> 3) & 1, (cr31 >> 3) & 1);
    printf("  display start: raw 0x%05x (x4 if dword-addressed = byte "
           "0x%x), scanout format: CR67[7:4]=%x -> %s\n",
           start, start * 4, cm, fmt);

    /* Reconcile against fbdev's claims, loudly. */
    struct fb_var_screeninfo vinfo;
    if (ctx->fb_fd >= 0 && ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
        if (vinfo.yres != vde + 1)
            printf("  *** CRTC/fbdev MISMATCH: CRTC scans %u lines but "
                   "fbdev claims yres=%u — fbdev geometry is NOT scanout "
                   "truth\n", vde + 1, vinfo.yres);
        if (ctx->stride != lsw * 2 && ctx->stride != lsw * 4 &&
            ctx->stride != lsw * 8)
            printf("  *** CRTC/fbdev MISMATCH: no CRTC pitch candidate "
                   "(%u/%u/%u) matches fbdev line_length=%u — engine "
                   "strides based on line_length will shear\n",
                   lsw * 2, lsw * 4, lsw * 8, ctx->stride);
        if (vinfo.bits_per_pixel >= 15 && vinfo.bits_per_pixel <= 16) {
            uint8_t want = (vinfo.green.length == 5) ? 0x3 : 0x5;
            if (cm != want)
                printf("  *** CRTC/fbdev MISMATCH: fbdev claims %s but "
                       "CR67 scanout is %s — the V8 depth switch did NOT "
                       "reach the RAMDAC\n",
                       vinfo.green.length == 5 ? "RGB555" : "RGB565", fmt);
        }
    }
    if (start != 0)
        printf("  *** WARNING: display start address is non-zero but the "
               "engine draws at VRAM offset 0 — drawn content is displaced "
               "from what is scanned out\n");
}

/* Registers saved before a scanout takeover and restored at cleanup.
 * Order is the ctx->saved_scanout[] layout. CR11 is included because
 * its bit 7 write-protects CR00-CR07 and must be juggled around the
 * horizontal-timing writes. */
static const uint8_t virge_scanout_regs[] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05,   /* horizontal timing set */
    0x11,                                 /* vert retrace end + CR0-7 lock */
    0x13, 0x3B, 0x43, 0x50, 0x51, 0x5D, 0x67,
};
#define VIRGE_SCANOUT_REG_COUNT \
    ((int)(sizeof(virge_scanout_regs) / sizeof(virge_scanout_regs[0])))
#define VIRGE_SCANOUT_CR11_IDX 6          /* index of 0x11 in the list */

/*
 * Native scanout takeover, for machines with no fbdev driver bound to
 * the card. Observed on the primary test machine: /dev/fb0 absent, the
 * chip left scanning the bootloader's VBE mode — 800x600 raster, CR67
 * Mode 13, 32-bit pixels at pitch 3200 (proven by scantest phase 1).
 * With no kernel driver there is nothing to ioctl, so the V8 fbdev
 * depth switch can never run; the driver owns the scanout itself.
 *
 * The recipe follows the kernel s3fb driver (s3fb_set_par, the
 * plain-ViRGE/DX branch), consulted as reference documentation because
 * DB019-B does not describe the per-depth timing scaling (and does not
 * document CR50 at all). For 15/16bpp on this chip family:
 *
 *   - CR67 bits 7-4 = Mode 9 (15-bit 555, DB019-B PDF p.216), matching
 *     the 3D engine's ZRGB1555-only 16bpp output (V8).
 *   - hmul = 2: EVERY horizontal CRTC timing value is programmed at
 *     TWICE its pixel-based value — in 15/16bpp modes the CRTC counts
 *     at two character clocks per 8 pixels (byte-based fetch). The
 *     first takeover attempt changed CR67 alone, which halved the real
 *     line period and doubled hsync: monitor "input signal out of
 *     range" on hardware (2026-07-06). The DCLK itself is untouched.
 *   - CR50 bits 5-4 = 01b (pixel length 16bpp; per s3fb — not in
 *     DB019-B).
 *   - Pitch in QUADWORDS: LSW = pitch/8 via CR13 + CR51 bits 5-4, with
 *     CR43 bit 2 cleared (s3fb s3_offset_regs; confirmed by the BIOS
 *     mode itself: LSW=400 with a measured 3200-byte pitch). The first
 *     attempt used /4 — its "pitch 1600" was really 3200, which is why
 *     scantest phase 2's pattern was silently squashed 2:1.
 *
 * Horizontal doubling decodes the live registers (field semantics per
 * DB019-B §16 PDF pp.148-153: HT = chars-5, HDE = chars-1, SHB/SHS =
 * raw char counts, EHB/EHS = modulo end counters 7/6 bits wide;
 * extension bits in CR5D §18 PDF p.214) back to character counts,
 * doubles them, and re-encodes, preserving skew bits. Blank/sync
 * WIDTHS are doubled, so the sync pulse the monitor sees is unchanged
 * in real time. CR3B (Start Display FIFO, "typically 5 less than
 * CR00", §18 PDF p.203) is recomputed against the new horizontal
 * total. Vertical timing counts lines and needs no scaling.
 *
 * Idempotency: if CR67 already reads Mode 9/10 (a previous takeover
 * that died without restore), the timings are already doubled — the
 * geometry is derived at hmul=2 and the doubling step is skipped.
 *
 * Requires vga_ensure_new_mmio() (port access + unlocks) and the S3d
 * enable sequence (virge_wait_vsync reads SUBSYS_STATUS over MMIO).
 */
static void virge_scanout_takeover(struct virge_ctx *ctx)
{
    uint8_t r[VIRGE_SCANOUT_REG_COUNT];
    for (int i = 0; i < VIRGE_SCANOUT_REG_COUNT; i++) {
        r[i] = vga_crtc_read(virge_scanout_regs[i]);
        ctx->saved_scanout[i] = r[i];
    }
    uint8_t cr03 = r[3], cr05 = r[5], cr5d = r[12];
    uint8_t cr07 = vga_crtc_read(0x07);
    uint8_t cr12 = vga_crtc_read(0x12);
    uint8_t cr5e = vga_crtc_read(0x5E);

    /* Decode the live horizontal set to character counts (see the
     * comment above for field semantics). */
    uint32_t ht    = ((uint32_t)r[0] | (uint32_t)((cr5d >> 0) & 1) << 8) + 5;
    uint32_t hde   = ((uint32_t)r[1] | (uint32_t)((cr5d >> 1) & 1) << 8) + 1;
    uint32_t shb   =  (uint32_t)r[2] | (uint32_t)((cr5d >> 2) & 1) << 8;
    uint32_t ehb   =  (uint32_t)(cr03 & 0x1F) |
                      (uint32_t)((cr05 >> 7) & 1) << 5 |
                      (uint32_t)((cr5d >> 3) & 1) << 6;
    uint32_t blkw  = (ehb - (shb & 0x7F)) & 0x7F;   /* blank width, chars */
    uint32_t shs   =  (uint32_t)r[4] | (uint32_t)((cr5d >> 4) & 1) << 8;
    uint32_t ehs   =  (uint32_t)(cr05 & 0x1F) |
                      (uint32_t)((cr5d >> 5) & 1) << 5;
    uint32_t syncw = (ehs - (shs & 0x3F)) & 0x3F;   /* sync width, chars */

    uint32_t lines = (cr12 | (uint32_t)((cr07 >> 1) & 1) << 8
                           | (uint32_t)((cr07 >> 6) & 1) << 9
                           | (uint32_t)((cr5e >> 1) & 1) << 10) + 1;

    /* Diagnostic for the "dark vertical band" symptom. CR3B (Start
     * Display FIFO Fetch) is a 9-bit value — low 8 bits in CR3B, bit 8
     * in CR5D bit 6 (DB019-B §18 "Extended Horizontal Overflow" CR5D,
     * PDF p.203: "Bit 6 SFF 8 - Start FIFO Fetch (CR3B) Bit 8"). Its
     * description (CR3B, PDF p.203) requires it to "lie in the
     * horizontal blanking period" and notes it is "typically 5 less
     * than the value programmed in CR0" — typically, not always. It is
     * only active when CR34 bit 4 (ENB SFF) is set (CR34 "Backward
     * Compatibility 3", PDF p.203). The takeover rewrites CR3B as
     * new_CR00-5 but never reads CR34, so dump the pre-takeover state
     * to learn whether SFF is in effect and what the BIOS relation was. */
    uint8_t  cr34    = vga_crtc_read(0x34);
    uint32_t old_sff = (uint32_t)r[8] | (uint32_t)((r[12] >> 6) & 1) << 8;
    printf("S3 ViRGE: pre-takeover SFF: CR34=%02x (ENB SFF bit4=%d); "
           "CR3B=%02x CR5D=%02x -> 9-bit SFF=%u; HT=%u chars; "
           "blank window=[%u,%u); typical SFF (HT-10)=%u\n",
           cr34, (cr34 >> 4) & 1, r[8], r[12], old_sff, ht,
           shb, shb + blkw, ht > 10 ? ht - 10 : 0);

    /* Character clocks per 8 pixels in the CURRENT mode: 2 for the
     * 15/16bpp modes (9/10), 1 for 8bpp Mode 0 and 24-bit Mode 13. */
    uint8_t cur_mode = r[13] >> 4;
    int cur_hmul = (cur_mode == 0x3 || cur_mode == 0x5) ? 2 : 1;
    if (cur_mode != 0x0 && cur_mode != 0xD && cur_hmul == 1)
        printf("S3 ViRGE: WARNING: current CR67 mode %x is not a known "
               "linear graphics mode; takeover geometry may be wrong\n",
               cur_mode);

    uint32_t width = hde * 8 / cur_hmul;

    printf("S3 ViRGE: native scanout takeover: adopting the live %ux%u "
           "raster (caller requested %dx%d), Mode 9/555 @ pitch %u\n",
           width, lines, ctx->width, ctx->height, width * 2);

    ctx->width = (int)width;
    ctx->height = (int)lines;
    ctx->stride = width * 2;
    ctx->fb_size = ctx->stride * lines;

    virge_wait_vsync(ctx);

    if (cur_hmul == 1) {
        /* Double every horizontal count; widths double too, so the
         * real-time sync/blank the monitor sees is unchanged. */
        ht *= 2;  hde *= 2;  shb *= 2;  blkw *= 2;  shs *= 2;  syncw *= 2;

        uint32_t cr00n = ht - 5;
        uint32_t cr01n = hde - 1;
        uint32_t ehbn  = (shb + blkw) & 0x7F;
        uint32_t ehsn  = (shs + syncw) & 0x3F;
        uint32_t sffn  = cr00n - 5;   /* CR3B rule: 5 less than CR00 */

        vga_crtc_write(0x11, r[VIRGE_SCANOUT_CR11_IDX] & 0x7F); /* unlock CR0-7 */
        vga_crtc_write(0x00, (uint8_t)cr00n);
        vga_crtc_write(0x01, (uint8_t)cr01n);
        vga_crtc_write(0x02, (uint8_t)shb);
        vga_crtc_write(0x03, (uint8_t)((cr03 & 0x60) | (ehbn & 0x1F)));
        vga_crtc_write(0x04, (uint8_t)shs);
        vga_crtc_write(0x05, (uint8_t)((((ehbn >> 5) & 1) << 7) |
                                       (cr05 & 0x60) | (ehsn & 0x1F)));
        vga_crtc_write(0x3B, (uint8_t)sffn);
        vga_crtc_write(0x5D, (uint8_t)((cr5d & 0x80)
                            | (((cr00n >> 8) & 1) << 0)
                            | (((cr01n >> 8) & 1) << 1)
                            | (((shb   >> 8) & 1) << 2)
                            | (((ehbn  >> 6) & 1) << 3)
                            | (((shs   >> 8) & 1) << 4)
                            | (((ehsn  >> 5) & 1) << 5)
                            | (((sffn  >> 8) & 1) << 6)));
        vga_crtc_write(0x11, r[VIRGE_SCANOUT_CR11_IDX]);        /* re-lock */
    }

    /* Pixel format, engine pixel length, pitch. */
    vga_crtc_write(0x67, (uint8_t)((r[13] & 0x0F) | (0x3 << 4)));
    vga_crtc_write(0x50, (uint8_t)((r[10] & ~0x30) | 0x10));
    uint32_t lsw = ctx->stride / 8;   /* pitch in quadwords */
    vga_crtc_write(0x13, (uint8_t)(lsw & 0xFF));
    vga_crtc_write(0x51, (uint8_t)((r[11] & ~0x30) | (((lsw >> 8) & 3) << 4)));
    vga_crtc_write(0x43, (uint8_t)(r[9] & ~0x04));
    ctx->scanout_owned = 1;

    printf("S3 ViRGE: scanout now CR67=%02x CR50=%02x CR00=%02x CR01=%02x "
           "CR13=%02x CR51=%02x CR5D=%02x CR3B=%02x CR34=%02x; "
           "programmed SFF(9bit)=%u (= CR00 %02x - 5); "
           "originals restored at cleanup\n",
           vga_crtc_read(0x67), vga_crtc_read(0x50), vga_crtc_read(0x00),
           vga_crtc_read(0x01), vga_crtc_read(0x13), vga_crtc_read(0x51),
           vga_crtc_read(0x5D), vga_crtc_read(0x3B), vga_crtc_read(0x34),
           (uint32_t)vga_crtc_read(0x3B) |
               (uint32_t)((vga_crtc_read(0x5D) >> 6) & 1) << 8,
           vga_crtc_read(0x00));
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
    /* SUBSYS_STATUS bit 0 (VSY INT) is a LATCHED interrupt status, not
     * a live retrace level: it stays 1 until software writes VSY CLR to
     * the write-only Subsystem Control register (DB019-B §22, PDF
     * pp.299-301). The old code here waited for the bit to fall on its
     * own — it never does, so every call after the first spun forever
     * (hard-hung scantest phase 2 on real hardware). Correct sequence:
     * clear the latch, then wait for the next vsync to set it again.
     *
     * The latch holds the event, so sleep-polling loses nothing; the
     * poll is also bounded so a misconfigured scanout (no vsync being
     * generated) degrades to a warning instead of an unkillable spin. */
    virge_write32(ctx, VIRGE_SUBSYS_CONTROL, VIRGE_SSC_VSY_CLR);
    for (int i = 0; i < 1250; i++) {   /* 1250 * 200us = 250ms bound */
        if (virge_read32(ctx, VIRGE_SUBSYS_STATUS) & VIRGE_STATUS_VSYNC)
            return;
        usleep(200);
    }
    fprintf(stderr, "S3 ViRGE: wait_vsync timed out (no vsync in 250ms) "
                    "-- proceeding without sync\n");
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
    uint32_t dest_stride = ctx->stride;  /* real scanout pitch (P1) */
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

    /* Clip to full screen. Left/top = 0 in the upper field, right/bottom
     * = dimension-1 in the lower field -- these were previously swapped,
     * producing a degenerate (left > right) clip rectangle that
     * silently rejected every pixel regardless of any other setting. */
    virge_write32(ctx, VIRGE_3D_CLIP_L_R,
                  (0 << 16) | ((ctx->width - 1) & 0x7FF));
    virge_write32(ctx, VIRGE_3D_CLIP_T_B,
                  (0 << 16) | ((ctx->height - 1) & 0x7FF));
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

    uint32_t dest_stride = ctx->stride;  /* real scanout pitch (P1) */

    /* Set destination base and stride for the 2D bank */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
    /* Left/top = 0, right/bottom = dimension-1 (see engine_init_3d for
     * why this ordering, not the reverse, matters). */
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  (0 << 16) | ((ctx->width - 1) & 0x7FF));
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  (0 << 16) | ((ctx->height - 1) & 0x7FF));

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
     *   bits 30-27: rectangle fill (0010)
     *   bit 8: mono pattern (forced, selects PAT_FG_CLR)
     *   bit 5: draw enable (without this, the engine computes but
     *          never writes a single pixel)
     *   bits 4-2: destination format
     *   bits 24-17: ROP = PATCOPY (0xF0)
     *   bit 1: clipping enabled -- NOT set, see VIRGE_CMD_CLIP_ENABLE
     *          comment in virge.h
     */
    uint32_t cmd = VIRGE_2D_CMD_RECT_FILL
                 | VIRGE_2D_MONO_PATTERN
                 | VIRGE_2D_CMD_DRAW_ENABLE
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17);
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

    uint32_t dest_stride = ctx->stride;  /* real scanout pitch (P1) */
    uint32_t z_stride = ctx->width * 2;  /* 16-bit Z */

    /* Reprogram 2D registers to point at Z buffer instead of framebuffer.
     * Stride is a byte offset (DB019-B PDF p.246); the Z row is width*2
     * bytes since Z is 16-bit/pixel. Width/clip are in *pixels* (PDF p.235,
     * p.232) because the fill runs at ctx->dest_format (16bpp) — one pixel
     * per 16-bit Z word, see the width field below. */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->z_base & ~0x7);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((z_stride & 0xFFF) << 16) | (z_stride & 0xFFF));
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  (0 << 16) | ((ctx->width - 1) & 0x7FF));
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  (0 << 16) | ((ctx->height - 1) & 0x7FF));

    /* Fill color = the Z value repeated */
    virge_write32(ctx, VIRGE_2D_PAT_FG_CLR, z_color);

    /* See virge_fill_rect() for why this is required. */
    virge_write32(ctx, VIRGE_2D_MONO_PAT_0, 0xFFFFFFFF);
    virge_write32(ctx, VIRGE_2D_MONO_PAT_1, 0xFFFFFFFF);

    /* Width in pixels (RWIDTH [26:16] is pixels, value 0 = 1 pixel → N-1),
     * height in lines. The fill runs at ctx->dest_format (16bpp on a
     * normal run), so one pixel == one 16-bit Z word and a width-pixel
     * row covers exactly one Z row. The 2D engine's 16bpp mode is
     * format-agnostic ("RGB1555 or RGB565", DB019-B PDF p.232), so the
     * raw Z words pass through unmodified. */
    virge_write32(ctx, VIRGE_2D_RWIDTH_HEIGHT,
                  (((ctx->width - 1) & 0x7FF) << 16) |
                  (ctx->height & 0x7FF));

    virge_write32(ctx, VIRGE_2D_RDEST_XY, 0);

    /* Fill at ctx->dest_format (16bpp) with the width in pixels above.
     * The old code mixed units: it programmed width as width*2 (bytes)
     * while keeping a 16bpp dest format, so the pixel-based width field
     * read as 2x the real count and every Z scanline overran into the
     * next. 16bpp is the format proven for fills (virge_fill_rect) and
     * the engine writes the low 16 bits of PAT_FG_CLR per pixel.
     * No VIRGE_CMD_CLIP_ENABLE -- see the comment in virge.h. */
    uint32_t cmd = VIRGE_2D_CMD_RECT_FILL
                 | VIRGE_2D_MONO_PATTERN
                 | VIRGE_2D_CMD_DRAW_ENABLE
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17);

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

    /* Slope of the long edge 02 in X, per scanline up (S11.20 -> float).
     * The engine walks each attribute base up one scanline AND along
     * edge 02, so the Y deltas we program are edge-walk deltas
     * TdAdY = -dA/dy + slope02*dA/dx (86Box; docs/datasheets/README.md),
     * not the raw plane dA/dy. */
    float slope02 = (float)dXdY02 / (float)(1 << VIRGE_X_FRAC_BITS);

    /* Span-end X per the engine's edge-walk semantics: TXEND01 is edge
     * 01's X at the FIRST (bottom) scanline = the bottom vertex X (edges
     * 01 and 02 both start at v0), NOT the middle vertex; TXEND12 is
     * edge 12's X at the MIDDLE scanline. Programming middle/top vertex
     * X here makes the bottom span jump straight to the middle-vertex X. */
    int32_t x_end01 = VIRGE_X_FIXED(v0.x);  /* edge 01 at bottom scanline */
    int32_t x_end12 = VIRGE_X_FIXED(v1.x);  /* edge 12 at middle scanline */

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
    /* Y deltas are edge-walk along side 02: -dA/dy + slope02*dA/dx
     * (86Box; docs/datasheets/README.md). X deltas above stay raw. */
    virge_write32(ctx, VIRGE_3D_TdGdY_dBdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(-dgdy + slope02 * dgdx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(-dbdy + slope02 * dbdx));
    virge_write32(ctx, VIRGE_3D_TdAdY_dRdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(-dady + slope02 * dadx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(-drdy + slope02 * drdx));

    /* --- Program Z start and deltas (S16.15) --- */
    virge_write32(ctx, VIRGE_3D_TZS, (uint32_t)VIRGE_Z_FIXED(z_s));
    virge_write32(ctx, VIRGE_3D_TdZdX, (uint32_t)VIRGE_Z_FIXED(dzdx));
    virge_write32(ctx, VIRGE_3D_TdZdY,
                  (uint32_t)VIRGE_Z_FIXED(-dzdy + slope02 * dzdx));

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

    /* --- Program Command Set and execute ---
     * Z-buffering bits come from ctx->z_cmd_bits (built by the glue from
     * the cached depth func/test/mask state); the GL default is LESS, not
     * the LEQUAL hardcoded here before.
     * No VIRGE_CMD_CLIP_ENABLE -- see the comment in virge.h. */
    uint32_t cmd = VIRGE_CMD_3D
                 | VIRGE_3D_GOURAUD
                 | ctx->dest_format
                 | ctx->z_cmd_bits
                 | ctx->gouraud_blend_bits;

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
     * X DELTA = -(ΔX << 20) / ΔY, integer divide (DB019-B §15.4.4.3,
     * PDF p.122). ΔX and ΔY must share one sign convention; we use
     * dX = x1-x0 and dY = y1-y0 (both measured top-minus-bottom after
     * the Y sort above, so dY ≤ 0). For horizontal lines (dY = 0) X
     * DELTA is 0 and the engine draws from END0/END1 instead.
     *
     * The divisor MUST be the signed dY, not abs_dY: after the sort dY
     * is negative, so the leading '-' and the negative divisor together
     * yield the correct sign (positive for a line that climbs to the
     * right). Dividing by abs_dY — as this code did before — flips the
     * sign and mirrors the line. The formula has always read '/ΔY';
     * the old implementation contradicted its own comment.
     */
    int32_t x_delta;
    if (dY == 0) {
        x_delta = 0;  /* horizontal line: engine uses END0/END1 */
    } else {
        x_delta = -(int32_t)(((int64_t)dX << 20) / dY);
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

    uint32_t dest_stride = ctx->stride;  /* real scanout pitch (P1) */

    /* Set up 2D registers for line draw bank */
    virge_write32(ctx, VIRGE_2D_DEST_BASE, ctx->fb_base);
    virge_write32(ctx, VIRGE_2D_DEST_SRC_STR,
                  ((dest_stride & 0xFFF) << 16) | (dest_stride & 0xFFF));
    virge_write32(ctx, VIRGE_2D_CLIP_L_R,
                  (0 << 16) | ((ctx->width - 1) & 0x7FF));
    virge_write32(ctx, VIRGE_2D_CLIP_T_B,
                  (0 << 16) | ((ctx->height - 1) & 0x7FF));

    /* Foreground color */
    virge_write32(ctx, VIRGE_2D_PAT_FG_CLR, color);

    /* Line endpoints (DB019-B §15.4.4.3, PDF p.123): END0 [31:16] is the
     * first pixel drawn (bottom scanline = x0 after the Y sort), END1
     * [15:0] is the last pixel drawn (top scanline = x1). We draw every
     * pixel, so no ±1 "last pixel off" adjustment. Each 16-bit field's
     * top 5 bits are sign bits that must be 0 → mask to 11 bits (0..2047). */
    uint32_t endpoints = ((x0 & 0x7FF) << 16) | (x1 & 0x7FF);
    virge_write32(ctx, 0xA96C, endpoints);  /* L_XEND0_END1 */

    /* X DELTA and X START */
    virge_write32(ctx, 0xA970, (uint32_t)x_delta);  /* L_XDELTA */
    virge_write32(ctx, 0xA974, (uint32_t)x_start);  /* L_XSTART */
    virge_write32(ctx, 0xA978, (uint32_t)y0);       /* L_YSTART */

    /* Program Y count + direction BEFORE the command kick. With
     * autoexecute off (bit 0 of CMD_SET clear), writing CMD_SET (A900)
     * is what launches the 2D command — the same kick convention
     * rectangle fill uses (it programs RWIDTH_HEIGHT, then CMD_SET last
     * to fire). Programming Y count first guarantees the engine sees the
     * correct extent when CMD_SET fires. The worked example at PDF p.124
     * writes A97C last only because it runs with autoexecute ON, where
     * A97C itself is the per-segment kick. */
    uint32_t ycnt_dir = (dir << 31) | (y_count & 0x7FF);
    virge_write32(ctx, 0xA97C, ycnt_dir);  /* L_YCNT_DIR */

    /* Command Set for 2D line (DB019-B §15.4.4.3, PDF p.124):
     *   bits [30:27] = 0011 — 2D line command. The old code computed
     *                  this as ((0x06 << 27) & 0x1F), which is 0 — i.e.
     *                  a BitBLT, not a line. (86Box independently
     *                  decodes 2D line = 3.)
     *   bit 5  (DE) = 1  — Draw Enable: actually write pixels (same trap
     *                  as rect fill, commit 98a6169).
     *   bit 8  (MP) = 0  — mono pattern not set; for lines the hardware
     *                  forces pattern=1 selecting PAT_FG_CLR regardless.
     *   bits [24:17]  = ROP PATCOPY (0xF0); a pattern-only ROP, legal for
     *                  lines since they forbid source-containing ROPs.
     *   bit 1  (HC) = 0  — clipping disabled; see VIRGE_CMD_CLIP_ENABLE.
     *   bit 0  (AE) = 0  — single line, no autoexecute. */
    uint32_t cmd = VIRGE_2D_CMD_LINE
                 | VIRGE_2D_CMD_DRAW_ENABLE
                 | ctx->dest_format
                 | (VIRGE_ROP_PATCOPY << 17);

    virge_write32(ctx, 0xA900, cmd);  /* L_CMD_SET — kicks the line */
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

    /* Slope of edge 02 in X per scanline up; Y deltas are edge-walk
     * deltas TdAdY = -dA/dy + slope02*dA/dx (86Box; README.md). */
    float slope02 = (float)dXdY02 / (float)(1 << VIRGE_X_FRAC_BITS);

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
    /* Y deltas are edge-walk along side 02: -dA/dy + slope02*dA/dx
     * (86Box; docs/datasheets/README.md). X deltas above stay raw. */
    virge_write32(ctx, VIRGE_3D_TdGdY_dBdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(-dgdy + slope02 * dgdx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(-dbdy + slope02 * dbdx));
    virge_write32(ctx, VIRGE_3D_TdAdY_dRdY,
                  ((uint16_t)VIRGE_COLOR_FIXED(-dady + slope02 * dadx) << 16) |
                  (uint16_t)VIRGE_COLOR_FIXED(-drdy + slope02 * drdx));

    /* --- Z starts + deltas (S16.15) --- */
    virge_write32(ctx, VIRGE_3D_TZS, (uint32_t)VIRGE_Z_FIXED(v0.z));
    virge_write32(ctx, VIRGE_3D_TdZdX, (uint32_t)VIRGE_Z_FIXED(dzdx));
    virge_write32(ctx, VIRGE_3D_TdZdY,
                  (uint32_t)VIRGE_Z_FIXED(-dzdy + slope02 * dzdx));

    /* --- Texture coordinates (U, V, W with perspective) --- */
    /* Convert to hardware fixed-point */
    int32_t u_start = tex_coord_fixed(v0.u, s_val);
    int32_t v_start = tex_coord_fixed(v0.v, s_val);
    int32_t w_start = (int32_t)(v0.w * (float)(1 << 19));  /* S12.19 */

    /* dU/dX in the same format. X deltas are raw plane gradients; Y deltas
     * are edge-walk along side 02, -dA/dy + slope02*dA/dx (86Box; README). */
    int32_t du_dx = tex_coord_fixed(dudx, s_val);
    int32_t du_dy = tex_coord_fixed(-dudy + slope02 * dudx, s_val);
    int32_t dv_dx = tex_coord_fixed(dvdx, s_val);
    int32_t dv_dy = tex_coord_fixed(-dvdy + slope02 * dvdx, s_val);
    int32_t dw_dx = (int32_t)(dwdx * (float)(1 << 19));
    int32_t dw_dy = (int32_t)((-dwdy + slope02 * dwdx) * (float)(1 << 19));

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
    /* TXEND01 = edge 01 X at the bottom scanline (v0.x); TXEND12 = edge
     * 12 X at the middle scanline (v1.x). See gouraud path for derivation. */
    virge_write32(ctx, VIRGE_3D_TXEND01, (uint32_t)VIRGE_X_FIXED(v0.x));
    virge_write32(ctx, VIRGE_3D_TXEND12, (uint32_t)VIRGE_X_FIXED(v1.x));
    virge_write32(ctx, VIRGE_3D_TXS, (uint32_t)VIRGE_X_FIXED(v0.x));
    virge_write32(ctx, VIRGE_3D_TYS, (uint32_t)y_bot);

    virge_write32(ctx, VIRGE_3D_TY01_Y12,
                  ((uint32_t)lr_direction << 31) |
                  ((scan_01 & 0x7FF) << 16) |
                  (scan_12 & 0x7FF));

    /* --- Command Set: lit texture triangle with perspective ---
     * Z-buffering bits come from ctx->z_cmd_bits (cached depth state);
     * texture format/filter/blend/wrap come from ctx->tex_cmd_bits.
     * No VIRGE_CMD_CLIP_ENABLE -- see the comment in virge.h. */
    uint32_t cmd = VIRGE_CMD_3D
                 | VIRGE_3D_LIT_TEX_PERSP   /* 0101: lit texture + perspective */
                 | ctx->dest_format
                 | ctx->tex_cmd_bits         /* texture format, filter, blend, wrap */
                 | ctx->z_cmd_bits           /* ZB mode, compare code, Z update */
                 | ctx->textured_blend_bits; /* ABC: tex alpha / src alpha / none */

    virge_write32(ctx, VIRGE_3D_CMD_SET, cmd);
}

/* ========================================================================
 * Initialization
 * ======================================================================== */

int virge_init(struct virge_ctx *ctx, int width, int height, int bpp)
{
    /* The S3d engine's destination format field (2D CMD_SET bits 4-2)
     * only encodes 8/16/24bpp -- there is no 32bpp mode on this chip
     * generation (confirmed against the datasheet's own render-target
     * table). Below, any bpp that isn't 2 or 3 silently fell into the
     * 8bpp case, so a caller requesting 32bpp (4 bytes/pixel) got a
     * destination format mismatched by 4x against every width/stride
     * register it also programs, which explains fills and triangles
     * "completing" per SUBSYS_STATUS while writing to the wrong extent
     * of VRAM. Reject it explicitly instead of silently corrupting. */
    if (bpp != 1 && bpp != 2 && bpp != 3) {
        fprintf(stderr, "S3 ViRGE: unsupported bpp %d (%d bits) -- this "
                        "chip's engine only supports 8/16/24bpp "
                        "(bpp 1/2/3), not 32bpp\n", bpp, bpp * 8);
        return -EINVAL;
    }

    memset(ctx, 0, sizeof(*ctx));

    ctx->width = width;
    ctx->height = height;
    ctx->bpp = bpp;
    ctx->fb_fd = -1;
    ctx->bar_fd = -1;

    /*
     * Adopt the console mode early, before any PCI/MMIO work, so a depth
     * change routed through the kernel fbdev driver settles before our own
     * register programming runs.
     *
     * (V8) At 16bpp the S3d 3D engine's destination format is ZRGB1555
     * ONLY (3D CMD_SET, DB019-B PDF p.250) — there is no 565 triangle
     * path. The 2D engine is format-agnostic (p.232) and scanout is chosen
     * separately by CR67 bits 7-4 (Mode 9 = 15-bit/555, Mode 10 =
     * 16-bit/565, p.216). For 3D colors to scan out correctly the console
     * must be 555; on a 565 console every triangle is written 555 and
     * decoded 565 (green shifted, hues wrong — the "black/magenta/
     * stippled" triangle symptom that surfaced once V10 made colors
     * visible). Read the live layout; if it's 565, request the 555
     * equivalent via fbdev (s3fb accepts this; fixed-mode vesafb/simplefb
     * refuse). Native CR67 programming is task P6; until then refuse a
     * mismatched mode rather than ship wrong colors.
     */
    ctx->fb_fd = open("/dev/fb0", O_RDWR);
    if (ctx->fb_fd >= 0) {
        struct fb_fix_screeninfo finfo;
        if (ioctl(ctx->fb_fd, FBIOGET_FSCREENINFO, &finfo) == 0) {
            ctx->fb_size = finfo.smem_len;
            /* Real framebuffer pitch (P1). The engine's destination stride
             * MUST equal the scanout pitch or the image shears and tiles
             * diagonally ("multiple triangles"). fbdev can pad the pitch
             * past width*bpp, and the caller's width can mismatch the
             * actual console resolution -- so trust line_length, not the
             * width*bpp guess every stride site used before this fix. */
            ctx->stride = finfo.line_length ? finfo.line_length
                                            : (uint32_t)width * bpp;
        } else {
            ctx->fb_size = width * height * bpp;
            ctx->stride = (uint32_t)width * bpp;
        }

        if (bpp == 2) {
            struct fb_var_screeninfo vinfo;
            if (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) {
                printf("  FB var: %ux%u, %u bpp, green.length=%u (%s)\n",
                       vinfo.xres, vinfo.yres, vinfo.bits_per_pixel,
                       vinfo.green.length,
                       vinfo.green.length == 6 ? "RGB565" : "RGB555");

                if (vinfo.green.length == 6) {
                    /* 565 console — request the 555 equivalent. Copy the
                     * whole mode and touch only depth + color bitfields;
                     * 15bpp is still 2 bytes/pixel, so the stride is
                     * unchanged. */
                    struct fb_var_screeninfo v555 = vinfo;
                    v555.bits_per_pixel = 15;
                    v555.red    = (struct fb_bitfield){ .offset = 10, .length = 5 };
                    v555.green  = (struct fb_bitfield){ .offset =  5, .length = 5 };
                    v555.blue   = (struct fb_bitfield){ .offset =  0, .length = 5 };
                    v555.transp = (struct fb_bitfield){ .offset = 15, .length = 1 };

                    int switched =
                        (ioctl(ctx->fb_fd, FBIOPUT_VSCREENINFO, &v555) == 0) &&
                        (ioctl(ctx->fb_fd, FBIOGET_VSCREENINFO, &vinfo) == 0) &&
                        (vinfo.green.length != 6);
                    if (switched) {
                        printf("  FB var: switched to RGB555 (green.length=%u, "
                               "%u bpp) for 3D color correctness (V8)\n",
                               vinfo.green.length, vinfo.bits_per_pixel);
                    } else {
                        fprintf(stderr,
                            "S3 ViRGE: console is RGB565 but the 3D engine "
                            "writes RGB555 only (DB019-B p.250), so every\n"
                            "  triangle would scan out with shifted/wrong "
                            "colors. Switching to 15bpp via fbdev was refused "
                            "or ignored\n"
                            "  (typical of a fixed-mode vesafb/simplefb "
                            "console; s3fb would accept it). Re-run on a 555 "
                            "console — e.g. boot\n"
                            "  s3fb at 15bpp, or `fbset -rgba 5,5,5,1` — or "
                            "wait for native mode setting (P6).\n");
                        close(ctx->fb_fd);
                        ctx->fb_fd = -1;
                        return -EINVAL;
                    }
                }
            } else {
                fprintf(stderr,
                    "S3 ViRGE: FBIOGET_VSCREENINFO failed; cannot verify "
                    "16bpp is RGB555 (the 3D engine writes 555 only).\n"
                    "  If the console is 565, triangle colors will be "
                    "shifted/wrong (V8).\n");
            }
        }
    } else {
        ctx->fb_size = width * height * bpp;
        ctx->stride = (uint32_t)width * bpp;
        if (bpp == 2) {
            printf("S3 ViRGE: /dev/fb0 unavailable -- no kernel fb driver "
                   "owns the card. Will adopt the live CRTC raster and\n"
                   "  take over scanout natively (Mode 9/555) after chip "
                   "init.\n");
        }
    }

    /* Diagnostic: if these two differ, the old width*bpp stride was wrong
     * (padded pitch and/or a console resolution other than the caller's
     * request) -- the "multiple triangles" / sheared-tile symptom. */
    printf("  FB stride (line_length): %u bytes/scanline; width*bpp guess = %u\n",
           ctx->stride, (unsigned)((uint32_t)width * bpp));

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

    /* What is the CRTC actually scanning out? Read-only dump, decoded
     * and reconciled against fbdev's claims from the top of init — see
     * virge_dump_crtc_truth for the full rationale and register cites. */
    virge_dump_crtc_truth(ctx);

    /* S3d Engine software reset/enable sequence. MM8504 is Subsystem
     * STATUS on read but a completely different write-only Subsystem
     * CONTROL register on write; bits 15-14 there reset (10b) then
     * enable (01b) the engine. This is a required one-time init step
     * independent of CR40/CR53/CR66/AFC. */
    virge_write32(ctx, VIRGE_SUBSYS_CONTROL, VIRGE_SSC_S3D_RESET);
    virge_write32(ctx, VIRGE_SUBSYS_CONTROL, VIRGE_SSC_S3D_ENABLE);

    /* Enable the 8514/A-compatible accelerated register interface AND
     * linear addressing (AFC bit 4). Without bit 0, the 2D/3D command
     * bank silently ignores all writes. Without bit 4, the CPU-side
     * BAR0 aperture may still be a banked/windowed view of VRAM even
     * though the S3d engine itself writes through real linear
     * addresses -- see the VIRGE_AFC_LINEAR_ADDR comment in virge.h. */
    virge_write32(ctx, VIRGE_ADV_FUNC_CTRL,
                  VIRGE_AFC_ENABLE | VIRGE_AFC_LINEAR_ADDR);
    {
        uint32_t afc_check = virge_read32(ctx, VIRGE_ADV_FUNC_CTRL);
        printf("S3 ViRGE: AFC readback: 0x%08x (bit0 %s, bit4/LA %s)\n", afc_check,
               (afc_check & VIRGE_AFC_ENABLE) ? "set" : "NOT SET",
               (afc_check & VIRGE_AFC_LINEAR_ADDR) ? "set" : "NOT SET");
        uint32_t status_check = virge_read32(ctx, VIRGE_SUBSYS_STATUS);
        printf("S3 ViRGE: SUBSYS_STATUS readback: 0x%08x\n", status_check);
    }

    /* No fbdev driver on the card: nothing set (or will ever set) a
     * matching scanout, so adopt the live raster and own the scanout
     * format/pitch ourselves. Placed after the S3d enable sequence
     * because the takeover's vsync wait reads SUBSYS_STATUS. Only the
     * 16bpp path is supported natively (Mode 9/555 -- see V8). */
    if (ctx->fb_fd < 0 && bpp == 2)
        virge_scanout_takeover(ctx);

    /* Compute memory layout (byte offsets in VRAM).
     * VRAM size is detected from the CR36 straps (virge_detect_vram);
     * texture allocation checks against this, so an under-detected size
     * just refuses large textures rather than over-allocating.
     * ctx->width/height, not the caller's request: the takeover above
     * may have adopted the real raster. */
    ctx->fb_base = 0;
    ctx->z_base = ctx->width * ctx->height * bpp;  /* Z after framebuffer */
    ctx->vram_size = virge_detect_vram(pci.device_id);

    /* Texture heap starts after framebuffer + Z buffer, quadword aligned */
    uint32_t z_size = ctx->width * ctx->height * 2;  /* Z is always 16-bit */
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

    /* Default Z-buffering bits: NORMAL Z-buffering, LEQUAL compare, Z
     * writes on. Direct callers of the 3D paths (skipping the L10GL
     * glue) get sensible defaults; the glue overrides this with the GL
     * default (LESS) and the cached depth state at init. */
    ctx->z_cmd_bits = VIRGE_ZB_NORMAL | VIRGE_ZBC_LEQUAL | VIRGE_ZUP_ENABLE;

    /* Alpha blending off by default (ABC = 00b). The glue recomputes
     * both fields when blend state or the bound texture changes. */
    ctx->gouraud_blend_bits = VIRGE_BLEND_NONE;
    ctx->textured_blend_bits = VIRGE_BLEND_NONE;

    /* Initialize the 3D register bank */
    engine_init_3d(ctx);

    printf("S3 ViRGE: S3d Engine initialized.\n");
    printf("  Screen: %dx%d, %d bpp (stride %u)\n",
           ctx->width, ctx->height, bpp * 8, ctx->stride);
    printf("  FB base: 0x%x, Z base: 0x%x\n", ctx->fb_base, ctx->z_base);

    return 0;
}

void virge_cleanup(struct virge_ctx *ctx)
{
    virge_wait_engine(ctx);

    /* Give back a scanout we took over (native no-fbdev path) so the
     * console returns to its previous state. Before the unmap below:
     * the vsync wait reads SUBSYS_STATUS over MMIO. CR11 bit 7 write-
     * protects CR00-CR07, so unlock first and restore CR11 (with its
     * original lock bit) last. */
    if (ctx->scanout_owned && ctx->mmio) {
        printf("S3 ViRGE: restoring pre-takeover scanout registers\n");
        virge_wait_vsync(ctx);
        vga_crtc_write(0x11,
                       ctx->saved_scanout[VIRGE_SCANOUT_CR11_IDX] & 0x7F);
        for (int i = 0; i < VIRGE_SCANOUT_REG_COUNT; i++) {
            if (i == VIRGE_SCANOUT_CR11_IDX)
                continue;
            vga_crtc_write(virge_scanout_regs[i], ctx->saved_scanout[i]);
        }
        vga_crtc_write(0x11, ctx->saved_scanout[VIRGE_SCANOUT_CR11_IDX]);
        ctx->scanout_owned = 0;
    }

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
