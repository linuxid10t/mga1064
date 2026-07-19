/* Hardware-independent tests for P6 fixed modes and DCLK PLL selection. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "backends/virge/virge.h"
#include "backends/virge/virge_mode.h"

static int failed;

#define EXPECT(condition, label) do {                                     \
    if (!(condition)) {                                                   \
        fprintf(stderr, "test-virge-mode: FAIL: %s\n", (label));       \
        failed = 1;                                                       \
    }                                                                     \
} while (0)

static void test_fixed_modes(void)
{
    static const struct {
        unsigned width, height, refresh, clock;
        unsigned hs, he, ht, vs, ve, vt, flags;
    } expected[] = {
        {  640, 480, 60, 25175,  656,  752,  800, 490, 492, 525, 0 },
        {  640, 480, 75, 31500,  656,  720,  840, 481, 484, 500, 0 },
        {  800, 600, 60, 40000,  840,  968, 1056, 601, 605, 628, 3 },
        {  800, 600, 75, 49500,  816,  896, 1056, 601, 604, 625, 3 },
        { 1024, 768, 60, 65000, 1048, 1184, 1344, 771, 777, 806, 0 },
        { 1024, 768, 75, 78750, 1040, 1136, 1312, 769, 772, 800, 3 },
    };
    unsigned i;

    for (i = 0; i < sizeof(expected) / sizeof(expected[0]); i++) {
        const struct virge_mode *mode =
            virge_mode_find(expected[i].width, expected[i].height,
                            expected[i].refresh);
        struct virge_pll pll;
        struct virge_crtc_image image;
        unsigned ht, hde, vtotal, vde, vrs, vbs, line_compare, lsw;

        EXPECT(mode != NULL, "find fixed mode");
        if (!mode)
            continue;
        EXPECT(virge_mode_validate(mode) == 0, "validate fixed mode");
        EXPECT(mode->pixel_clock_khz == expected[i].clock,
               "fixed pixel clock");
        EXPECT(mode->hsync_start == expected[i].hs &&
               mode->hsync_end == expected[i].he &&
               mode->htotal == expected[i].ht,
               "fixed horizontal timing");
        EXPECT(mode->vsync_start == expected[i].vs &&
               mode->vsync_end == expected[i].ve &&
               mode->vtotal == expected[i].vt,
               "fixed vertical timing");
        EXPECT(mode->sync_flags == expected[i].flags,
               "fixed sync polarity");
        EXPECT(virge_pll_compute(mode->pixel_clock_khz, &pll) == 0 &&
               pll.error_ppm <= 5000,
               "fixed mode has representable DCLK");
        EXPECT(virge_mode_encode_16bpp(mode, mode->width * 2u,
                                       4u * 1024u * 1024u, &image) == 0,
               "fixed mode has representable CRTC image");
        hde = image.value[0x01] |
              (unsigned)((image.value[0x5d] >> 1) & 1u) << 8;
        ht = image.value[0x00] |
             (unsigned)(image.value[0x5d] & 1u) << 8;
        vde = image.value[0x12] |
              (unsigned)((image.value[0x07] >> 1) & 1u) << 8 |
              (unsigned)((image.value[0x07] >> 6) & 1u) << 9 |
              (unsigned)((image.value[0x5e] >> 1) & 1u) << 10;
        vtotal = image.value[0x06] |
                 (unsigned)(image.value[0x07] & 1u) << 8 |
                 (unsigned)((image.value[0x07] >> 5) & 1u) << 9 |
                 (unsigned)(image.value[0x5e] & 1u) << 10;
        vrs = image.value[0x10] |
              (unsigned)((image.value[0x07] >> 2) & 1u) << 8 |
              (unsigned)((image.value[0x07] >> 7) & 1u) << 9 |
              (unsigned)((image.value[0x5e] >> 4) & 1u) << 10;
        vbs = image.value[0x15] |
              (unsigned)((image.value[0x07] >> 3) & 1u) << 8 |
              (unsigned)((image.value[0x09] >> 5) & 1u) << 9 |
              (unsigned)((image.value[0x5e] >> 2) & 1u) << 10;
        line_compare = image.value[0x18] |
                       (unsigned)((image.value[0x07] >> 4) & 1u) << 8 |
                       (unsigned)((image.value[0x09] >> 6) & 1u) << 9 |
                       (unsigned)((image.value[0x5e] >> 6) & 1u) << 10;
        lsw = image.value[0x13] |
              (unsigned)((image.value[0x51] >> 4) & 3u) << 8;
        EXPECT((ht + 5u) * 4u == mode->htotal &&
               (hde + 1u) * 4u == mode->width &&
               vtotal + 2u == mode->vtotal &&
               vde + 1u == mode->height &&
               vrs == mode->vsync_start && vbs + 1u == mode->vdisplay &&
               image.value[0x16] ==
                   (uint8_t)(((vbs - 1u) +
                              (mode->vtotal - mode->vdisplay)) & 0xffu) &&
               line_compare == 0x3ffu &&
               lsw * 8u == mode->width * 2u,
               "CRTC image decodes to requested complete timing and pitch");
    }

    EXPECT(virge_mode_find(1280, 720, 60) == NULL,
           "reject unsupported geometry");
    EXPECT(virge_mode_find(640, 480, 72) == NULL,
           "reject unsupported refresh");
}

static void test_mode_validation(void)
{
    struct virge_mode mode = *virge_mode_find(640, 480, 60);

    mode.hsync_start++;
    EXPECT(virge_mode_validate(&mode) == -ERANGE,
           "reject non-character horizontal timing");
    mode = *virge_mode_find(640, 480, 60);
    mode.hsync_end = mode.hsync_start;
    EXPECT(virge_mode_validate(&mode) == -EINVAL,
           "reject unordered horizontal timing");
    mode = *virge_mode_find(640, 480, 60);
    mode.pixel_clock_khz = 40000;
    EXPECT(virge_mode_validate(&mode) == -ERANGE,
           "reject mislabeled refresh");
    EXPECT(virge_mode_validate(NULL) == -EINVAL, "reject null mode");
}

static void expect_pll(uint32_t target, uint8_t sr12, uint8_t sr13,
                       uint32_t actual, const char *label)
{
    struct virge_pll pll = {0};
    int ret = virge_pll_compute(target, &pll);

    if (ret || pll.sr12 != sr12 || pll.sr13 != sr13 ||
        pll.actual_khz != actual || pll.error_ppm > 5000) {
        fprintf(stderr,
                "test-virge-mode: FAIL: %s: ret=%d SR12=%02x SR13=%02x "
                "actual=%u error=%uppm\n",
                label, ret, pll.sr12, pll.sr13, pll.actual_khz,
                pll.error_ppm);
        failed = 1;
    }
}

static void test_pll(void)
{
    struct virge_pll pll;

    /* Exact expected encodings using the documented 14.318 MHz reference. */
    expect_pll(25175, 0x67, 0x7d, 25255, "25.175 MHz");
    expect_pll(31500, 0x63, 0x56, 31500, "31.500 MHz");
    expect_pll(40000, 0x49, 0x79, 40025, "40.000 MHz");
    expect_pll(49500, 0x44, 0x51, 49516, "49.500 MHz");
    expect_pll(65000, 0x44, 0x6b, 65028, "65.000 MHz");

    EXPECT(virge_pll_compute(0, &pll) == -EINVAL, "reject zero clock");
    EXPECT(virge_pll_compute(10000, &pll) == -ERANGE,
           "reject clock below PLL range");
    EXPECT(virge_pll_compute(40000, NULL) == -EINVAL,
           "reject null PLL output");
}

