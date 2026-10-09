#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../../firmware/kernel/display-experimental/compas_fb_hw.h"

struct event { uint32_t offset, value; int write; };
struct read_script {
	uint32_t offset;
	uint32_t values[8];
	unsigned int count;
	unsigned int index;
};
struct fake {
	uint32_t regs[0x10000 / 4];
	struct event events[1024];
	unsigned int count;
	int acknowledge_stop;
	int clear_activity_on_stop;
	int frame_end_on_stop;
	int reassert_eod_while_active;
	int sticky_clear;
	int gated_bus;
	struct read_script scripts[8];
	unsigned int script_count;
};

static void record(struct fake *f, uint32_t off, uint32_t value, int write)
{
	assert(f->count < sizeof(f->events) / sizeof(f->events[0]));
	f->events[f->count++] = (struct event){ off, value, write };
}

static uint32_t fake_read(void *ctx, uint32_t off)
{
	struct fake *f = ctx;
	uint32_t value = f->gated_bus ? 0x80U : f->regs[off / 4];
	unsigned int i;
	if (!f->gated_bus) {
		for (i = 0; i < f->script_count; ++i) {
			struct read_script *script = &f->scripts[i];
			if (script->offset == off && script->index < script->count) {
				value = script->values[script->index++];
				break;
			}
		}
	}
	if (off == COMPAS_FB_HW_REG_INTERRUPT_FLAG &&
	    f->reassert_eod_while_active &&
	    (f->regs[COMPAS_FB_HW_REG_STATUS / 4] & ((1U << 0) | (1U << 3)))) {
		f->regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] |=
			COMPAS_FB_HW_FRAME_END;
		value = f->regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4];
	}
	record(f, off, value, 0);
	return value;
}

static void script_reads(struct fake *f, uint32_t off,
			 const uint32_t *values, unsigned int count)
{
	struct read_script *script;
	unsigned int i;
	assert(count <= 8U && f->script_count < 8U);
	script = &f->scripts[f->script_count++];
	script->offset = off;
	script->count = count;
	for (i = 0; i < count; ++i)
		script->values[i] = values[i];
}

static void set_adopt_state(struct fake *f, uint32_t desc, uint32_t site,
			    uint32_t status)
{
	f->regs[COMPAS_FB_HW_REG_RDMA_CHAIN_SITE / 4] = desc;
	f->regs[COMPAS_FB_HW_REG_RDMA_SITE / 4] = site;
	f->regs[COMPAS_FB_HW_REG_STATUS / 4] = status;
}

static void fake_write(void *ctx, uint32_t off, uint32_t value)
{
	struct fake *f = ctx;
	record(f, off, value, 1);
	if (f->gated_bus)
		return;
	if (off == COMPAS_FB_HW_REG_CLEAR_STATUS) {
		if (!f->sticky_clear) {
			f->regs[COMPAS_FB_HW_REG_STATUS / 4] &= ~value;
			f->regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] &= ~value;
		}
	} else {
		f->regs[off / 4] = value;
		if (off == COMPAS_FB_HW_REG_CTRL && f->acknowledge_stop) {
			f->regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] |=
				COMPAS_FB_HW_STOP_ACK;
			if (f->clear_activity_on_stop)
				f->regs[COMPAS_FB_HW_REG_STATUS / 4] &=
					~((1U << 0) | (1U << 3) | (1U << 4) | (1U << 5));
			if (f->frame_end_on_stop) {
				f->regs[COMPAS_FB_HW_REG_STATUS / 4] |=
					COMPAS_FB_HW_FRAME_END;
				f->regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] |=
					COMPAS_FB_HW_FRAME_END;
			}
		}
	}
}

static void fake_delay(void *ctx, uint32_t usec)
{
	struct fake *f = ctx;
	record(f, 0xffffffffU, usec, 0);
}

static struct event *find_event(struct fake *f, unsigned int start,
				uint32_t off, int write)
{
	unsigned int i;
	for (i = start; i < f->count; ++i)
		if (f->events[i].offset == off && f->events[i].write == write)
			return &f->events[i];
	return NULL;
}

static unsigned int count_delays(const struct fake *f, uint32_t usec,
				 unsigned int *total)
{
	unsigned int i, matches = 0;
	*total = 0;
	for (i = 0; i < f->count; ++i) {
		if (f->events[i].offset != 0xffffffffU || f->events[i].write)
			continue;
		++*total;
		if (f->events[i].value == usec)
			++matches;
	}
	return matches;
}

