/* Hardware-independent native-mode definitions for the S3 ViRGE. */

#ifndef VIRGE_MODE_H
#define VIRGE_MODE_H

#include <stdint.h>

#define VIRGE_MODE_HSYNC_POSITIVE (1u << 0)
#define VIRGE_MODE_VSYNC_POSITIVE (1u << 1)

struct virge_mode {
    uint16_t width;
    uint16_t height;
    uint16_t refresh_hz;
    uint32_t pixel_clock_khz;
    uint16_t hdisplay;
    uint16_t hsync_start;
    uint16_t hsync_end;
    uint16_t htotal;
    uint16_t vdisplay;
    uint16_t vsync_start;
    uint16_t vsync_end;
    uint16_t vtotal;
    uint8_t sync_flags;
};

struct virge_pll {
    uint8_t m;
    uint8_t n;
    uint8_t r;
    uint8_t sr12;
    uint8_t sr13;
    uint32_t actual_khz;
    uint32_t error_ppm;
};

#define VIRGE_CRTC_IMAGE_SIZE 0x70u
#define VIRGE_SEQ_IMAGE_SIZE  0x16u

struct virge_crtc_image {
    /* value/mask pairs make reserved-bit preservation explicit. Every CRTC
     * index with a nonzero mask must be snapshotted before the future writer
     * applies (old & ~mask) | (value & mask). */
    uint8_t value[VIRGE_CRTC_IMAGE_SIZE];
    uint8_t mask[VIRGE_CRTC_IMAGE_SIZE];
    uint8_t seq_value[VIRGE_SEQ_IMAGE_SIZE];
    uint8_t seq_mask[VIRGE_SEQ_IMAGE_SIZE];
    uint8_t misc_value;
    uint8_t misc_mask;
    uint8_t feature_value;
    uint8_t feature_mask;
    uint8_t dac_mask_value;
    uint8_t dac_mask_mask;
    uint16_t stride;
    uint16_t fifo_fetch;
    /* 25.175 MHz is a dedicated VGA clock source selected directly through
     * Misc Output. Other fixed modes use the programmable DCLK PLL. */
    uint8_t builtin_dclk_25175;
    struct virge_pll pll;
};

/* Return one of P6's fixed VESA timings. There is deliberately no general
 * modeline calculator in the native modeset path. */
const struct virge_mode *virge_mode_find(unsigned width, unsigned height,
                                          unsigned refresh_hz);

/* Validate that a fixed timing is representable by the base ViRGE CRTC in
 * the verified 16bpp/horizontal-multiply-by-two path. */
int virge_mode_validate(const struct virge_mode *mode);

/* Compute SR12/SR13 DCLK parameters from DB019-B section 9.1, PDF pp.65-66.
 * The result is constrained to the documented M/N ranges, 135-270 MHz VCO,
 * and 0.5 percent output tolerance. */
int virge_pll_compute(uint32_t target_khz, struct virge_pll *pll);

/* Build the complete P6 16bpp VGA/extended-CRTC target image without touching
 * hardware. The masks are also the exact CRTC snapshot descriptor for the
 * eventual opt-in writer. Sequencer and DAC masks describe the remaining
 * state that must be saved around PLL loading and display unblanking. */
int virge_mode_encode_16bpp(const struct virge_mode *mode, uint32_t stride,
                            uint32_t vram_size,
                            struct virge_crtc_image *image);

/* Restrict a complete 800x600 image to P6's first silicon gate: retain the
 * live vertical raster and touch only the register subset already proven by
 * the pre-P6 32->15bpp takeover, plus the new DCLK controls. */
void virge_mode_limit_first_gate(struct virge_crtc_image *image);

#endif
