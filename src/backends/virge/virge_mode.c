/* Pure timing/PLL support for P6 native ViRGE modesetting. */

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "virge_mode.h"

/* The first P6 hardware pass uses only the conservative 60 Hz entries. The
 * 75 Hz entries are defined now so the eventual register writer consumes one
 * fixed, reviewable table rather than synthesizing timings at runtime.
 * Horizontal values are canonical VESA timings and are all divisible by 8;
 * the verified ViRGE 15/16bpp path represents them at two CRTC character
 * clocks per eight pixels (equivalently, one count per four pixels). */
static const struct virge_mode virge_modes[] = {
    {  640, 480, 60, 25175,  640,  656,  752,  800,
       480, 490, 492, 525, 0 },
    {  640, 480, 75, 31500,  640,  656,  720,  840,
       480, 481, 484, 500, 0 },
    {  800, 600, 60, 40000,  800,  840,  968, 1056,
       600, 601, 605, 628,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
    {  800, 600, 75, 49500,  800,  816,  896, 1056,
       600, 601, 604, 625,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
    { 1024, 768, 60, 65000, 1024, 1048, 1184, 1344,
       768, 771, 777, 806, 0 },
    { 1024, 768, 75, 78750, 1024, 1040, 1136, 1312,
       768, 769, 772, 800,
       VIRGE_MODE_HSYNC_POSITIVE | VIRGE_MODE_VSYNC_POSITIVE },
};

int virge_mode_validate(const struct virge_mode *mode)
{
    uint64_t frame_rate_den;
    uint64_t clock_hz;
    uint64_t requested_hz;
    uint64_t error;

    if (!mode || !mode->width || !mode->height || !mode->refresh_hz ||
        !mode->pixel_clock_khz)
        return -EINVAL;
    if (mode->width != mode->hdisplay || mode->height != mode->vdisplay)
        return -EINVAL;
    if (!(mode->hdisplay < mode->hsync_start &&
          mode->hsync_start < mode->hsync_end &&
          mode->hsync_end < mode->htotal))
        return -EINVAL;
    if (!(mode->vdisplay < mode->vsync_start &&
          mode->vsync_start < mode->vsync_end &&
          mode->vsync_end < mode->vtotal))
        return -EINVAL;
    if (mode->hdisplay < 4u || mode->htotal < 20u || mode->vtotal < 2u)
        return -ERANGE;

    /* At 16bpp the hardware-verified path doubles the VGA horizontal
     * counters, so each timing boundary must be an integral 8-pixel VGA
     * character before the x2 conversion. DB019-B section 16, PDF pp.149-151
     * documents the 9-bit horizontal fields and CR5D extensions. */
    if ((mode->hdisplay | mode->hsync_start | mode->hsync_end |
         mode->htotal) & 7u)
        return -ERANGE;
    if (mode->htotal / 4u - 5u > 0x1ffu ||
        mode->hdisplay / 4u - 1u > 0x1ffu ||
        mode->vtotal - 2u > 0x7ffu ||
        mode->vdisplay - 1u > 0x7ffu ||
        mode->vsync_start > 0x7ffu)
        return -ERANGE;

    /* Reject a mislabeled fixed entry. Allow two percent for nominal refresh
     * labels such as 640x480's 59.94 Hz "60 Hz" mode. */
    frame_rate_den = (uint64_t)mode->htotal * mode->vtotal;
    clock_hz = (uint64_t)mode->pixel_clock_khz * 1000u;
    requested_hz = (uint64_t)mode->refresh_hz * frame_rate_den;
    error = clock_hz > requested_hz ? clock_hz - requested_hz
                                    : requested_hz - clock_hz;
    if (error * 100u > requested_hz * 2u)
        return -ERANGE;

    return 0;
}

const struct virge_mode *virge_mode_find(unsigned width, unsigned height,
                                          unsigned refresh_hz)
{
    size_t i;

    for (i = 0; i < sizeof(virge_modes) / sizeof(virge_modes[0]); i++) {
        const struct virge_mode *mode = &virge_modes[i];

        if (mode->width == width && mode->height == height &&
            mode->refresh_hz == refresh_hz && !virge_mode_validate(mode))
            return mode;
    }
    return NULL;
}

int virge_pll_compute(uint32_t target_khz, struct virge_pll *pll)
{
    /* DB019-B identifies the normal external crystal as 14.318 MHz
     * (pin description) and section 9.1 defines:
     *   fout = (M + 2) * fref / ((N + 2) * 2^R)
     * with M=1..127, N=1..31, R=0..3 and a 135..270 MHz VCO. */
    const uint32_t ref_khz = 14318;
    uint64_t best_diff = UINT64_MAX;
    uint32_t best_den = 0;
    unsigned best_m = 0, best_n = 0, best_r = 0;
    unsigned r, n, m;

    if (!target_khz || !pll)
        return -EINVAL;

    for (r = 0; r <= 3; r++) {
        for (n = 1; n <= 31; n++) {
            uint32_t n_div = n + 2u;
            uint32_t output_den = n_div << r;

            for (m = 1; m <= 127; m++) {
                uint64_t numerator = (uint64_t)(m + 2u) * ref_khz;
                uint64_t target_numerator;
                uint64_t diff;

                if (numerator < (uint64_t)135000u * n_div ||
                    numerator > (uint64_t)270000u * n_div)
                    continue;
                target_numerator = (uint64_t)target_khz * output_den;
                diff = numerator > target_numerator
                     ? numerator - target_numerator
                     : target_numerator - numerator;
                if (best_den && diff * best_den >= best_diff * output_den)
                    continue;
                best_diff = diff;
                best_den = output_den;
                best_m = m;
                best_n = n;
                best_r = r;
            }
        }
    }

    if (!best_den)
        return -ERANGE;

    pll->error_ppm = (uint32_t)((best_diff * 1000000u +
                       (uint64_t)target_khz * best_den / 2u) /
                      ((uint64_t)target_khz * best_den));
    if (pll->error_ppm > 5000u)
        return -ERANGE;

    pll->m = (uint8_t)best_m;
    pll->n = (uint8_t)best_n;
    pll->r = (uint8_t)best_r;
    pll->sr12 = (uint8_t)((best_r << 5) | best_n);
    pll->sr13 = (uint8_t)best_m;
    pll->actual_khz = (uint32_t)(((uint64_t)(best_m + 2u) * ref_khz +
                                  best_den / 2u) / best_den);
    return 0;
}

static void crtc_set(struct virge_crtc_image *image, unsigned index,
                     uint8_t value, uint8_t mask)
{
    image->value[index] = value;
    image->mask[index] = mask;
}

int virge_mode_encode_16bpp(const struct virge_mode *mode, uint32_t stride,
                            uint32_t vram_size,
                            struct virge_crtc_image *image)
{
    uint32_t ht, hd, hbs, hbe, hss, hse, sff;
    uint32_t vt, vd, vbs, vbe, vss, vse;
    uint32_t lsw;
    uint32_t refill;
    /* Disable split-screen by placing line compare at the largest standard
     * VGA value, 0x3ff. CR18 + CR07.4 + CR09.6 encode those ten bits. All P6
     * modes have fewer than 1024 lines, so CR5E.6 (the ViRGE-only bit 10) is
     * unnecessary. This matches the proven live 800x600 CR5E=00 state and
     * avoids changing an extended vertical bit without need (DB019-B section
     * 16 CR18, PDF p.161; section 18 CR5E, PDF p.214). */
    uint32_t line_compare = 0x3ffu;
    uint8_t cr07, cr09, cr5d, cr5e;
    unsigned i;
    int ret;

    if (!image)
        return -EINVAL;
    ret = virge_mode_validate(mode);
    if (ret)
        return ret;
    if ((stride & 7u) || stride < (uint32_t)mode->width * 2u)
        return -EINVAL;
    lsw = stride / 8u; /* Hardware-verified ViRGE packed-pixel pitch unit. */
    if (lsw > 0x3ffu)
        return -ERANGE;
    if (vram_size < 1024u * 1024u)
        return -ERANGE;

    memset(image, 0, sizeof(*image));
    image->stride = (uint16_t)stride;
    image->builtin_dclk_25175 = mode->pixel_clock_khz == 25175u;
    if (image->builtin_dclk_25175) {
        /* DB019-B section 9.2 specifies 25.175 MHz as a dedicated VGA DCLK
         * selected by Misc Output clock bits 00. Do not synthesize an
         * approximation through SR12/SR13 for the standard VGA mode. */
        image->pll.actual_khz = 25175u;
    } else {
        ret = virge_pll_compute(mode->pixel_clock_khz, &image->pll);
        if (ret)
            return ret;
    }

    /* The verified 15/16bpp path uses two CRTC character clocks per normal
     * eight-pixel VGA character: all horizontal boundaries become pixels/4.
     * DB019-B section 16 (PDF pp.149-151) defines CR00-CR05 and section 18
     * (PDF p.214) supplies their ninth/extra end bits in CR5D. */
    ht = mode->htotal / 4u;
    hd = mode->hdisplay / 4u;
    hbs = hd;
    /* The working BIOS-derived takeover ends blank four x2 character clocks
     * before the counter wraps (800x600: 260 of 264), preserving the original
     * two VGA-character margin. */
    hbe = ht - 4u;
    hss = mode->hsync_start / 4u;
    hse = mode->hsync_end / 4u;

    /* On the target 800x600 mode the BIOS reserved 3 us from SFF to the end
     * of the line for FIFO refill: (264-234)*4/40MHz. Preserve that verified
     * time budget at other clocks, rounding toward an earlier fetch. CR3B
     * must remain inside horizontal blank (DB019-B section 18, PDF p.203). */
    refill = (mode->pixel_clock_khz * 3u + 3999u) / 4000u;
    if (!refill || refill >= ht - hbs)
        return -ERANGE;
    sff = ht - refill;
    if (sff < hbs || sff >= hbe || sff > 0x1ffu)
        return -ERANGE;
    image->fifo_fetch = (uint16_t)sff;

    vt = mode->vtotal - 2u;
    vd = mode->vdisplay - 1u;
    vbs = vd;
    vbe = mode->vtotal - 1u;
    vss = mode->vsync_start;
    vse = mode->vsync_end;

    cr07 = (uint8_t)((((vt >> 8) & 1u) << 0) |
                     (((vd >> 8) & 1u) << 1) |
                     (((vss >> 8) & 1u) << 2) |
                     (((vbs >> 8) & 1u) << 3) |
                     (((line_compare >> 8) & 1u) << 4) |
                     (((vt >> 9) & 1u) << 5) |
                     (((vd >> 9) & 1u) << 6) |
                     (((vss >> 9) & 1u) << 7));
    cr09 = (uint8_t)((((vbs >> 9) & 1u) << 5) |
                     (((line_compare >> 9) & 1u) << 6));
    cr5d = (uint8_t)(((((ht - 5u) >> 8) & 1u) << 0) |
                     ((((hd - 1u) >> 8) & 1u) << 1) |
                     (((hbs >> 8) & 1u) << 2) |
                     (((hbe >> 6) & 1u) << 3) |
                     (((hss >> 8) & 1u) << 4) |
                     (((hse >> 5) & 1u) << 5) |
                     (((sff >> 8) & 1u) << 6));
    cr5e = (uint8_t)((((vt >> 10) & 1u) << 0) |
                     (((vd >> 10) & 1u) << 1) |
                     (((vbs >> 10) & 1u) << 2) |
                     (((vss >> 10) & 1u) << 4) |
                     (((line_compare >> 10) & 1u) << 6));

    /* Standard VGA CRTC image. CR11 bit 7 is the final CR00-CR07 lock;
     * the future writer must first write this byte with bit 7 clear, program
     * CR00-CR07, then restore the encoded locked value. Vertical field and
     * addressing definitions: DB019-B section 16, PDF pp.152-161. */
    for (i = 0; i <= 0x18; i++)
        image->mask[i] = 0xff;
    image->value[0x00] = (uint8_t)(ht - 5u);
    image->value[0x01] = (uint8_t)(hd - 1u);
    image->value[0x02] = (uint8_t)hbs;
    image->value[0x03] = (uint8_t)(hbe & 0x1fu);
    image->value[0x04] = (uint8_t)hss;
    image->value[0x05] = (uint8_t)(((hbe >> 5) & 1u) << 7 |
                                   (hse & 0x1fu));
    image->value[0x06] = (uint8_t)vt;
    image->value[0x07] = cr07;
    image->value[0x08] = 0x00; /* no preset row scan / byte pan */
    image->value[0x09] = cr09; /* one scanline; split-screen overflow only */
    image->value[0x0a] = 0x20; /* disable VGA text cursor */
    image->value[0x0b] = 0x00;
    image->value[0x0c] = 0x00; /* display start = VRAM byte offset zero */
    image->value[0x0d] = 0x00;
    image->value[0x0e] = 0x00;
    image->value[0x0f] = 0x00;
    image->value[0x10] = (uint8_t)vss;
    image->value[0x11] = (uint8_t)(0xa0u | (vse & 0x0fu));
    image->value[0x12] = (uint8_t)vd;
    image->value[0x13] = (uint8_t)lsw;
    image->value[0x14] = 0x00;
    image->value[0x15] = (uint8_t)vbs;
    image->value[0x16] = (uint8_t)vbe;
    image->value[0x17] = 0xe3; /* retrace on, byte mode, linear graphics */
    image->value[0x18] = (uint8_t)line_compare;

    /* Extended packed-pixel state. Masks exclude reserved/unrelated bits and
     * therefore double as the exact save set. CR31/CR51/CR69 clear all old
     * display-start extensions; CR3A selects >=8bpp enhanced mode; CR34/3B
     * enable and position FIFO fetch; CR43/51 encode pitch; CR67 selects
     * Mode 9 RGB555 with Streams disabled. DB019-B section 18, PDF pp.192-216.
     * CR50 pixel length is retained from the hardware-verified takeover; that
     * register is not described in DB019-B. */
    crtc_set(image, 0x31, 0x08, 0x38);
    /* Force VCLK from the internal DCLK. CR33.3=0 permits an external
     * feature-connector VCLK when that path is enabled; CR33.3=1 explicitly
     * selects inverted internal DCLK (DB019-B section 18, PDF p.194). */
    crtc_set(image, 0x33, 0x08, 0x08);
    crtc_set(image, 0x34, 0x10, 0x10);
    crtc_set(image, 0x35, 0x00, 0x30); /* timing registers unlocked */
    crtc_set(image, 0x3a, 0x10, 0x18);
    crtc_set(image, 0x3b, (uint8_t)sff, 0xff);
    crtc_set(image, 0x42, 0x00, 0x20); /* progressive, not interlaced */
    crtc_set(image, 0x43, 0x00, 0x04);
    crtc_set(image, 0x50, 0x10, 0x30);
    crtc_set(image, 0x51, (uint8_t)(((lsw >> 8) & 3u) << 4), 0x33);
    crtc_set(image, 0x55, 0x00, 0x80); /* VCLK output enabled */
    crtc_set(image, 0x56, 0x00, 0x06); /* HSYNC/VSYNC outputs enabled */
    crtc_set(image, 0x58,
             (uint8_t)(0x10u | (vram_size >= 4u * 1024u * 1024u ? 3u :
                                vram_size >= 2u * 1024u * 1024u ? 2u : 1u)),
             0x13);
    crtc_set(image, 0x5d, cr5d, 0x7f);
    crtc_set(image, 0x5e, cr5e, 0x57);
    crtc_set(image, 0x67, 0x30, 0xfc);
    crtc_set(image, 0x69, 0x00, 0x0f);

    /* Misc Output: color CRTC ports, RAM enable, DCLK source, and VESA sync
     * polarities. Clock select 00 is the exact built-in 25.175 MHz VGA DCLK;
     * 11 selects programmable SR12/SR13. Preserve unrelated bits 5-4. VGA
     * polarity bits are 1 for negative sync and 0 for positive sync. */
    image->misc_mask = 0xcf;
    image->misc_value = image->builtin_dclk_25175 ? 0x03 : 0x0f;
    if (!(mode->sync_flags & VIRGE_MODE_HSYNC_POSITIVE))
        image->misc_value |= 0x40;
    if (!(mode->sync_flags & VIRGE_MODE_VSYNC_POSITIVE))
        image->misc_value |= 0x80;

    /* Feature Control bit 3 ORs vertical sync with active display when set.
     * Native scanout always needs the normal external VSYNC waveform. */
    image->feature_value = 0x00;
    image->feature_mask = 0x08;

    /* Extended sequencer access must be unlocked through SR08 before SR12,
     * SR13, and SR15 are touched (DB019-B section 13.2.1, PDF p.90). SR15
     * bit 5 is toggled 0->1->0 by the future writer to load this PLL image.
     * SR01 is included so the writer can blank atomically and restore its
     * original state. The masks make all five original bytes part of the
     * save set. */
    image->seq_value[0x08] = 0x06;
    image->seq_mask[0x08] = 0xff;
    /* SR0D owns the external feature connector and DPMS sync overrides.
     * Disable feature-connector direction control and select normal H/V sync
     * output, preserving the unrelated LPB selector and reserved bits. */
    image->seq_value[0x0d] = 0x00;
    image->seq_mask[0x0d] = 0xf1;
    image->seq_value[0x01] = 0x00; /* screen on after the atomic load */
    image->seq_mask[0x01] = 0x20;
    image->seq_value[0x12] = image->pll.sr12;
    image->seq_mask[0x12] = 0x7f;
    image->seq_value[0x13] = image->pll.sr13;
    image->seq_mask[0x13] = 0x7f;
    /* Fixed VGA clocks load when SR15.1 is set; programmable clocks are
     * loaded immediately by pulsing SR15.5. Include every bit changed by
     * either path in the snapshot descriptor. */
    image->seq_value[0x15] = image->builtin_dclk_25175 ? 0x02 : 0x00;
    image->seq_mask[0x15] = image->builtin_dclk_25175 ? 0x22 : 0x20;

    /* DB019-B section 13.1 releases display blanking with DAC mask FFh after
     * the CRTC load. Snapshot the old mask before the writer changes it. */
    image->dac_mask_value = 0xff;
    image->dac_mask_mask = 0xff;

    return 0;
}

void virge_mode_limit_first_gate(struct virge_crtc_image *image)
{
    static const uint8_t proven_crtc[] = {
        /* Horizontal timing set used by virge_scanout_takeover. */
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        /* CR11 bit 7 is needed only to unlock/relock CR00-CR05. */
        0x11,
        /* Display start, pitch, FIFO, depth, and extended horizontal bits. */
        0x0c, 0x0d, 0x13, 0x31, 0x34, 0x3b, 0x43, 0x50, 0x51,
        0x5d, 0x67, 0x69,
    };
    uint8_t keep[VIRGE_CRTC_IMAGE_SIZE] = {0};
    unsigned i;

    if (!image)
        return;
    for (i = 0; i < sizeof(proven_crtc) / sizeof(proven_crtc[0]); i++)
        keep[proven_crtc[i]] = 1;
    for (i = 0; i < VIRGE_CRTC_IMAGE_SIZE; i++) {
        if (!keep[i])
            image->mask[i] = 0;
    }

    /* Do not change the live raster's sync polarities during the clock-only
     * gate. Bits 3-2 select the programmable DCLK; bits 1-0 retain RAM and
     * color-CRTC access. The complete encoder still owns polarity for the
     * later true resolution-change gate. */
    image->misc_mask &= 0x0fu;
    image->feature_mask = 0;
    image->seq_mask[0x0d] = 0;

    /* Preserve CR11's interrupt/retrace bits; only its timing lock is part of
     * this transaction. CR35 is deliberately absent because the proven
     * takeover writes this subset without disturbing S3's timing locks. */
    image->mask[0x03] = 0x1f; /* preserve live display skew */
    image->mask[0x05] = 0x9f; /* preserve live horizontal-sync skew */
    image->mask[0x11] = 0x80;
}
