// SPDX-License-Identifier: GPL-2.0-only
#include "compas_fb_fops.h"

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/types.h>

int compas_fb_check_var(struct fb_var_screeninfo *var,
			unsigned int expected_pixclock_ps)
{
	if (!var)
		return -EINVAL;

	if (var->xres != COMPAS_FB_WIDTH || var->yres != COMPAS_FB_HEIGHT ||
	    var->xres_virtual != COMPAS_FB_WIDTH ||
	    var->yres_virtual != COMPAS_FB_VIRTUAL_HEIGHT ||
	    var->xoffset != 0 ||
	    (var->yoffset != 0 && var->yoffset != COMPAS_FB_HEIGHT) ||
	    var->bits_per_pixel != 16 || var->grayscale || var->nonstd ||
	    var->red.offset != 11 || var->red.length != 5 || var->red.msb_right ||
	    var->green.offset != 5 || var->green.length != 6 ||
	    var->green.msb_right || var->blue.offset != 0 ||
	    var->blue.length != 5 || var->blue.msb_right ||
	    var->transp.offset != 0 || var->transp.length != 0 ||
	    var->transp.msb_right || var->vmode != FB_VMODE_NONINTERLACED ||
	    !expected_pixclock_ps || var->pixclock != expected_pixclock_ps ||
	    var->left_margin != 24 ||
	    var->right_margin != 24 || var->upper_margin != 8 ||
	    var->lower_margin != 14 || var->hsync_len != 24 ||
	    var->vsync_len != 5 ||
	    var->sync != (FB_SYNC_HOR_HIGH_ACT | FB_SYNC_VERT_HIGH_ACT))
		return -EINVAL;

	return 0;
}

int compas_fb_setcolreg(unsigned int regno, unsigned int red,
			unsigned int green, unsigned int blue,
			unsigned int transp, struct fb_info *info)
{
	u32 *palette;

	(void)transp;
	if (!info || !info->pseudo_palette || regno >= 16)
		return -EINVAL;

	palette = info->pseudo_palette;
	palette[regno] = ((red & 0xf800U) | ((green & 0xfc00U) >> 5) |
			  ((blue & 0xf800U) >> 11));
	return 0;
}

int compas_fb_mmap_bounds(const struct vm_area_struct *vma,
			  unsigned long allocation_bytes)
{
	unsigned long length;
	unsigned long offset;

	if (!vma || vma->vm_end < vma->vm_start)
		return -EINVAL;
	length = vma->vm_end - vma->vm_start;
	if (!length || vma->vm_pgoff > (ULONG_MAX >> PAGE_SHIFT))
		return -EINVAL;
	offset = vma->vm_pgoff << PAGE_SHIFT;
	if (offset > allocation_bytes || length > allocation_bytes - offset)
		return -EINVAL;
	return 0;
}
