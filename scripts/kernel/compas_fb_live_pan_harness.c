/* SPDX-License-Identifier: GPL-2.0-only */
/* Fake-MMIO exercise for the experimental continuous-pan fence. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/kernel/display-experimental/compas_fb_hw.h"

#define DESC 0x01234000U
#define PAGE 0x01250000U

struct fake {
	uint32_t chain, site, status, tft, flags;
	uint32_t writes[8];
	unsigned int write_count, reads;
	uint32_t reassert_events;
	unsigned int reassert_count;
};

static uint32_t fake_read(void *context, uint32_t offset)
{
	struct fake *f = context;
	++f->reads;
	switch (offset) {
	case COMPAS_FB_HW_REG_RDMA_CHAIN_SITE: return f->chain;
	case COMPAS_FB_HW_REG_RDMA_SITE: return f->site;
	case COMPAS_FB_HW_REG_STATUS: return f->status;
	case COMPAS_FB_HW_REG_TFT_STATUS: return f->tft;
	case COMPAS_FB_HW_REG_INTERRUPT_FLAG: return f->flags;
	default: assert(!"unexpected MMIO read"); return 0;
	}
}

static void fake_write(void *context, uint32_t offset, uint32_t value)
{
	struct fake *f = context;
	assert(offset == COMPAS_FB_HW_REG_CLEAR_STATUS);
	assert(f->write_count < sizeof(f->writes) / sizeof(f->writes[0]));
	f->writes[f->write_count++] = value;
	/* Model W1C completion bits while retaining unrelated error evidence. */
	f->status &= ~value;
	f->flags &= ~value;
	if (f->reassert_count) {
		f->flags |= f->reassert_events;
		--f->reassert_count;
	}
}

static void setup(struct fake *f)
{
	memset(f, 0, sizeof(*f));
	f->chain = DESC;
	f->site = PAGE + 64U;
	f->status = COMPAS_FB_HW_SRD_WORKING;
	f->tft = COMPAS_FB_HW_TFT_WORKING;
}

static struct compas_fb_hw_ops ops_for(struct fake *f)
{
	return (struct compas_fb_hw_ops) { f, fake_read, fake_write, NULL };
}

static void test_prepare_clears_only_fence_events_and_preserves_faults(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence = { 99U, 99U };
	setup(&f);
	/* Sticky underrun evidence remains visible when completion bits drain. */
	f.status |= COMPAS_FB_HW_UNDERRUN;
	ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == -EIO);
	assert(f.write_count == 1U);
	assert(f.writes[0] == (COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END));
	assert(fence.events == 0U && fence.adopted == 0U);
	assert(f.status & COMPAS_FB_HW_UNDERRUN);
}

static void test_prepare_rejects_stale_and_inactive_state(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f); f.flags = COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END;
	f.status |= COMPAS_FB_HW_SRD_END;
	ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(f.flags == 0U && !(f.status & (COMPAS_FB_HW_SRD_END |
		COMPAS_FB_HW_FRAME_END)));
	setup(&f); f.tft = 0U; ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == -EIO);
	setup(&f); f.status |= COMPAS_FB_HW_DIRECT_WORKING; ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == -EOPNOTSUPP);
}

static void test_prepare_reassertion_retries_are_bounded(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f);
	f.flags = COMPAS_FB_HW_FRAME_END;
	f.reassert_events = COMPAS_FB_HW_FRAME_END;
	f.reassert_count = 1U;
	ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(f.write_count == 2U && f.flags == 0U);

	setup(&f);
	f.flags = COMPAS_FB_HW_SRD_END;
	f.reassert_events = COMPAS_FB_HW_SRD_END;
	f.reassert_count = 3U;
	ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == -ETIMEDOUT);
	assert(f.write_count == 3U && f.flags == COMPAS_FB_HW_SRD_END);
}

static void test_events_before_and_after_target_adoption(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	/* Old-chain evidence is retained, then target adoption completes the fence. */
	f.chain = DESC + COMPAS_FB_HW_DESC_SLOT_BYTES;
	f.site = PAGE - 64U;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES,
		COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END, &fence) == -EAGAIN);
	assert(fence.adopted == 0U);
	f.chain = DESC;
	f.site = PAGE + 64U;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == 0);
	assert(fence.adopted == 1U);
	assert(fence.events == (COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END));

	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EAGAIN);
	assert(fence.adopted == 1U);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES,
		COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END, &fence) == 0);

	/* Reverse arrival order is also retained across target sampling. */
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_FRAME_END, &fence) == -EAGAIN);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_SRD_END, &fence) == 0);
}

static void test_missing_dma_or_display_end_stays_pending(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_SRD_END, &fence) == -EAGAIN);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_FRAME_END, &fence) == 0);
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_FRAME_END, &fence) == -EAGAIN);
	/* Once target was adopted, chain reversal invalidates the pending fence. */
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EAGAIN);
	f.chain = DESC + COMPAS_FB_HW_DESC_SLOT_BYTES;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_SRD_END, &fence) == -EIO);
}

static void test_bad_runtime_state_and_target_fail_closed(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	f.status |= COMPAS_FB_HW_UNDERRUN;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EIO);
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	f.tft = 0U;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EIO);
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	f.status |= COMPAS_FB_HW_DIRECT_WORKING;
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EOPNOTSUPP);
	setup(&f); f.chain = DESC + COMPAS_FB_HW_DESC_SLOT_BYTES; ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(&ops, &fence) == 0);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES,
		COMPAS_FB_HW_SRD_END | COMPAS_FB_HW_FRAME_END, &fence) == -EAGAIN);
}

static void test_invalid_inputs_do_not_touch_mmio(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_live_fence fence;
	setup(&f); ops = ops_for(&f);
	assert(compas_fb_hw_prepare_live_fence(NULL, &fence) == -EINVAL);
	assert(compas_fb_hw_prepare_live_fence(&ops, NULL) == -EINVAL);
	assert(compas_fb_hw_sample_live_fence(NULL, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EINVAL);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC + 4U, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U, &fence) == -EINVAL);
	assert(compas_fb_hw_sample_live_fence(&ops, DESC, PAGE,
		COMPAS_FB_HW_R1_PAGE_BYTES - 1U, 0U, &fence) == -EINVAL);
	assert(f.reads == 0U && f.write_count == 0U);
}

int main(void)
{
	test_prepare_clears_only_fence_events_and_preserves_faults();
	test_prepare_rejects_stale_and_inactive_state();
	test_prepare_reassertion_retries_are_bounded();
	test_events_before_and_after_target_adoption();
	test_missing_dma_or_display_end_stays_pending();
	test_bad_runtime_state_and_target_fail_closed();
	test_invalid_inputs_do_not_touch_mmio();
	puts("compas_fb_live_pan: PASS");
	return 0;
}
