/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * R1 panel description shared with the retained soc_fb.ko ABI.
 *
 * The first 152 bytes are the vendor panel configuration. Keep their order
 * and types unchanged. The trailing callback slots are part of the binary
 * ABI even when this panel leaves them NULL.
 */
#ifndef COMPAS_LCDC_ABI_H
#define COMPAS_LCDC_ABI_H

#include <linux/bug.h>
#include <linux/stddef.h>

struct lcdc_data {
	const char *name;
	unsigned int refresh;
	unsigned int xres;
	unsigned int yres;
	unsigned int pixclock;
	unsigned int left_margin;
	unsigned int right_margin;
	unsigned int upper_margin;
	unsigned int lower_margin;
	unsigned int hsync_len;
	unsigned int vsync_len;
	unsigned int fb_fmt;		/* 1: RGB565 in framebuffer memory. */
	unsigned int lcd_mode;
	unsigned int out_format;
	unsigned int color_even;
	unsigned int color_odd;
	unsigned int pix_clk_active;
	unsigned int de_active_level;
	unsigned int hsync_active_level;
	unsigned int vsync_active_level;
	unsigned int smart_lcd[18];
	int (*power_on)(void *unused);
	int (*power_off)(void *unused);
	int (*optional_ioctl_callback)(unsigned short value);
	int (*optional_disable_callback)(void);
};

/* Call from a function body compiled for the target kernel. */
static inline void compas_lcdc_abi_assert(void)
{
	BUILD_BUG_ON(sizeof(struct lcdc_data) != 168);
	BUILD_BUG_ON(offsetof(struct lcdc_data, power_on) != 152);
	BUILD_BUG_ON(offsetof(struct lcdc_data, power_off) != 156);
	BUILD_BUG_ON(offsetof(struct lcdc_data, optional_ioctl_callback) != 160);
	BUILD_BUG_ON(offsetof(struct lcdc_data, optional_disable_callback) != 164);
}

#endif /* COMPAS_LCDC_ABI_H */
