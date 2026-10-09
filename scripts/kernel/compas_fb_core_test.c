#include "../../firmware/kernel/display-experimental/compas_fb_core.h"

#include <errno.h>
#include <stdio.h>

static int failures;

#define CHECK(condition, label) do { \
	if (!(condition)) { \
		fprintf(stderr, "FAIL: %s\n", label); \
		failures++; \
	} \
} while (0)

static void test_geometry(void)
{
	struct compas_fb_geometry geometry;

	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 960, 2) == 0,
	      "RGB565 480x800 geometry accepted");
	CHECK(geometry.page_bytes == 768000 &&
	      geometry.allocation_bytes == 1536000,
	      "two tightly packed RGB565 pages have correct sizes");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 968, 2) == 0,
	      "aligned padded stride accepted");
	CHECK(geometry.page_bytes == 774400 &&
	      geometry.allocation_bytes == 1548800,
	      "padded stride is included in page and allocation sizes");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 962, 2) == -EINVAL,
	      "unaligned stride rejected");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 952, 2) == -EINVAL,
	      "stride smaller than RGB565 row rejected");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 960, 1) == -EINVAL,
	      "single page rejected");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 960, 3) == -EINVAL,
	      "more than two pages rejected");
	CHECK(compas_fb_geometry_init(&geometry, 0, 800, 960, 2) == -EINVAL,
	      "zero width rejected");
	CHECK(compas_fb_geometry_init(&geometry, 1, 0, 8, 2) == -EINVAL,
	      "zero height rejected");
	CHECK(compas_fb_geometry_init(NULL, 1, 1, 8, 2) == -EINVAL,
	      "null geometry rejected");
	CHECK(compas_fb_geometry_init(&geometry, 1, 0xffffffffU,
				      0xfffffff8U, 2) == -EOVERFLOW,
	      "allocation multiplication overflow rejected");
	CHECK(compas_fb_geometry_init(&geometry, 1, 4, 0xfffffff8U, 2) ==
	      -EOVERFLOW,
	      "allocation larger than 32-bit size_t rejected");
}

static void test_dma_page_addresses(void)
{
	struct compas_fb_geometry geometry;
	compas_fb_u32 address = 0;

	CHECK(compas_fb_geometry_init(&geometry, 1, 1, 8, 2) == 0,
	      "small aligned DMA geometry initialized");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 16, 0,
					 &address) == 0 && address == 0xfffffff0U,
	      "page zero at the 32-bit bus boundary is valid");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 16, 1,
					 &address) == 0 && address == 0xfffffff8U,
	      "last page address remains within the 32-bit bus range");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 24, 1,
					 &address) == 0 && address == 0xfffffff8U,
	      "padding beyond bus range is outside required DMA span");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 15, 0,
					 &address) == -EINVAL,
	      "undersized allocation is rejected");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff8ULL, 16, 0,
					 &address) == -EOVERFLOW,
	      "allocation crossing the 32-bit bus limit is rejected");
	CHECK(compas_fb_dma_page_address(&geometry, 0x100000000ULL, 16, 0,
					 &address) == -EOVERFLOW,
	      "base above the 32-bit bus limit is rejected");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff1ULL, 16, 0,
					 &address) == -EINVAL,
	      "misaligned DMA base is rejected");
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 16, 2,
					 &address) == -EINVAL,
	      "invalid DMA page index is rejected");
	CHECK(compas_fb_geometry_init(&geometry, 480, 800, 960, 2) == 0,
	      "device-sized DMA geometry initialized");
	CHECK(compas_fb_dma_page_address(&geometry, 0x10000000ULL, 1536000, 1,
					 &address) == 0 && address == 0x100bb800U,
	      "real 480x800 page-one DMA address uses byte stride");
	geometry.page_bytes++;
	CHECK(compas_fb_dma_page_address(&geometry, 0xfffffff0ULL, 1536000, 0,
					 &address) == -EINVAL,
	      "inconsistent geometry structure is rejected");
}