static unsigned snapshot_count(const struct virge_crtc_image *image)
{
    unsigned count = 0;
    unsigned i;

    for (i = 0; i < VIRGE_CRTC_IMAGE_SIZE; i++)
        count += image->mask[i] != 0;
    return count;
}

static void test_crtc_image(void)
{
    const struct virge_mode *mode = virge_mode_find(800, 600, 60);
    struct virge_crtc_image image;
    static const uint8_t extended[] = {
        0x31, 0x33, 0x34, 0x35, 0x3a, 0x3b, 0x42, 0x43, 0x50,
        0x51, 0x55, 0x56, 0x58, 0x5d, 0x5e, 0x67, 0x69,
    };
    unsigned i;

    EXPECT(virge_mode_encode_16bpp(mode, 1600, 4u * 1024u * 1024u,
                                   &image) == 0,
           "encode 800x600 CRTC image");
    EXPECT(image.value[0x00] == 0x03 && image.value[0x01] == 0xc7 &&
           image.value[0x02] == 0xc8 && image.value[0x04] == 0xd2,
           "800x600 horizontal low bytes");
    EXPECT(image.value[0x06] == 0x72 && image.value[0x07] == 0xf0 &&
           image.value[0x10] == 0x59 && image.value[0x12] == 0x57,
           "800x600 vertical bytes");
    EXPECT(image.value[0x13] == 0xc8 && image.value[0x17] == 0xe3,
           "800x600 pitch and addressing");
    EXPECT(image.value[0x3b] == 0xea && image.fifo_fetch == 234 &&
           image.value[0x5d] == 0x01,
           "800x600 x2/SFF bytes without false pulse extensions");
    EXPECT(image.value[0x33] == 0x00 && image.value[0x50] == 0x10 &&
           image.value[0x51] == 0x00 &&
           image.value[0x67] == 0x30,
           "normal VCLK, RGB555 pixel length, and scanout mode");
    EXPECT(image.value[0x58] == 0x13 && image.misc_value == 0x0f &&
           image.misc_mask == 0xcf,
           "4MB linear window and positive sync");
    EXPECT(!image.builtin_dclk_25175,
           "800x600 uses programmable DCLK");
    EXPECT(image.feature_value == 0 && image.feature_mask == 0x08 &&
           image.seq_value[0x0d] == 0 && image.seq_mask[0x0d] == 0xf1,
           "normal external sync path is in save image");
    EXPECT(image.seq_mask[0x01] == 0x20 &&
           image.seq_value[0x08] == 0x06 &&
           image.seq_value[0x12] == image.pll.sr12 &&
           image.seq_value[0x13] == image.pll.sr13 &&
           image.seq_mask[0x15] == 0x20,
           "sequencer unlock, PLL, and load-bit save image");
    EXPECT(image.dac_mask_value == 0xff && image.dac_mask_mask == 0xff,
           "DAC unblank/save image");
    EXPECT(snapshot_count(&image) == 25u +
           sizeof(extended) / sizeof(extended[0]),
           "complete standard plus extended CRTC save set");
    for (i = 0; i <= 0x18; i++)
        EXPECT(image.mask[i] == 0xff, "standard CRTC fully snapshotted");
    for (i = 0; i < sizeof(extended) / sizeof(extended[0]); i++)
        EXPECT(image.mask[extended[i]] != 0,
               "extended CRTC register snapshotted");

    mode = virge_mode_find(800, 600, 75);
    EXPECT(virge_mode_encode_16bpp(mode, 1600, 4u * 1024u * 1024u,
                                   &image) == 0,
           "encode 800x600@75 CRTC image");
    {
        static const uint8_t expected_standard[25] = {
            0x03, 0xc7, 0xc8, 0x04, 0xcc, 0x00, 0x6f, 0xf0,
            0x00, 0x60, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
            0x59, 0xac, 0x57, 0xc8, 0x00, 0x57, 0x6f, 0xe3,
            0xff,
        };

        for (i = 0; i < sizeof(expected_standard); i++)
            EXPECT(image.value[i] == expected_standard[i],
                   "800x600@75 standard CRTC byte matches DB019-B encoding");
    }
    EXPECT(image.value[0x3b] == 0xe2 && image.fifo_fetch == 226 &&
           image.value[0x5d] == 0x01 && image.value[0x5e] == 0x00,
           "800x600@75 FIFO and extended overflow bytes");
    EXPECT(image.value[0x15] == 0x57 && image.value[0x16] == 0x6f,
           "800x600@75 vertical blank ends before the 625-line wrap");
    EXPECT(image.value[0x13] == 0xc8 && image.misc_value == 0x0f &&
           image.pll.sr12 == 0x44 && image.pll.sr13 == 0x51 &&
           image.pll.actual_khz == 49516 && image.pll.error_ppm == 332,
           "800x600@75 pitch, polarity, and programmable 49.5MHz clock");

    mode = virge_mode_find(640, 480, 60);
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 2u * 1024u * 1024u,
                                   &image) == 0 &&
           image.value[0x58] == 0x12 && image.misc_value == 0xc3 &&
           image.builtin_dclk_25175,
           "2MB window, negative sync, and built-in VGA DCLK encoding");
    {
        static const uint8_t expected_standard[25] = {
            0xc3, 0x9f, 0xa0, 0x04, 0xa4, 0x1c, 0x0b, 0x3e,
            0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xea, 0xac, 0xdf, 0xa0, 0x00, 0xdf, 0x0b, 0xe3,
            0xff,
        };

        for (i = 0; i < sizeof(expected_standard); i++)
            EXPECT(image.value[i] == expected_standard[i],
                   "640x480 standard CRTC byte matches DB019-B encoding");
    }
    EXPECT(image.value[0x3b] == 0xb5 && image.fifo_fetch == 181 &&
           image.value[0x5d] == 0x00 && image.value[0x5e] == 0x00,
           "640x480 FIFO and extended overflow bytes");
    EXPECT(image.value[0x13] == 0xa0 && image.value[0x51] == 0x00 &&
           image.pll.actual_khz == 25175 && image.pll.error_ppm == 0,
           "640x480 pitch and exact built-in 25.175MHz clock");
    EXPECT(image.seq_value[0x15] == 0x02 &&
           image.seq_mask[0x15] == 0x22,
           "built-in VGA DCLK enable is included in save image");

    mode = virge_mode_find(640, 480, 75);
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 4u * 1024u * 1024u,
                                   &image) == 0,
           "encode 640x480@75 CRTC image");
    {
        static const uint8_t expected_standard[25] = {
            0xcd, 0x9f, 0xa0, 0x0e, 0xa4, 0x14, 0xf2, 0x1f,
            0x00, 0x40, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00,
            0xe1, 0xa4, 0xdf, 0xa0, 0x00, 0xdf, 0xf2, 0xe3,
            0xff,
        };

        for (i = 0; i < sizeof(expected_standard); i++)
            EXPECT(image.value[i] == expected_standard[i],
                   "640x480@75 standard CRTC byte matches DB019-B encoding");
    }
    EXPECT(image.value[0x3b] == 0xba && image.fifo_fetch == 186 &&
           image.value[0x5d] == 0x00 && image.value[0x5e] == 0x00,
           "640x480@75 FIFO and extended overflow bytes");
    EXPECT(image.value[0x15] == 0xdf && image.value[0x16] == 0xf2,
           "640x480@75 vertical blank ends before the 500-line wrap");
    EXPECT(image.value[0x13] == 0xa0 && image.misc_value == 0xcf &&
           !image.builtin_dclk_25175 && image.pll.sr12 == 0x63 &&
           image.pll.sr13 == 0x56 && image.pll.actual_khz == 31500 &&
           image.pll.error_ppm == 13,
           "640x480@75 pitch, polarity, and programmable 31.5MHz clock");
    EXPECT(image.seq_value[0x15] == 0x00 &&
           image.seq_mask[0x15] == 0x20,
           "640x480@75 uses the immediate programmable-DCLK load pulse");

    mode = virge_mode_find(640, 480, 60);
    EXPECT(virge_mode_encode_16bpp(mode, 1279, 2u * 1024u * 1024u,
                                   &image) == -EINVAL,
           "reject unaligned pitch");
    EXPECT(virge_mode_encode_16bpp(mode, 1024, 2u * 1024u * 1024u,
                                   &image) == -EINVAL,
           "reject pitch narrower than scanout");
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 512u * 1024u,
                                   &image) == -ERANGE,
           "reject unsupported linear-window size");
    EXPECT(virge_mode_encode_16bpp(mode, 1280, 2u * 1024u * 1024u,
                                   NULL) == -EINVAL,
           "reject null CRTC image");

    mode = virge_mode_find(1024, 768, 60);
    EXPECT(virge_mode_encode_16bpp(mode, 2048, 4u * 1024u * 1024u,
                                   &image) == 0 &&
           (image.value[0x5d] & 0x28) == 0x28,
           "pulse extensions are used only for widths beyond base wrapping fields");
}

