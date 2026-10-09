/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef COMPAS_FB_FOPS_H
#define COMPAS_FB_FOPS_H

#include <linux/fb.h>
#include <linux/mm.h>

#define COMPAS_FB_WIDTH 480U
#define COMPAS_FB_HEIGHT 800U
#define COMPAS_FB_PAGES 2U
#define COMPAS_FB_VIRTUAL_HEIGHT (COMPAS_FB_HEIGHT * COMPAS_FB_PAGES)
#define COMPAS_FB_BYTES_PER_PIXEL 2U
#define COMPAS_FB_ALLOCATION_BYTES \
	((unsigned long)COMPAS_FB_WIDTH * COMPAS_FB_HEIGHT * \
	 COMPAS_FB_BYTES_PER_PIXEL * COMPAS_FB_PAGES)

/* Reject unsupported geometry/format; never rewrite the caller's mode. */
int compas_fb_check_var(struct fb_var_screeninfo *var,
			unsigned int expected_pixclock_ps);

/* Store an RGB565 color in info->pseudo_palette[0..15]. */
int compas_fb_setcolreg(unsigned int regno, unsigned int red,
			unsigned int green, unsigned int blue,
			unsigned int transp, struct fb_info *info);

/* Validate a page-offset mmap range against the fixed framebuffer allocation.
 * The caller remains responsible for selecting and applying cache policy and
 * mapping the allocation through its platform-specific mapping callback. */
int compas_fb_mmap_bounds(const struct vm_area_struct *vma,
			  unsigned long allocation_bytes);

#endif /* COMPAS_FB_FOPS_H */