int main(void)
{
	struct fake f;
	struct compas_fb_hw_ops ops;
	struct compas_fb_hw_desc_words desc;
	struct event *clear, *ctrl;
	memset(&f, 0, sizeof(f));
	ops = (struct compas_fb_hw_ops){ &f, fake_read, fake_write, fake_delay };

	assert(compas_fb_hw_encode_r1_rgb565_descriptor(&desc, 0x01234000U,
		0x01250000U, 480U) == 0);
	assert(desc.word[0] == 0x01234000U);
	assert(desc.word[1] == 0x01250000U);
	assert(desc.word[2] == 480U);
	assert(desc.word[3] == (2U << 19));
	assert(desc.word[4] == (1U << 17));
	assert(compas_fb_hw_encode_r1_rgb565_descriptor(&desc, 0x01234004U,
		0x01250000U, 480U) == -EINVAL);
	assert(compas_fb_hw_encode_r1_rgb565_descriptor(&desc, 0x01234000U,
		0x01250002U, 480U) == -EINVAL);
	assert(compas_fb_hw_encode_r1_rgb565_descriptor(&desc, 0x01234000U,
		0x21250000U, 480U) == -EINVAL);
	assert(compas_fb_hw_encode_r1_rgb565_descriptor(&desc, 0x01234000U,
		0x01250000U, 480U - 2U) == -EINVAL);

	/* Preserve display bits, clear pending events before unmasking, write the
	 * final TFT configuration last, and keep programming idempotent. */
	f.regs[COMPAS_FB_HW_REG_DISPLAY_COMMON / 4] = 0x80U;
	f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] = COMPAS_FB_HW_FRAME_END;
	assert(compas_fb_hw_program_r1_tft(&ops) == 0);
	assert(f.regs[COMPAS_FB_HW_REG_TFT_HSYNC / 4] == ((24U << 16) | 552U));
	assert(f.regs[COMPAS_FB_HW_REG_TFT_VSYNC / 4] == ((5U << 16) | 827U));
	assert(f.regs[COMPAS_FB_HW_REG_TFT_HDE / 4] == ((48U << 16) | 528U));
	assert(f.regs[COMPAS_FB_HW_REG_TFT_VDE / 4] == ((13U << 16) | 813U));
	assert(f.regs[COMPAS_FB_HW_REG_TFT_CONFIG / 4] == 0x00120000U);
	assert(f.regs[COMPAS_FB_HW_REG_COMMON_CONFIG / 4] == 0x30U);
	assert(f.regs[COMPAS_FB_HW_REG_DISPLAY_COMMON / 4] == 0xe1U);
	clear = find_event(&f, 0, COMPAS_FB_HW_REG_CLEAR_STATUS, 1);
	{
		struct event *mask = find_event(&f, 0, COMPAS_FB_HW_REG_INTERRUPT_MASK, 1);
		struct event *config = find_event(&f, 0, COMPAS_FB_HW_REG_TFT_CONFIG, 1);
		unsigned int j;
		assert(clear && mask && config && clear < mask && mask < config);
		for (j = (unsigned int)(config - f.events) + 1U; j < f.count; ++j)
			assert(!f.events[j].write);
	}
	assert(compas_fb_hw_program_r1_tft(&ops) == 0);
	assert(f.regs[COMPAS_FB_HW_REG_DISPLAY_COMMON / 4] == 0xe1U);
	f.regs[COMPAS_FB_HW_REG_DISPLAY_COMMON / 4] = 0xffffffffU;
	assert(compas_fb_hw_program_r1_tft(&ops) == 0);
	assert(f.regs[COMPAS_FB_HW_REG_DISPLAY_COMMON / 4] == 0xffc0ffedU);

	/* A stuck W1C must stop before any TFT programming writes. */
	memset(&f, 0, sizeof(f));
	f.sticky_clear = 1;
	f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] = COMPAS_FB_HW_FRAME_END;
	assert(compas_fb_hw_program_r1_tft(&ops) == -EIO);
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_COMMON_CONFIG, 1));
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_TFT_HSYNC, 1));

	/* Gated clocks return a fixed 0x80 bus value and must fail closed. */
	memset(&f, 0, sizeof(f));
	f.gated_bus = 1;
	assert(compas_fb_hw_program_r1_tft(&ops) == -EIO);
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_INTERRUPT_MASK, 1));
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_TFT_HSYNC, 1));
	assert(compas_fb_hw_stop_rdma(&ops, 4) == -EIO);
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_CTRL, 1));

	/* Truly idle: drain stale IRQ flags, recheck status, and do not stop. */
	memset(&f, 0, sizeof(f));
	f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] =
		COMPAS_FB_HW_STOP_ACK | COMPAS_FB_HW_FRAME_END;
	assert(compas_fb_hw_stop_rdma(&ops, 4) == 0);
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_CTRL, 1));
	assert(f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] == 0);
	assert(f.regs[COMPAS_FB_HW_REG_STATUS / 4] == 0);

	/* Composer/writeback active is not owned by this RDMA backend. */
	memset(&f, 0, sizeof(f));
	f.regs[COMPAS_FB_HW_REG_STATUS / 4] = 1U << 4;
	assert(compas_fb_hw_stop_rdma(&ops, 4) == -EOPNOTSUPP);
	assert(!find_event(&f, 0, COMPAS_FB_HW_REG_CTRL, 1));

	/* Active RDMA requires a new ACK and a cleared activity status. */
	memset(&f, 0, sizeof(f));
	f.regs[COMPAS_FB_HW_REG_STATUS / 4] = (1U << 0) | (1U << 3);
	f.acknowledge_stop = 0;
	{
		unsigned int begin = f.count, i;
		assert(compas_fb_hw_stop_rdma(&ops, 2) == -ETIMEDOUT);
		assert(find_event(&f, begin, COMPAS_FB_HW_REG_CTRL, 1));
		for (i = begin; i < f.count; ++i)
			assert(f.events[i].offset != COMPAS_FB_HW_REG_TFT_HSYNC &&
			       f.events[i].offset != COMPAS_FB_HW_REG_RDMA_CHAIN_ADDR &&
			       f.events[i].offset != COMPAS_FB_HW_REG_RDMA_CHAIN_CTRL);
		assert(f.regs[COMPAS_FB_HW_REG_STATUS / 4] == ((1U << 0) | (1U << 3)));
	}

	/* A fresh ACK alone is insufficient if the live RDMA-working bits persist. */
	memset(&f, 0, sizeof(f));
	f.regs[COMPAS_FB_HW_REG_STATUS / 4] = (1U << 0) | (1U << 3);
	f.acknowledge_stop = 1;
	f.clear_activity_on_stop = 0;
	assert(compas_fb_hw_stop_rdma(&ops, 2) == -ETIMEDOUT);
	assert(f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] & COMPAS_FB_HW_STOP_ACK);
	assert(f.regs[COMPAS_FB_HW_REG_STATUS / 4] & ((1U << 0) | (1U << 3)));

	/* Fresh ACK plus cleared status succeeds and drains the fresh frame event. */
	memset(&f, 0, sizeof(f));
	f.regs[COMPAS_FB_HW_REG_STATUS / 4] = (1U << 0) | (1U << 3);
	f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] =
		COMPAS_FB_HW_STOP_ACK | COMPAS_FB_HW_FRAME_END;
	f.acknowledge_stop = 1;
	f.clear_activity_on_stop = 1;
	f.frame_end_on_stop = 1;
	f.reassert_eod_while_active = 1;
	{
		unsigned int begin = f.count;
		assert(compas_fb_hw_stop_rdma(&ops, 4) == 0);
		clear = find_event(&f, begin, COMPAS_FB_HW_REG_CLEAR_STATUS, 1);
		ctrl = find_event(&f, begin, COMPAS_FB_HW_REG_CTRL, 1);
		assert(clear && ctrl && clear < ctrl);
		assert(clear->value == COMPAS_FB_HW_STOP_ACK);
		/* Full event drain occurs after stop and clears a fresh EOD. */
		clear = find_event(&f, (unsigned int)(ctrl - f.events + 1),
				   COMPAS_FB_HW_REG_CLEAR_STATUS, 1);
		assert(clear && (clear->value & (COMPAS_FB_HW_STOP_ACK |
					COMPAS_FB_HW_FRAME_END)) ==
		       (COMPAS_FB_HW_STOP_ACK | COMPAS_FB_HW_FRAME_END));
		assert(ctrl->value == COMPAS_FB_HW_GENERAL_STOP_RDMA);
		assert(!(f.regs[COMPAS_FB_HW_REG_INTERRUPT_FLAG / 4] & (COMPAS_FB_HW_STOP_ACK | COMPAS_FB_HW_FRAME_END | COMPAS_FB_HW_UNDERRUN)));
		assert(!(f.regs[COMPAS_FB_HW_REG_STATUS / 4] &
			 ((1U << 0) | (1U << 3) | (1U << 4) | (1U << 5))));
	}

	assert(compas_fb_hw_start_rdma(&ops, 0x01234000U) == 0);
	assert(f.regs[COMPAS_FB_HW_REG_RDMA_CHAIN_ADDR / 4] == 0x01234000U);
	assert(f.regs[COMPAS_FB_HW_REG_RDMA_CHAIN_CTRL / 4] == 1U);
	assert(compas_fb_hw_start_rdma(&ops, 0x01234004U) == -EINVAL);

	/* Adoption accepts only a coherent descriptor/site/status snapshot. The
	 * SRD_WORKING bit is bit 3 in the pinned X1600 dpu_reg.h definition. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING);
	f.regs[COMPAS_FB_HW_REG_RDMA_SITE / 4] = 0x01250000U;
	/* Base address alone may be a stale next-fetch value; require observed
	 * forward fetch progress before declaring the page adopted. */
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == -ETIMEDOUT);
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250002U,
			COMPAS_FB_HW_SRD_WORKING);
	f.regs[COMPAS_FB_HW_REG_RDMA_SITE / 4] = 0x01250002U;
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == 0);
	{
		unsigned int total;
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == 0U && total == 0U);
	}
	{
		struct event *chain_a = find_event(&f, 0,
			COMPAS_FB_HW_REG_RDMA_CHAIN_SITE, 0);
		struct event *site = find_event(&f, 0, COMPAS_FB_HW_REG_RDMA_SITE, 0);
		struct event *status = find_event(&f, 0, COMPAS_FB_HW_REG_STATUS, 0);
		struct event *chain_b = chain_a ? find_event(&f,
			(unsigned int)(chain_a - f.events) + 1U,
			COMPAS_FB_HW_REG_RDMA_CHAIN_SITE, 0) : NULL;
		assert(chain_a && site && status && chain_b &&
		       chain_a < site && site < status && status < chain_b);
	}
	/* Fetch progress may continue through the last RGB565 pixel. */
	f.regs[COMPAS_FB_HW_REG_RDMA_SITE / 4] =
		0x01250000U + COMPAS_FB_HW_R1_PAGE_BYTES - 2U;
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == 0);
	/* Last-byte address is not a complete 16-bit RGB565 pixel. */
	f.regs[COMPAS_FB_HW_REG_RDMA_SITE / 4]++;
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == -ETIMEDOUT);

	/* A stale descriptor retries; the following coherent target snapshot wins. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250002U,
			COMPAS_FB_HW_SRD_WORKING);
	{
		const uint32_t chains[] = { 0x01234100U, 0x01234000U,
					    0x01234000U, 0x01234000U };
		script_reads(&f, COMPAS_FB_HW_REG_RDMA_CHAIN_SITE, chains, 4U);
	}
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == 0);

	/* A chain that changes during the snapshot is never accepted. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250002U,
			COMPAS_FB_HW_SRD_WORKING);
	{
		const uint32_t chains[] = { 0x01234000U, 0x01234040U,
					    0x01234000U, 0x01234000U };
		script_reads(&f, COMPAS_FB_HW_REG_RDMA_CHAIN_SITE, chains, 4U);
	}
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == 0);

	/* The old framebuffer page is transient until RDMA_SITE enters the target. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01240000U,
			COMPAS_FB_HW_SRD_WORKING);
	{
		const uint32_t sites[] = { 0x01240000U, 0x01250002U };
		script_reads(&f, COMPAS_FB_HW_REG_RDMA_SITE, sites, 2U);
	}
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == 0);

	/* A not-yet-working engine retries until the target is actually scanning. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250002U, 0U);
	{
		const uint32_t states[] = { 0U, COMPAS_FB_HW_SRD_WORKING };
		script_reads(&f, COMPAS_FB_HW_REG_STATUS, states, 2U);
	}
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == 0);
	{
		unsigned int total;
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == 1U && total == 1U);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == 0U && total == 1U);
	}

	/* Two failed fast samples lead to adoption on retry two after exactly two
	 * fast delays; no 1ms slow delay is paid. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING);
	{
		const uint32_t sites[] = { 0x01250000U, 0x01250000U,
					   0x01250002U };
		unsigned int total;
		script_reads(&f, COMPAS_FB_HW_REG_RDMA_SITE, sites, 3U);
		assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U,
			0x01250000U, COMPAS_FB_HW_R1_PAGE_BYTES, 3U) == 0);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == 2U && total == 2U);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == 0U && total == 2U);
	}
	/* Five failed fast samples enter slow polling; adoption on its second
	 * sample retains the complete fast prefix and pays one 1ms delay. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING);
	{
		const uint32_t sites[] = {
			0x01250000U, 0x01250000U, 0x01250000U, 0x01250000U,
			0x01250000U, 0x01250000U, 0x01250002U
		};
		unsigned int total;
		script_reads(&f, COMPAS_FB_HW_REG_RDMA_SITE, sites, 7U);
		assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U,
			0x01250000U, COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == 0);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == COMPAS_FB_HW_ADOPT_FAST_RETRIES &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES + 1U);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == 1U &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES + 1U);
	}

	/* Direct composer or writeback ownership is unsupported, not adopted. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234000U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING | COMPAS_FB_HW_DIRECT_WORKING);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == -EOPNOTSUPP);
	/* Unsupported composer/writeback ownership remains an error even when it
	 * appears only after the fast prefix, during the slow phase. */
	{
		const uint32_t states[] = {
			COMPAS_FB_HW_SRD_WORKING, COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING, COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING | COMPAS_FB_HW_DIRECT_WORKING
		};
		unsigned int total;
		memset(&f, 0, sizeof(f));
		set_adopt_state(&f, 0x01234000U, 0x01250000U,
				COMPAS_FB_HW_SRD_WORKING);
		script_reads(&f, COMPAS_FB_HW_REG_STATUS, states, 6U);
		assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U,
			0x01250000U, COMPAS_FB_HW_R1_PAGE_BYTES, 2U) ==
		       -EOPNOTSUPP);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == COMPAS_FB_HW_ADOPT_FAST_RETRIES &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == 0U && total == COMPAS_FB_HW_ADOPT_FAST_RETRIES);
	}
	{
		const uint32_t states[] = {
			COMPAS_FB_HW_SRD_WORKING, COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING, COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING,
			COMPAS_FB_HW_SRD_WORKING | COMPAS_FB_HW_WRBK_WORKING
		};
		memset(&f, 0, sizeof(f));
		set_adopt_state(&f, 0x01234000U, 0x01250000U,
				COMPAS_FB_HW_SRD_WORKING);
		script_reads(&f, COMPAS_FB_HW_REG_STATUS, states, 6U);
		assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U,
			0x01250000U, COMPAS_FB_HW_R1_PAGE_BYTES, 2U) ==
		       -EOPNOTSUPP);
	}
	f.regs[COMPAS_FB_HW_REG_STATUS / 4] =
		COMPAS_FB_HW_SRD_WORKING | COMPAS_FB_HW_WRBK_WORKING;
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 2U) == -EOPNOTSUPP);

	/* No coherent target within the bound is a timeout, never adoption. */
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234100U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 3U) == -ETIMEDOUT);
	/* The failed fast prefix is followed by the full 100-poll slow budget. */
	{
		unsigned int total;
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == COMPAS_FB_HW_ADOPT_FAST_RETRIES &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES + 3U);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == 3U &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES + 3U);
	}
	memset(&f, 0, sizeof(f));
	set_adopt_state(&f, 0x01234100U, 0x01250000U,
			COMPAS_FB_HW_SRD_WORKING);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_ADOPT_POLLS) ==
	       -ETIMEDOUT);
	{
		unsigned int total;
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == COMPAS_FB_HW_ADOPT_FAST_RETRIES &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES +
				COMPAS_FB_HW_ADOPT_POLLS);
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_DELAY_US,
				    &total) == COMPAS_FB_HW_ADOPT_POLLS &&
		       total == COMPAS_FB_HW_ADOPT_FAST_RETRIES +
				COMPAS_FB_HW_ADOPT_POLLS);
	}

	/* Strict bounded arguments and 29-bit address overflow checks. */
	memset(&f, 0, sizeof(f));
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 0U) == -EINVAL);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, COMPAS_FB_HW_ADOPT_POLLS + 1U) == -EINVAL);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x20000000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == -EINVAL);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U,
		0x20000000U - COMPAS_FB_HW_R1_PAGE_BYTES + 64U,
		COMPAS_FB_HW_R1_PAGE_BYTES, 1U) == -EINVAL);
	assert(compas_fb_hw_wait_rdma_adopted(&ops, 0x01234000U, 0x01250000U,
		COMPAS_FB_HW_R1_PAGE_BYTES - 2U, 1U) == -EINVAL);
	{
		unsigned int total;
		assert(count_delays(&f, COMPAS_FB_HW_ADOPT_FAST_DELAY_US,
				    &total) == 0U && total == 0U);
		assert(f.count == 0U);
	}
	puts("compas_fb_hw: PASS");
	return 0;
}