static void test_first_gate_image(void)
{
    const struct virge_mode *mode = virge_mode_find(800, 600, 60);
    struct virge_crtc_image image;

    EXPECT(virge_mode_encode_16bpp(mode, 1600, 4u * 1024u * 1024u,
                                   &image) == 0,
           "encode first-gate source image");
    virge_mode_limit_first_gate(&image);

    EXPECT(image.mask[0x00] && image.mask[0x05] &&
           image.mask[0x0c] && image.mask[0x0d] && image.mask[0x13] &&
           image.mask[0x31] && image.mask[0x3b] && image.mask[0x43] &&
           image.mask[0x50] && image.mask[0x51] && image.mask[0x5d] &&
           image.mask[0x67] && image.mask[0x69],
           "first gate retains proven takeover register subset");
    EXPECT(image.mask[0x11] == 0x80,
           "first gate owns only the CR00-07 lock bit in CR11");
    EXPECT(image.mask[0x03] == 0x1f && image.mask[0x05] == 0x9f,
           "first gate preserves live horizontal skew bits");
    EXPECT(!image.mask[0x06] && !image.mask[0x07] &&
           !image.mask[0x08] && !image.mask[0x09] &&
           !image.mask[0x10] && !image.mask[0x12] &&
           !image.mask[0x14] && !image.mask[0x15] &&
           !image.mask[0x16] && !image.mask[0x17] &&
           !image.mask[0x18] && !image.mask[0x35] && !image.mask[0x5e],
           "first gate preserves live vertical/addressing timing state");
    EXPECT(image.misc_mask == 0x0f,
           "first gate changes DCLK select without changing sync polarity");
    EXPECT(image.feature_mask == 0 && image.seq_mask[0x0d] == 0,
           "first gate preserves live external sync routing");
    EXPECT(image.value[0x5d] == 0x21,
           "first gate preserves silicon-verified CR5D pulse byte");
    EXPECT(image.seq_mask[0x12] && image.seq_mask[0x13] &&
           image.seq_mask[0x15],
           "first gate retains programmable DCLK load sequence");
}

