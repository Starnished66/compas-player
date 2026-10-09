/* SPDX-License-Identifier: GPL-2.0-only */
/* Fixed R1 DPU scanout primitives, kept separate from Linux resource ownership. */
#include "compas_fb_hw.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/bug.h>
#else
#include <errno.h>
#include <stddef.h>
#endif

#define R1_STRIDE_PIXELS 480U
#define R1_IRQ_MASK (COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN | \
		     COMPAS_FB_HW_STOP_ACK)
#define R1_ACTIVITY_MASK ((1U << 0) | (1U << 3) | (1U << 4) | (1U << 5))
#define R1_OTHER_CHANNEL_ACTIVITY ((1U << 4) | (1U << 5))
#define R1_TFT_CFG 0x00120000U
#define R1_DISP_COM_MASK 0x003f0013U
#define R1_DISP_COM_VALUE 0x00000061U

#ifdef __KERNEL__
typedef char compas_fb_hw_desc_must_be_20_bytes[
	(sizeof(struct compas_fb_hw_desc_words) == COMPAS_FB_HW_DESC_BYTES) ? 1 : -1];
#else
_Static_assert(sizeof(struct compas_fb_hw_desc_words) ==
	       COMPAS_FB_HW_DESC_BYTES, "DPU descriptor must be 20 bytes");
#endif

static int ops_valid(const struct compas_fb_hw_ops *ops)
{
	return ops && ops->read && ops->write;
}

int compas_fb_hw_encode_r1_rgb565_descriptor(
	struct compas_fb_hw_desc_words *desc,
	compas_fb_hw_u32 desc_phys,
	compas_fb_hw_u32 framebuffer_phys,
	compas_fb_hw_u32 stride_pixels)
{
#ifdef __KERNEL__
	BUILD_BUG_ON(sizeof(struct compas_fb_hw_desc_words) !=
		     COMPAS_FB_HW_DESC_BYTES);
#endif
	if (!desc || stride_pixels != R1_STRIDE_PIXELS ||
	    (desc_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)) ||
	    desc_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    framebuffer_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    (framebuffer_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)))
		return -EINVAL;

	/* R1 panel descriptor: a single self-looping RGB565 scanout descriptor. */
	desc->word[0] = desc_phys;
	desc->word[1] = framebuffer_phys;
	desc->word[2] = stride_pixels;
	desc->word[3] = COMPAS_FB_HW_CHAIN_RGB565;
	desc->word[4] = COMPAS_FB_HW_CHAIN_EOD_MASK;
	return 0;
}

int compas_fb_hw_program_r1_tft(const struct compas_fb_hw_ops *ops)
{
	compas_fb_hw_u32 common;
	compas_fb_hw_u32 disp;
	if (!ops_valid(ops))
		return -EINVAL;

	/* Match the frozen R1 480x800@62 timing register values. */
	if (compas_fb_hw_clear_events(ops, R1_IRQ_MASK))
		return -EIO;
	ops->write(ops->context, COMPAS_FB_HW_REG_INTERRUPT_MASK, R1_IRQ_MASK);

	common = ops->read(ops->context, COMPAS_FB_HW_REG_COMMON_CONFIG);
	ops->write(ops->context, COMPAS_FB_HW_REG_COMMON_CONFIG, common | 0x30U);
	disp = ops->read(ops->context, COMPAS_FB_HW_REG_DISPLAY_COMMON);
	disp = (disp & ~R1_DISP_COM_MASK) | R1_DISP_COM_VALUE;
	ops->write(ops->context, COMPAS_FB_HW_REG_DISPLAY_COMMON, disp);
	ops->write(ops->context, COMPAS_FB_HW_REG_TFT_HSYNC,
		   (24U << 16) | 552U);
	ops->write(ops->context, COMPAS_FB_HW_REG_TFT_VSYNC,
		   (5U << 16) | 827U);
	ops->write(ops->context, COMPAS_FB_HW_REG_TFT_HDE,
		   (48U << 16) | 528U);
	ops->write(ops->context, COMPAS_FB_HW_REG_TFT_VDE,
		   (13U << 16) | 813U);
	ops->write(ops->context, COMPAS_FB_HW_REG_TFT_CONFIG, R1_TFT_CFG);
	return 0;
}

