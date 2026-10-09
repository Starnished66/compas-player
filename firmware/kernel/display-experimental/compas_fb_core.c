// SPDX-License-Identifier: GPL-2.0-only
#include "compas_fb_core.h"

#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

static int compas_fb_valid_page(compas_fb_u32 page)
{
	return page < COMPAS_FB_PAGE_COUNT;
}

int compas_fb_geometry_init(struct compas_fb_geometry *geometry,
			   compas_fb_u32 width, compas_fb_u32 height,
			   compas_fb_u32 stride, compas_fb_u32 page_count)
{
	compas_fb_u64 minimum_stride;
	compas_fb_u64 page_bytes;
	compas_fb_u64 max_value = ~(compas_fb_u64)0;
	compas_fb_u64 max_size = (compas_fb_u64)~(compas_fb_u32)0;

	if (!geometry || !width || !height || page_count != COMPAS_FB_PAGE_COUNT)
		return -EINVAL;
	minimum_stride = (compas_fb_u64)width * COMPAS_FB_RGB565_BYTES_PER_PIXEL;
	if ((compas_fb_u64)stride < minimum_stride || (stride & 7U))
		return -EINVAL;
	page_bytes = (compas_fb_u64)stride * height;
	if (page_bytes > max_value / COMPAS_FB_PAGE_COUNT)
		return -EOVERFLOW;
	if (page_bytes * COMPAS_FB_PAGE_COUNT > max_size)
		return -EOVERFLOW;

	geometry->width = width;
	geometry->height = height;
	geometry->stride = stride;
	geometry->page_bytes = page_bytes;
	geometry->allocation_bytes = page_bytes * COMPAS_FB_PAGE_COUNT;
	return 0;
}

int compas_fb_dma_page_address(const struct compas_fb_geometry *geometry,
			       compas_fb_u64 allocation_base,
			       compas_fb_u64 allocated_bytes,
			       compas_fb_u32 page,
			       compas_fb_u32 *address)
{
	struct compas_fb_geometry checked;
	compas_fb_u64 max_address = (compas_fb_u64)~(compas_fb_u32)0;
	compas_fb_u64 page_offset;
	compas_fb_u64 page_address;
	int ret;

	if (!geometry || !address || !compas_fb_valid_page(page))
		return -EINVAL;
	ret = compas_fb_geometry_init(&checked, geometry->width, geometry->height,
				      geometry->stride, COMPAS_FB_PAGE_COUNT);
	if (ret || checked.page_bytes != geometry->page_bytes ||
	    checked.allocation_bytes != geometry->allocation_bytes)
		return -EINVAL;
	if (allocated_bytes < geometry->allocation_bytes)
		return -EINVAL;
	if (allocation_base & 7U)
		return -EINVAL;
	if (allocation_base > max_address ||
	    geometry->allocation_bytes - 1 > max_address - allocation_base)
		return -EOVERFLOW;

	page_offset = geometry->page_bytes * page;
	page_address = allocation_base + page_offset;
	if (page_address & 7U)
		return -EINVAL;
	if (page_address > max_address)
		return -EOVERFLOW;
	*address = (compas_fb_u32)page_address;
	return 0;
}

int compas_fb_pan_init(struct compas_fb_pan_state *state,
		       compas_fb_u32 initial_page)
{
	if (!state || !compas_fb_valid_page(initial_page))
		return -EINVAL;
	state->committed_page = initial_page;
	state->submitted_page = state->committed_page;
	state->generation = 0;
	state->running = 0;
	state->pending = 0;
	state->uncertain = 0;
	state->stop_required = 0;
	state->stop_confirmed = 0;
	return 0;
}

int compas_fb_pan_start(struct compas_fb_pan_state *state)
{
	if (!state)
		return -EINVAL;
	if (state->uncertain || state->stop_required || state->pending)
		return -EIO;
	if (state->running)
		return -EBUSY;
	state->running = 1;
	return 0;
}

int compas_fb_pan_begin(struct compas_fb_pan_state *state,
			compas_fb_u32 page, compas_fb_u32 *token)
{
	if (!state || !token || !compas_fb_valid_page(page))
		return -EINVAL;
	if (state->uncertain || state->stop_required)
		return -EIO;
	if (!state->running)
		return -ESHUTDOWN;
	if (state->pending)
		return -EBUSY;
	if (page == state->committed_page) {
		*token = 0;
		return 0;
	}
	if (state->generation == ~(compas_fb_u32)0)
		return -EOVERFLOW;
	state->generation++;
	state->submitted_page = page;
	state->pending = 1;
	*token = state->generation;
	return 0;
}

int compas_fb_pan_complete(struct compas_fb_pan_state *state,
			   compas_fb_u32 token)
{
	if (!state || !token)
		return -EINVAL;
	if (!state->pending || token != state->generation)
		return -ESTALE;
	state->committed_page = state->submitted_page;
	state->pending = 0;
	return 0;
}

int compas_fb_pan_timeout(struct compas_fb_pan_state *state,
			  compas_fb_u32 token)
{
	if (!state || !token)
		return -EINVAL;
	if (!state->pending || token != state->generation)
		return -ESTALE;
	state->pending = 0;
	state->running = 0;
	state->uncertain = 1;
	state->stop_required = 1;
	state->stop_confirmed = 0;
	return -ETIMEDOUT;
}

void compas_fb_pan_stop_requested(struct compas_fb_pan_state *state)
{
	if (!state)
		return;
	state->running = 0;
	state->pending = 0;
	state->uncertain = 1;
	state->stop_required = 1;
	state->stop_confirmed = 0;
}

int compas_fb_pan_stop_confirmed(struct compas_fb_pan_state *state)
{
	if (!state)
		return -EINVAL;
	if (!state->stop_required || state->running)
		return -EINVAL;
	state->stop_confirmed = 1;
	return 0;
}

int compas_fb_pan_reset(struct compas_fb_pan_state *state,
			compas_fb_u32 initial_page)
{
	if (!state || !compas_fb_valid_page(initial_page))
		return -EINVAL;
	if (state->running || state->pending || !state->stop_confirmed)
		return -EBUSY;
	state->committed_page = initial_page;
	state->submitted_page = initial_page;
	state->pending = 0;
	state->uncertain = 0;
	state->stop_required = 0;
	state->stop_confirmed = 0;
	return 0;
}