static void test_fifo_status_decode(void)
{
    EXPECT(VIRGE_FIFO_DEPTH == 16u, "documented S3d FIFO depth");
    EXPECT(virge_fifo_slots_free(0) == 0u,
           "decode empty S3d FIFO capacity");
    EXPECT(virge_fifo_slots_free(1u << 8) == 1u,
           "decode one free S3d FIFO slot");
    EXPECT(virge_fifo_slots_free(16u << 8) == 16u,
           "decode all free S3d FIFO slots");
    EXPECT(virge_fifo_slots_free(VIRGE_STATUS_3DIDLE |
                                 VIRGE_STATUS_VSYNC | (9u << 8)) == 9u,
           "FIFO decode ignores adjacent status bits");
}

static void test_state_cache(void)
{
    struct virge_state_cache cache = {0};
    uint32_t desired[VIRGE_STATE_CACHE_REGS];
    uint32_t dirty;
    unsigned i;

    for (i = 0; i < VIRGE_STATE_CACHE_REGS; i++)
        desired[i] = 0x10u * (i + 1u);

    dirty = virge_state_dirty_mask(&cache, desired,
                                   VIRGE_STATE_CACHE_REGS);
    EXPECT(dirty == 0xffffu && virge_state_dirty_count(dirty) == 16u,
           "empty full-size state cache marks every register dirty");
    for (i = 0; i < VIRGE_STATE_CACHE_REGS; i++)
        virge_state_commit(&cache, i, desired[i]);
    EXPECT(virge_state_dirty_mask(&cache, desired,
                                  VIRGE_STATE_CACHE_REGS) == 0,
           "committed state cache suppresses identical values");

    desired[2] = 0x31;
    dirty = virge_state_dirty_mask(&cache, desired,
                                   VIRGE_STATE_CACHE_REGS);
    EXPECT(dirty == (1u << 2) && virge_state_dirty_count(dirty) == 1u,
           "state cache isolates one changed register");
    virge_state_commit(&cache, 2, desired[2]);
    virge_state_invalidate(&cache, (1u << 3) | (1u << 14));
    dirty = virge_state_dirty_mask(&cache, desired,
                                   VIRGE_STATE_CACHE_REGS);
    EXPECT(dirty == ((1u << 3) | (1u << 14)) &&
           virge_state_dirty_count(dirty) == 2u,
           "targeted invalidation re-emits only clobbered registers");
}

