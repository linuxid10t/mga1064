#ifndef L10GL_FBDEV_H
#define L10GL_FBDEV_H

#include <linux/fb.h>

#include "l10gl.h"

struct l10gl_fbdev_mode {
    struct fb_fix_screeninfo fix;
    struct fb_var_screeninfo var;
};

/* Read and validate a packed true/direct-color fbdev mode. */
int l10gl_fbdev_read_mode(int fd, const char *name,
                          struct l10gl_fbdev_mode *mode);

/* Adopt the requested geometry/depth, asking the kernel driver to switch when
 * necessary. If required_format is non-NULL, its channel layout must match as
 * well (used for engines whose render-target format is fixed). */
int l10gl_fbdev_negotiate(int fd, const char *name,
                          int width, int height, int bits_per_pixel,
                          const struct l10gl_pixel_format *required_format,
                          struct l10gl_fbdev_mode *mode);

int l10gl_fbdev_mode_matches(const struct l10gl_fbdev_mode *mode,
                             int width, int height, int bits_per_pixel,
                             const struct l10gl_pixel_format *required_format);

/* Find a second non-overlapping, fully mapped vertical page for fbdev
 * panning. Returns one when both offsets are usable, zero otherwise. */
int l10gl_fbdev_find_pan_pages(const struct l10gl_fbdev_mode *mode,
                               uint32_t *front_yoffset,
                               uint32_t *back_yoffset);

/* Publish authoritative mode facts in the frontend context. */
void l10gl_mode_from_fbdev(struct l10gl_ctx *ctx,
                           const struct l10gl_fbdev_mode *mode);
void l10gl_mode_set_linear(struct l10gl_ctx *ctx, int width, int height,
                           int bits_per_pixel, uint32_t stride,
                           const struct l10gl_pixel_format *format);

/* Canonical packed layouts used for offscreen buffers and fixed-format
 * hardware render targets. Returns zero for unsupported bit depths. */
int l10gl_pixel_format_standard(int bits_per_pixel,
                                struct l10gl_pixel_format *format);

#endif
