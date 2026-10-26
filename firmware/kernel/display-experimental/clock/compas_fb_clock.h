/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef COMPAS_FB_CLOCK_H
#define COMPAS_FB_CLOCK_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 compas_fb_clock_u32;
typedef u64 compas_fb_clock_u64;
#else
#include <stdint.h>
typedef uint32_t compas_fb_clock_u32;
typedef uint64_t compas_fb_clock_u64;
#endif

#define COMPAS_FB_CLOCK_NOMINAL_HZ 28303248ULL
#define COMPAS_FB_CLOCK_INVERT_BIT (1U << 26)
#define COMPAS_FB_CLOCK_BUSY_BIT   (1U << 28)
#define COMPAS_FB_CLOCK_CE_BIT     (1U << 29)
#define COMPAS_FB_CLOCK_WRITE_BUSY_MASK \
	(COMPAS_FB_CLOCK_BUSY_BIT | COMPAS_FB_CLOCK_CE_BIT)

/* Pure helpers shared by the driver and host tests. */
int compas_fb_clock_clear_invert(compas_fb_clock_u32 before,
				compas_fb_clock_u32 *after);
int compas_fb_clock_rate_valid(compas_fb_clock_u32 rate,
			       compas_fb_clock_u32 nominal);
int compas_fb_clock_pixclock_ps(compas_fb_clock_u32 rate,
				compas_fb_clock_u32 *pixclock_ps);

#endif