static void test_presentation_config(void)
{
    struct virge_buffer_layout synced;
    struct virge_buffer_layout direct;
    int enabled = -1;

    EXPECT(virge_parse_vsync(NULL, &enabled) == 0 && enabled == 1,
           "unset vsync selects synchronized presentation");
    EXPECT(virge_parse_vsync("", &enabled) == 0 && enabled == 1,
           "empty vsync selects synchronized presentation");
    EXPECT(virge_parse_vsync("1", &enabled) == 0 && enabled == 1,
           "vsync one selects synchronized presentation");
    EXPECT(virge_parse_vsync("0", &enabled) == 0 && enabled == 0,
           "vsync zero selects direct-front presentation");
    EXPECT(virge_parse_vsync("off", &enabled) == -EINVAL,
           "invalid vsync value is rejected");
    EXPECT(virge_parse_vsync("0", NULL) == -EINVAL,
           "null vsync destination is rejected");

    EXPECT(virge_buffer_layout_compute(1600, 600, 800u * 600u * 2u,
                                       4u * 1024u * 1024u, 1,
                                       &synced) == 0,
           "800x600 synchronized layout fits 4MB");
    EXPECT(synced.front_base == 0 && synced.back_base == 960000u &&
           synced.z_base == 1920000u && synced.texture_base == 2880000u,
           "synchronized layout has two color buffers before Z");

    EXPECT(virge_buffer_layout_compute(1600, 600, 800u * 600u * 2u,
                                       4u * 1024u * 1024u, 0,
                                       &direct) == 0,
           "800x600 direct-front layout fits 4MB");
    EXPECT(direct.front_base == 0 && direct.back_base == 0 &&
           direct.z_base == 960000u && direct.texture_base == 1920000u,
           "direct-front layout has one color buffer before Z");
    EXPECT(synced.texture_base - direct.texture_base == 960000u,
           "direct-front layout reclaims exactly one color buffer");

    EXPECT(virge_buffer_layout_compute(1600, 600, 960000,
                                       1500000, 0, &direct) == -ENOSPC,
           "layout rejects insufficient VRAM");
    EXPECT(virge_buffer_layout_compute(UINT32_MAX, UINT32_MAX, 0,
                                       UINT32_MAX, 1, &direct) == -EOVERFLOW,
           "layout rejects 32-bit offset overflow");
    EXPECT(virge_buffer_layout_compute(1600, 600, 960000,
                                       UINT32_MAX, 2, &direct) == -EINVAL,
           "layout rejects invalid presentation flag");
    EXPECT(virge_buffer_layout_compute(1600, 600, 960000,
                                       UINT32_MAX, 0, NULL) == -EINVAL,
           "layout rejects null result");
}