int compas_fb_hw_clear_events(const struct compas_fb_hw_ops *ops,
			      compas_fb_hw_u32 events)
{
	compas_fb_hw_u32 pending;
	if (!ops_valid(ops) || (events & ~R1_IRQ_MASK))
		return -EINVAL;
	pending = ops->read(ops->context, COMPAS_FB_HW_REG_INTERRUPT_FLAG) & events;
	if (pending) {
		ops->write(ops->context, COMPAS_FB_HW_REG_CLEAR_STATUS, pending);
		if (ops->read(ops->context, COMPAS_FB_HW_REG_INTERRUPT_FLAG) & events)
			return -EIO;
	}
	return 0;
}

int compas_fb_hw_stop_rdma(const struct compas_fb_hw_ops *ops,
			   compas_fb_hw_u32 poll_limit)
{
	compas_fb_hw_u32 i;
	compas_fb_hw_u32 status;
	if (!ops_valid(ops) || !ops->delay_us || !poll_limit ||
	    poll_limit > COMPAS_FB_HW_STOP_POLLS)
		return -EINVAL;

	status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
	if (status & R1_OTHER_CHANNEL_ACTIVITY)
		return -EOPNOTSUPP;
	if (!(status & R1_ACTIVITY_MASK)) {
		/* Treat hardware as idle only after pending IRQs drain and recheck. */
		if (compas_fb_hw_clear_events(ops, R1_IRQ_MASK))
			return -EIO;
		status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
		if (status & R1_OTHER_CHANNEL_ACTIVITY)
			return -EOPNOTSUPP;
		if (!(status & R1_ACTIVITY_MASK))
			return 0;
	}
	/* EOD can recur continuously while scanout is active. Drain only the
	 * stop ACK here; frame-end/underrun flags are drained after quiescence. */
	if (compas_fb_hw_clear_events(ops, COMPAS_FB_HW_STOP_ACK))
		return -EIO;
	status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
	if (status & R1_OTHER_CHANNEL_ACTIVITY)
		return -EOPNOTSUPP;
	if (!(status & R1_ACTIVITY_MASK)) {
		if (compas_fb_hw_clear_events(ops, R1_IRQ_MASK))
			return -EIO;
		status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
		if (status & R1_OTHER_CHANNEL_ACTIVITY)
			return -EOPNOTSUPP;
		if (!(status & R1_ACTIVITY_MASK))
			return 0;
	}

	ops->write(ops->context, COMPAS_FB_HW_REG_CTRL,
		   COMPAS_FB_HW_GENERAL_STOP_RDMA);
	for (i = 0; i < poll_limit; ++i) {
		compas_fb_hw_u32 flags = ops->read(ops->context,
						    COMPAS_FB_HW_REG_INTERRUPT_FLAG);
		status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
		if (status & R1_OTHER_CHANNEL_ACTIVITY)
			return -EOPNOTSUPP;
		if ((flags & COMPAS_FB_HW_STOP_ACK) &&
		    !(status & R1_ACTIVITY_MASK)) {
			if (compas_fb_hw_clear_events(ops, R1_IRQ_MASK))
				return -EIO;
			status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
			if (status & R1_OTHER_CHANNEL_ACTIVITY)
				return -EOPNOTSUPP;
			if (status & R1_ACTIVITY_MASK)
				return -EIO;
			return 0;
		}
		ops->delay_us(ops->context, 1000U);
	}
	/* Timeout leaves all scanout resources and descriptor memory untouched. */
	return -ETIMEDOUT;
}

int compas_fb_hw_start_rdma(const struct compas_fb_hw_ops *ops,
			    compas_fb_hw_u32 desc_phys)
{
	if (!ops_valid(ops) || desc_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    (desc_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)))
		return -EINVAL;
	ops->write(ops->context, COMPAS_FB_HW_REG_RDMA_CHAIN_ADDR, desc_phys);
	ops->write(ops->context, COMPAS_FB_HW_REG_RDMA_CHAIN_CTRL,
		   COMPAS_FB_HW_RDMA_START);
	return 0;
}

