/* SPDX-License-Identifier: GPL-2.0-only */
/* Narrow R1 X1600 single-RDMA scanout operations. */
#ifndef COMPAS_FB_HW_H
#define COMPAS_FB_HW_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 compas_fb_hw_u32;
#else
#include <stdint.h>
typedef uint32_t compas_fb_hw_u32;
#endif

#define COMPAS_FB_HW_DPU_BASE 0xb3050000U
#define COMPAS_FB_HW_REG_RDMA_CHAIN_ADDR 0x1000U
#define COMPAS_FB_HW_REG_RDMA_CHAIN_CTRL 0x1004U
#define COMPAS_FB_HW_REG_CTRL 0x2000U
#define COMPAS_FB_HW_REG_STATUS 0x2004U
#define COMPAS_FB_HW_REG_CLEAR_STATUS 0x2008U
#define COMPAS_FB_HW_REG_INTERRUPT_MASK 0x200cU
#define COMPAS_FB_HW_REG_INTERRUPT_FLAG 0x2010U
#define COMPAS_FB_HW_REG_COMMON_CONFIG 0x2014U
#define COMPAS_FB_HW_REG_DISPLAY_COMMON 0x8000U
#define COMPAS_FB_HW_REG_RDMA_CHAIN_SITE 0x2204U
#define COMPAS_FB_HW_REG_RDMA_SITE 0x3110U
#define COMPAS_FB_HW_REG_TFT_HSYNC 0x9000U
#define COMPAS_FB_HW_REG_TFT_VSYNC 0x9004U
#define COMPAS_FB_HW_REG_TFT_HDE 0x9008U
#define COMPAS_FB_HW_REG_TFT_VDE 0x900cU
#define COMPAS_FB_HW_REG_TFT_CONFIG 0x9010U
#define COMPAS_FB_HW_REG_TFT_STATUS 0x9014U

#define COMPAS_FB_HW_STOP_ACK (1U << 7)
#define COMPAS_FB_HW_SRD_WORKING (1U << 3)
#define COMPAS_FB_HW_SRD_END (1U << 1)
#define COMPAS_FB_HW_TFT_WORKING (1U << 1)
#define COMPAS_FB_HW_TFT_UNDERRUN (1U << 0)
#define COMPAS_FB_HW_DIRECT_WORKING (1U << 4)
#define COMPAS_FB_HW_WRBK_WORKING (1U << 5)
#define COMPAS_FB_HW_FRAME_END (1U << 17)
#define COMPAS_FB_HW_UNDERRUN (1U << 8)
#define COMPAS_FB_HW_GENERAL_STOP_RDMA (1U << 4)
#define COMPAS_FB_HW_RDMA_START (1U << 0)
#define COMPAS_FB_HW_CHAIN_RGB565 (2U << 19)
#define COMPAS_FB_HW_CHAIN_EOD_MASK (1U << 17)
#define COMPAS_FB_HW_PHYS_LIMIT 0x20000000U
#define COMPAS_FB_HW_DESC_BYTES 20U
#define COMPAS_FB_HW_DESC_SLOT_BYTES 64U
#define COMPAS_FB_HW_DESC_ALIGN 64U
#define COMPAS_FB_HW_STOP_POLLS 100U
#define COMPAS_FB_HW_ADOPT_POLLS 100U
#define COMPAS_FB_HW_ADOPT_FAST_RETRIES 4U
#define COMPAS_FB_HW_R1_PAGE_BYTES 768000U
#define COMPAS_FB_HW_ADOPT_DELAY_US 1000U
#define COMPAS_FB_HW_ADOPT_FAST_DELAY_US 25U

struct compas_fb_hw_ops {
	void *context;
	compas_fb_hw_u32 (*read)(void *context, compas_fb_hw_u32 offset);
	void (*write)(void *context, compas_fb_hw_u32 offset,
		      compas_fb_hw_u32 value);
	void (*delay_us)(void *context, compas_fb_hw_u32 usec);
};

struct compas_fb_hw_desc_words {
	compas_fb_hw_u32 word[5];
};

/* The caller must enable the DPU/MMIO clocks before calling any hardware
 * operation below. Reads while gated can return a fixed 0x80 bus value. */

/* Inputs are validated 29-bit DMA physical addresses. The caller must convert
 * the KSEG0 pointer returned by the allocator explicitly before calling. */
int compas_fb_hw_encode_r1_rgb565_descriptor(
	struct compas_fb_hw_desc_words *desc,
	compas_fb_hw_u32 desc_phys,
	compas_fb_hw_u32 framebuffer_phys,
	compas_fb_hw_u32 stride_pixels);

/* Program fixed R1 480x800@62 parallel TFT timing and the RGB666 output
 * configuration after the caller has stopped scanout and enabled clocks. */
int compas_fb_hw_program_r1_tft(const struct compas_fb_hw_ops *ops);

/* Stop only reports success after observing a fresh STOP_SRD_ACK. The caller
 * must drain its IRQ/work before invoking this and retain all memory on error. */
int compas_fb_hw_stop_rdma(const struct compas_fb_hw_ops *ops,
			   compas_fb_hw_u32 poll_limit);

/* Caller provides the validated physical address of a 64-byte-aligned
 * descriptor slot and its immutable self-loop contents before scanout. */
int compas_fb_hw_start_rdma(const struct compas_fb_hw_ops *ops,
			    compas_fb_hw_u32 desc_phys);

/* After the caller has stopped/drained the old owner and started the target
 * descriptor, wait for a coherent descriptor/site/status snapshot proving
 * that the R1 single-RDMA engine is fetching from target_page_phys. The
 * poll_limit bounds the existing slow phase; up to four fast retries precede
 * that phase. This does not switch live scanout or make an old page safe to
 * reclaim. */
int compas_fb_hw_wait_rdma_adopted(const struct compas_fb_hw_ops *ops,
				   compas_fb_hw_u32 expected_desc_phys,
				   compas_fb_hw_u32 target_page_phys,
				   compas_fb_hw_u32 target_page_bytes,
				   compas_fb_hw_u32 poll_limit);

/* Experimental continuous-pan fence. The caller drains/disables its IRQ,
 * prepares immediately before submission, then supplies IRQ-latched events
 * captured before acknowledgement. Source descriptors remain immutable and
 * self-looping, with submissions serialized. Fresh retiring-frame DMA/display
 * ends plus coherent target fetching are required concurrently. Physical
 * scanout/old-page-reuse validation is required before deployment. */
struct compas_fb_hw_live_fence {
	compas_fb_hw_u32 events;
	compas_fb_hw_u32 adopted;
};
int compas_fb_hw_prepare_live_fence(const struct compas_fb_hw_ops *ops,
				  struct compas_fb_hw_live_fence *fence);
/* Returns 0 when complete, -EAGAIN while pending, or another negative error.
 * Never acknowledges events or waits. Caller must quiesce on failure. */
int compas_fb_hw_sample_live_fence(const struct compas_fb_hw_ops *ops,
				 compas_fb_hw_u32 expected_desc_phys,
				 compas_fb_hw_u32 target_page_phys,
				 compas_fb_hw_u32 target_page_bytes,
				 compas_fb_hw_u32 latched_events,
				 struct compas_fb_hw_live_fence *fence);

/* Acknowledge only the observed W1C events, then verify they are drained. */
int compas_fb_hw_clear_events(const struct compas_fb_hw_ops *ops,
			      compas_fb_hw_u32 events);

#endif /* COMPAS_FB_HW_H */