static void test_autoexecute(void)
{
    struct virge_state_cache command_cache = {0};
    uint32_t command = VIRGE_CMD_3D | VIRGE_3D_GOURAUD |
                       VIRGE_DEST_16BPP | VIRGE_ZBC_LESS;
    uint32_t enabled = virge_autoexec_command(command);
    uint32_t desired[1] = { enabled };

    EXPECT(virge_parse_autoexec("0", NULL) == -EINVAL,
           "null autoexecute destination is rejected");
    {
        int value = -1;
        EXPECT(virge_parse_autoexec(NULL, &value) == 0 && value == 0,
               "autoexecute defaults to silicon-proven legacy kick");
        EXPECT(virge_parse_autoexec("", &value) == 0 && value == 0,
               "empty autoexecute value keeps legacy kick");
        EXPECT(virge_parse_autoexec("1", &value) == 0 && value == 1,
               "autoexecute one enables optimization");
        EXPECT(virge_parse_autoexec("0", &value) == 0 && value == 0,
               "autoexecute zero selects legacy kick");
        EXPECT(virge_parse_autoexec("yes", &value) == -EINVAL,
               "invalid autoexecute value is rejected");
    }

    EXPECT((enabled & VIRGE_CMD_AUTOEXEC) != 0 &&
           (enabled & ~VIRGE_CMD_AUTOEXEC) == command,
           "autoexecute command sets only AE bit");
    EXPECT(virge_autoexec_disable_command() ==
           (VIRGE_CMD_3D | VIRGE_3D_NOP) &&
           !(virge_autoexec_disable_command() & VIRGE_CMD_AUTOEXEC),
           "autoexecute disable is AE-clear 3D NOP");
    EXPECT(VIRGE_3D_TY01_Y12 == 0xB57Cu &&
           VIRGE_3D_TY01_Y12 > VIRGE_3D_TYS,
           "B57C is highest triangle register and autoexecute kick");

    EXPECT(virge_state_dirty_mask(&command_cache, desired, 1) == 1u,
           "first autoexecute command is dirty");
    virge_state_commit(&command_cache, 0, enabled);
    EXPECT(virge_state_dirty_mask(&command_cache, desired, 1) == 0,
           "unchanged autoexecute command is reused");
    desired[0] ^= VIRGE_ZUP_ENABLE;
    EXPECT(virge_state_dirty_mask(&command_cache, desired, 1) == 1u,
           "3D command-state change is detected");
    virge_state_commit(&command_cache, 0, desired[0]);
    virge_state_invalidate(&command_cache, 1u);
    EXPECT(virge_state_dirty_mask(&command_cache, desired, 1) == 1u,
           "2D kick invalidates cached autoexecute command");
}

