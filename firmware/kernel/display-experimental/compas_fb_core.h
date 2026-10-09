/* SPDX-License-Identifier: GPL-2.0-only */
/* Hardware-independent state and geometry for the experimental framebuffer.
 * The caller serializes every operation; this core contains no locks. */
#ifndef COMPAS_FB_CORE_H
#define COMPAS_FB_CORE_H

#ifdef __KERNEL__
#include <linux/types.h>
typedef u32 compas_fb_u32;
typedef u64 compas_fb_u64;
#else
#include <stdint.h>
typedef uint32_t compas_fb_u32;
typedef uint64_t compas_fb_u64;
#endif

#define COMPAS_FB_RGB565_BYTES_PER_PIXEL 2U
#define COMPAS_FB_PAGE_COUNT 2U

struct compas_fb_geometry {
	compas_fb_u32 width;
	compas_fb_u32 height;
	compas_fb_u32 stride;
	compas_fb_u64 page_bytes;
	compas_fb_u64 allocation_bytes;
};

struct compas_fb_pan_state {
	compas_fb_u32 committed_page;
	compas_fb_u32 submitted_page;
	compas_fb_u32 generation;
	compas_fb_u32 running;
	compas_fb_u32 pending;
	compas_fb_u32 uncertain;
	compas_fb_u32 stop_required;
	compas_fb_u32 stop_confirmed;
};

/* RGB565 uses two bytes per pixel; exactly two frame pages are supported.
 * The allocation must also fit MIPS32's 32-bit size/addressable range. */
int compas_fb_geometry_init(struct compas_fb_geometry *geometry,
			   compas_fb_u32 width, compas_fb_u32 height,
			   compas_fb_u32 stride, compas_fb_u32 page_count);

/* allocation_base must be the allocator's DMA/bus address, never virt_to_phys.
 * allocated_bytes is the actual buffer size and must cover the required
 * two-page span. Any allocator padding may extend beyond the 32-bit range;
 * only the required scanout span is checked. The base and page address must
 * be 8-byte aligned. */
int compas_fb_dma_page_address(const struct compas_fb_geometry *geometry,
			       compas_fb_u64 allocation_base,
			       compas_fb_u64 allocated_bytes,
			       compas_fb_u32 page,
			       compas_fb_u32 *address);

/* Initialize a fresh state object only. Never reinitialize a live or faulted
 * instance. The initial page from init/reset must be programmed in hardware
 * before the next start.
 * Token generations never wrap; exhaustion requires a fresh instance after
 * all old waiters/references have drained. */
int compas_fb_pan_init(struct compas_fb_pan_state *state,
		       compas_fb_u32 initial_page);
int compas_fb_pan_start(struct compas_fb_pan_state *state);
/* A successful begin with token 0 is a no-op and needs no wait. On failure,
 * token is left unchanged. complete and timeout reject token 0. */
int compas_fb_pan_begin(struct compas_fb_pan_state *state,
			compas_fb_u32 page, compas_fb_u32 *token);
/* Call completion only after hardware completion has been verified and
 * correlated with this token. A generic IRQ without descriptor identity, or
 * draining old IRQ/work, cannot manufacture a valid completion token. */
int compas_fb_pan_complete(struct compas_fb_pan_state *state,
			   compas_fb_u32 token);
int compas_fb_pan_timeout(struct compas_fb_pan_state *state,
			  compas_fb_u32 token);

/* A timeout returns -ETIMEDOUT only while that token is still pending. If it
 * returns -ESTALE, completion may have won or stop may have aborted the pan;
 * compare committed_page with the requested page to distinguish them. After
 * timeout, pans and restart return -EIO until the wrapper stops DMA, drains
 * IRQ/work, confirms stop and resets. The core does not schedule recovery. */

/* Blank/shutdown paths call this before stopping scanout. stop_confirmed means
 * the caller stopped DMA, drained its IRQ/work, and every in-flight pan waiter
 * has returned. Only then may reset clear
 * pan uncertainty. Framebuffer mappings/lifetime must be retained separately. */
void compas_fb_pan_stop_requested(struct compas_fb_pan_state *state);
int compas_fb_pan_stop_confirmed(struct compas_fb_pan_state *state);
int compas_fb_pan_reset(struct compas_fb_pan_state *state,
			compas_fb_u32 initial_page);

#endif /* COMPAS_FB_CORE_H */