static void test_pan_submit_and_completion(void)
{
	struct compas_fb_pan_state state;
	compas_fb_u32 token, stale, generation;

	CHECK(compas_fb_pan_init(&state, 0) == 0, "page-zero state initialized");
	CHECK(compas_fb_pan_init(&state, 2) == -EINVAL, "invalid initial page rejected");
	token = 0x12345678U;
	CHECK(compas_fb_pan_begin(&state, 1, &token) == -ESHUTDOWN &&
	      token == 0x12345678U,
	      "begin before start fails without overwriting caller token");
	CHECK(compas_fb_pan_start(&state) == 0, "scanout starts");
	CHECK(compas_fb_pan_start(&state) == -EBUSY, "second start rejected");
	generation = state.generation;
	token = 99;
	CHECK(compas_fb_pan_begin(&state, 0, &token) == 0 && token == 0 &&
	      state.generation == generation && !state.pending,
	      "request for committed page is a no-op");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0 && token != 0 &&
	      state.pending && state.submitted_page == 1 && state.committed_page == 0,
	      "page change creates a pending generation");
	stale = token + 1;
	CHECK(compas_fb_pan_begin(&state, 0, &token) == -EBUSY && token == generation + 1,
	      "second submission rejected while one is pending");
	CHECK(compas_fb_pan_complete(&state, 0) == -EINVAL &&
	      compas_fb_pan_timeout(&state, 0) == -EINVAL,
	      "zero token cannot complete or time out a pan");
	CHECK(compas_fb_pan_complete(&state, stale) == -ESTALE && state.pending &&
	      state.committed_page == 0,
	      "mismatched completion cannot commit pending page");
	CHECK(compas_fb_pan_complete(&state, token) == 0 && !state.pending &&
	      state.committed_page == 1,
	      "completion can arrive before caller waits and commits page");
	CHECK(compas_fb_pan_timeout(&state, token) == -ESTALE,
	      "timeout loses after completion already committed");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0 && token == 0,
	      "repeat submission of committed page remains a no-op");
	CHECK(compas_fb_pan_begin(&state, 2, &token) == -EINVAL,
	      "out-of-range page rejected");

	CHECK(compas_fb_pan_begin(&state, 0, &token) == 0 && token != 0,
	      "next pan gets a new generation");
	stale = token;
	CHECK(compas_fb_pan_complete(&state, token) == 0,
	      "second pan completes");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0,
	      "subsequent pan submitted");
	CHECK(compas_fb_pan_complete(&state, stale) == -ESTALE &&
	      state.pending && state.committed_page == 0,
	      "late completion from older generation is ignored");
	CHECK(compas_fb_pan_timeout(&state, stale) == -ESTALE && state.pending,
	      "timeout with an old token leaves current pan pending");
	CHECK(compas_fb_pan_complete(&state, token) == 0 &&
	      state.committed_page == 1,
	      "current generation still completes after stale event");
}

static void test_generation_exhaustion_is_permanent(void)
{
	struct compas_fb_pan_state state;
	compas_fb_u32 token;

	CHECK(compas_fb_pan_init(&state, 0) == 0,
	      "generation exhaustion state initialized");
	CHECK(compas_fb_pan_start(&state) == 0, "generation state starts");
	state.generation = 0xfffffffeU;
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0 &&
	      token == 0xffffffffU,
	      "last nonzero generation token is issued");
	CHECK(compas_fb_pan_complete(&state, token) == 0,
	      "last generation can complete");
	token = 0x12345678U;
	CHECK(compas_fb_pan_begin(&state, 0, &token) == -EOVERFLOW &&
	      token == 0x12345678U,
	      "generation exhaustion fails without wrapping or changing output");
	compas_fb_pan_stop_requested(&state);
	CHECK(compas_fb_pan_stop_confirmed(&state) == 0 &&
	      compas_fb_pan_reset(&state, 1) == 0,
	      "stop/reset does not rewind exhausted generation");
	CHECK(compas_fb_pan_start(&state) == 0, "reset state can start");
	CHECK(compas_fb_pan_begin(&state, 0, &token) == -EOVERFLOW &&
	      token == 0x12345678U,
	      "exhaustion remains until a fresh state after references drain");
}