static void test_triangle_reuse_config(void)
{
    int value = -1;

    EXPECT(virge_parse_tri_reuse(NULL, &value) == 0 && value == 0,
           "triangle reuse defaults off pending hardware validation");
    EXPECT(virge_parse_tri_reuse("", &value) == 0 && value == 0,
           "empty triangle reuse value keeps full parameter writes");
    EXPECT(virge_parse_tri_reuse("1", &value) == 0 && value == 1,
           "triangle reuse one enables the experiment");
    EXPECT(virge_parse_tri_reuse("0", &value) == 0 && value == 0,
           "triangle reuse zero selects the exact control path");
    EXPECT(virge_parse_tri_reuse("yes", &value) == -EINVAL,
           "invalid triangle reuse value is rejected");
    EXPECT(virge_parse_tri_reuse("0", NULL) == -EINVAL,
           "null triangle reuse destination is rejected");
}

int main(void)
{
    test_fixed_modes();
    test_mode_validation();
    test_pll();
    test_crtc_image();
    test_first_gate_image();
    test_fifo_status_decode();
    test_state_cache();
    test_presentation_config();
    test_autoexecute();
    test_triangle_reuse_config();
    if (failed)
        return 1;
    printf("test-virge-mode: PASS (modes, CRTC, FIFO/state cache, presentation, triangle gates)\n");
    return 0;
}
