// SPDX-License-Identifier: GPL-2.0-only
#include "compas_fb_clock.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/math64.h>
#else
#include <errno.h>
#endif

int compas_fb_clock_clear_invert(compas_fb_clock_u32 before,
				compas_fb_clock_u32 *after)
{
	if (!after)
		return -EINVAL;
	if (before & COMPAS_FB_CLOCK_WRITE_BUSY_MASK)
		return -EBUSY;
	*after = before & ~COMPAS_FB_CLOCK_INVERT_BIT;
	return 0;
}

int compas_fb_clock_rate_valid(compas_fb_clock_u32 rate,
			       compas_fb_clock_u32 nominal)
{
	compas_fb_clock_u32 tolerance;

	if (!rate || !nominal)
		return 0;
	tolerance = nominal / 10U;
	if (rate >= nominal)
		return rate - nominal <= tolerance;
	return nominal - rate <= tolerance;
}

int compas_fb_clock_pixclock_ps(compas_fb_clock_u32 rate,
				compas_fb_clock_u32 *pixclock_ps)
{
	compas_fb_clock_u64 rounded;

	if (!rate || !pixclock_ps)
		return -EINVAL;
	rounded = 1000000000000ULL + rate / 2U;
#ifdef __KERNEL__
	rounded = div_u64(rounded, rate);
#else
	rounded /= rate;
#endif
	if (!rounded || rounded > ~(compas_fb_clock_u32)0)
		return -ERANGE;
	*pixclock_ps = (compas_fb_clock_u32)rounded;
	return 0;
}