static void test_timeout_requires_stopped_reset(void)
{
	struct compas_fb_pan_state state;
	compas_fb_u32 token, generation;

	CHECK(compas_fb_pan_init(&state, 0) == 0, "timeout state initialized");
	CHECK(compas_fb_pan_stop_confirmed(&state) == -EINVAL,
	      "stop confirmation without a stop request is rejected");
	CHECK(compas_fb_pan_start(&state) == 0, "timeout scanout starts");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0,
	      "timeout pan submitted");
	generation = state.generation;
	CHECK(compas_fb_pan_timeout(&state, token) == -ETIMEDOUT &&
	      state.committed_page == 0 && !state.pending && state.uncertain &&
	      state.stop_required,
	      "timeout leaves committed page and marks physical state uncertain");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == -EIO,
	      "uncertain scanout blocks another pan");
	CHECK(compas_fb_pan_start(&state) == -EIO,
	      "uncertain scanout cannot restart directly");
	CHECK(compas_fb_pan_complete(&state, generation) == -ESTALE,
	      "late completion after timeout cannot commit");
	CHECK(compas_fb_pan_reset(&state, 0) == -EBUSY,
	      "reset requires confirmed hardware stop");
	CHECK(compas_fb_pan_stop_confirmed(&state) == 0,
	      "stopped hardware is explicitly confirmed");
	CHECK(compas_fb_pan_reset(&state, 0) == 0 && !state.uncertain &&
	      !state.stop_required && state.generation == generation,
	      "reset clears uncertainty without reusing generations");
	CHECK(compas_fb_pan_start(&state) == 0,
	      "scanout can restart after confirmed stop and reset");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0 && token > generation,
	      "post-reset pan receives a fresh token");
}

static void test_blank_shutdown_abort_and_reset(void)
{
	struct compas_fb_pan_state state;
	compas_fb_u32 token;

	CHECK(compas_fb_pan_init(&state, 0) == 0, "stop-path state initialized");
	CHECK(compas_fb_pan_start(&state) == 0, "stop-path scanout starts");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0,
	      "stop-path pan submitted");
	compas_fb_pan_stop_requested(&state); /* blank or shutdown */
	CHECK(!state.pending && !state.running && state.uncertain &&
	      state.stop_required,
	      "blank or shutdown aborts pending pan and requires stop");
	CHECK(compas_fb_pan_complete(&state, token) == -ESTALE,
	      "aborted pan completion is ignored");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == -EIO,
	      "new pan blocked until stop is confirmed and reset");
	CHECK(compas_fb_pan_reset(&state, 0) == -EBUSY,
	      "blank path cannot reset before confirmed stop");
	CHECK(compas_fb_pan_stop_confirmed(&state) == 0,
	      "blank path stop is confirmed");
	CHECK(compas_fb_pan_reset(&state, 0) == 0,
	      "blank path can reset after confirmed stop");
	CHECK(compas_fb_pan_start(&state) == 0, "scanout restarts after blank reset");
	CHECK(compas_fb_pan_begin(&state, 1, &token) == 0,
	      "shutdown path pan submitted");
	compas_fb_pan_stop_requested(&state); /* shutdown */
	CHECK(!state.pending && state.stop_required,
	      "shutdown also aborts pending page change");
	CHECK(compas_fb_pan_stop_confirmed(&state) == 0 &&
	      compas_fb_pan_reset(&state, 0) == 0,
	      "shutdown reset follows stop confirmation");
}

int main(void)
{
	test_geometry();
	test_dma_page_addresses();
	test_pan_submit_and_completion();
	test_generation_exhaustion_is_permanent();
	test_timeout_requires_stopped_reset();
	test_blank_shutdown_abort_and_reset();
	if (failures)
		return 1;
	puts("experimental framebuffer core tests passed (6 groups)");
	return 0;
}