static int adoption_snapshot_matches(const struct compas_fb_hw_ops *ops,
				     compas_fb_hw_u32 expected_desc_phys,
				     compas_fb_hw_u32 target_page_phys,
				     compas_fb_hw_u32 page_end)
{
	compas_fb_hw_u32 chain_before = ops->read(ops->context,
							 COMPAS_FB_HW_REG_RDMA_CHAIN_SITE);
	compas_fb_hw_u32 site = ops->read(ops->context,
							 COMPAS_FB_HW_REG_RDMA_SITE);
	compas_fb_hw_u32 status = ops->read(ops->context,
							   COMPAS_FB_HW_REG_STATUS);
	compas_fb_hw_u32 chain_after = ops->read(ops->context,
							COMPAS_FB_HW_REG_RDMA_CHAIN_SITE);

	if (status & (COMPAS_FB_HW_DIRECT_WORKING |
		      COMPAS_FB_HW_WRBK_WORKING))
		return -EOPNOTSUPP;
	if (chain_before == expected_desc_phys &&
	    chain_after == expected_desc_phys &&
	    (site & 1U) == 0 && site > target_page_phys &&
	    site <= page_end - 2U &&
	    (status & COMPAS_FB_HW_SRD_WORKING))
		return 1;
	return 0;
}

int compas_fb_hw_wait_rdma_adopted(const struct compas_fb_hw_ops *ops,
				   compas_fb_hw_u32 expected_desc_phys,
				   compas_fb_hw_u32 target_page_phys,
				   compas_fb_hw_u32 target_page_bytes,
				   compas_fb_hw_u32 poll_limit)
{
	compas_fb_hw_u32 i;
	compas_fb_hw_u32 page_end;
	int adopted;

	if (!ops_valid(ops) || !ops->delay_us || !poll_limit ||
	    poll_limit > COMPAS_FB_HW_ADOPT_POLLS ||
	    target_page_bytes != COMPAS_FB_HW_R1_PAGE_BYTES ||
	    (expected_desc_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)) ||
	    expected_desc_phys > COMPAS_FB_HW_PHYS_LIMIT -
				 COMPAS_FB_HW_DESC_SLOT_BYTES ||
	    (target_page_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)) ||
	    target_page_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    target_page_bytes > COMPAS_FB_HW_PHYS_LIMIT - target_page_phys)
		return -EINVAL;

	page_end = target_page_phys + target_page_bytes;
	/* First sample immediately, then retry four times at 25us intervals. The
	 * adoption fence is identical in both phases; only the delay changes. */
	for (i = 0; i <= COMPAS_FB_HW_ADOPT_FAST_RETRIES; ++i) {
		adopted = adoption_snapshot_matches(ops, expected_desc_phys,
						    target_page_phys, page_end);
		if (adopted)
			return adopted > 0 ? 0 : adopted;
		if (i < COMPAS_FB_HW_ADOPT_FAST_RETRIES)
			ops->delay_us(ops->context,
				      COMPAS_FB_HW_ADOPT_FAST_DELAY_US);
	}

	/* Keep poll_limit as the slow-phase poll bound and retain its original
	 * 1ms delay after every failed sample, including the final timeout sample. */
	for (i = 0; i < poll_limit; ++i) {
		adopted = adoption_snapshot_matches(ops, expected_desc_phys,
						    target_page_phys, page_end);
		if (adopted)
			return adopted > 0 ? 0 : adopted;
		ops->delay_us(ops->context, COMPAS_FB_HW_ADOPT_DELAY_US);
	}
	return -ETIMEDOUT;
}

