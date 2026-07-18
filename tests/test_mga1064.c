/* Hardware-independent MGA-1064 buffer-layout and CRTC encoding tests. */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>

#include "backends/mga1064/mga1064.h"

static int expect_layout(uint32_t front, uint32_t stride, uint32_t height,
                         uint32_t bpp, uint32_t vram, int expected_ret,
                         uint32_t expected_back, uint32_t expected_z,
                         const char *label)
{
    struct mga1064_buffer_layout layout;
    int ret = mga1064_plan_double_buffer(front, stride, height, bpp, vram,
                                         &layout);

    if (ret != expected_ret) {
        fprintf(stderr, "test-mga1064: %s returned %d, expected %d\n",
                label, ret, expected_ret);
        return -1;
    }
    if (ret)
        return 0;
    if (layout.front_bytes != front || layout.back_bytes != expected_back ||
        layout.z_bytes != expected_z ||
        layout.surface_bytes != stride * height) {
        fprintf(stderr,
                "test-mga1064: %s layout front=%u back=%u Z=%u size=%u\n",
                label, layout.front_bytes, layout.back_bytes, layout.z_bytes,
                layout.surface_bytes);
        return -1;
    }
    return 0;
}

static int expect_start(uint32_t byte_offset, int expected_ret,
                        uint8_t expected_high, uint8_t expected_low,
                        uint8_t expected_extended, const char *label)
{
    uint8_t high = 0, low = 0, extended = 0;
    int ret = mga1064_encode_start_address(byte_offset, &high, &low,
                                           &extended);

    if (ret != expected_ret ||
        (!ret && (high != expected_high || low != expected_low ||
                  extended != expected_extended))) {
        fprintf(stderr,
                "test-mga1064: %s returned %d and %02x:%02x:%x, "
                "expected %d and %02x:%02x:%x\n",
                label, ret, high, low, extended, expected_ret,
                expected_high, expected_low, expected_extended);
        return -1;
    }
    return 0;
}

int main(void)
{
    const uint32_t surface = 1600u * 600u;
    int failed = 0;

    failed |= expect_layout(0, 1600, 600, 2, 4u * 1024u * 1024u, 0,
                            surface, surface * 2u, "4MB front at zero");
    failed |= expect_layout(surface, 1600, 600, 2,
                            4u * 1024u * 1024u, 0,
                            0, surface * 2u, "nonzero live front");
    failed |= expect_layout(0, 3200, 600, 4, 4u * 1024u * 1024u,
                            -ENOSPC, 0, 0, "4MB 32bpp capacity");
    failed |= expect_layout(4, 1600, 600, 2, 4u * 1024u * 1024u,
                            -EINVAL, 0, 0, "unaligned scanout");

    failed |= expect_start(0, 0, 0x00, 0x00, 0x0,
                           "zero start address");
    failed |= expect_start(surface, 0, 0xd4, 0xc0, 0x1,
                           "800x600x16 second page");
    failed |= expect_start(0x7ffff8u, 0, 0xff, 0xff, 0xf,
                           "largest 20-bit start address");
    failed |= expect_start(3, -EINVAL, 0, 0, 0, "unaligned start address");
    failed |= expect_start(0x800000u, -ERANGE, 0, 0, 0,
                           "start address overflow");

    if (failed) {
        fprintf(stderr, "test-mga1064: FAILED\n");
        return 1;
    }
    printf("test-mga1064: PASS (VRAM layout, live-front exclusion, "
           "capacity, CRTC start encoding)\n");
    return 0;
}