/* Only completion bits are acknowledged here. Fault evidence must survive. */
int compas_fb_hw_prepare_live_fence(const struct compas_fb_hw_ops *ops,
				  struct compas_fb_hw_live_fence *fence)
{
	compas_fb_hw_u32 status, events, tft;
	unsigned int retry;
	if (!ops_valid(ops) || !fence)
		return -EINVAL;
	fence->events = 0;
	fence->adopted = 0;
	/* A boundary may reassert completion during acknowledgement. Retry the
	 * sampled-low handshake without waiting inside the caller's IRQ section. */
	for (retry = 0; retry < 3U; ++retry) {
		ops->write(ops->context, COMPAS_FB_HW_REG_CLEAR_STATUS,
			   COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END);
		status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
		events = status | ops->read(ops->context,
					  COMPAS_FB_HW_REG_INTERRUPT_FLAG);
		tft = ops->read(ops->context, COMPAS_FB_HW_REG_TFT_STATUS);
		if (status & R1_OTHER_CHANNEL_ACTIVITY)
			return -EOPNOTSUPP;
		if ((events & (COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_STOP_ACK)) ||
		    (tft & COMPAS_FB_HW_TFT_UNDERRUN) ||
		    !(status & COMPAS_FB_HW_SRD_WORKING) ||
		    !(tft & COMPAS_FB_HW_TFT_WORKING))
			return -EIO;
		if (!(events & (COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END)))
			return 0;
	}
	return -ETIMEDOUT;
}

int compas_fb_hw_sample_live_fence(const struct compas_fb_hw_ops *ops,
				 compas_fb_hw_u32 expected_desc_phys,
				 compas_fb_hw_u32 target_page_phys,
				 compas_fb_hw_u32 target_page_bytes,
				 compas_fb_hw_u32 latched_events,
				 struct compas_fb_hw_live_fence *fence)
{
	compas_fb_hw_u32 chain_before, chain_after, site, status, tft, events;
	compas_fb_hw_u32 page_end;
	int target;

	if (!ops_valid(ops) || !fence ||
	    target_page_bytes != COMPAS_FB_HW_R1_PAGE_BYTES ||
	    (expected_desc_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)) ||
	    expected_desc_phys > COMPAS_FB_HW_PHYS_LIMIT -
				 COMPAS_FB_HW_DESC_SLOT_BYTES ||
	    (target_page_phys & (COMPAS_FB_HW_DESC_ALIGN - 1U)) ||
	    target_page_phys >= COMPAS_FB_HW_PHYS_LIMIT ||
	    target_page_bytes > COMPAS_FB_HW_PHYS_LIMIT - target_page_phys)
		return -EINVAL;
	page_end = target_page_phys + target_page_bytes;
	chain_before = ops->read(ops->context, COMPAS_FB_HW_REG_RDMA_CHAIN_SITE);
	site = ops->read(ops->context, COMPAS_FB_HW_REG_RDMA_SITE);
	status = ops->read(ops->context, COMPAS_FB_HW_REG_STATUS);
	tft = ops->read(ops->context, COMPAS_FB_HW_REG_TFT_STATUS);
	events = status | ops->read(ops->context,
				  COMPAS_FB_HW_REG_INTERRUPT_FLAG) | latched_events;
	chain_after = ops->read(ops->context, COMPAS_FB_HW_REG_RDMA_CHAIN_SITE);
	fence->events |= events & (COMPAS_FB_HW_SRD_END |
		COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN |
		COMPAS_FB_HW_STOP_ACK);
	if (status & R1_OTHER_CHANNEL_ACTIVITY)
		return -EOPNOTSUPP;
	if ((fence->events & (COMPAS_FB_HW_UNDERRUN | COMPAS_FB_HW_STOP_ACK)) ||
	    (tft & COMPAS_FB_HW_TFT_UNDERRUN) ||
	    !(status & COMPAS_FB_HW_SRD_WORKING) ||
	    !(tft & COMPAS_FB_HW_TFT_WORKING))
		return -EIO;
	if (fence->adopted && (chain_before != expected_desc_phys ||
			      chain_after != expected_desc_phys))
		return -EIO;
	target = chain_before == expected_desc_phys &&
		 chain_after == expected_desc_phys && !(site & 1U) &&
		 site > target_page_phys && site <= page_end - 2U;
	if (target)
		fence->adopted = 1;
	/* The retiring frame's DMA/display ends and the new immutable source
	 * ownership are concurrent evidence; never discard a boundary merely
	 * because target adoption was sampled later. */
	if (target && (fence->events & (COMPAS_FB_HW_SRD_END |
		COMPAS_FB_HW_FRAME_END)) ==
		(COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END))
		return 0;
	return -EAGAIN;
}
