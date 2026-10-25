#include "artwork_memory.h"
#include "cover_decode.h"  /* MAX_PNG_STREAMING_NATIVE_SIDE, jpeg_scale_for_target() */
#include "artwork_clock.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* --- System MemAvailable Helper with Short Cache --- */

#define MEMINFO_CACHE_TTL_MS 50ULL

static size_t mock_mem_available_bytes = 0;

void system_set_mock_mem_available(size_t bytes) {
    mock_mem_available_bytes = bytes;
}

size_t system_get_mem_available_bytes(void) {
    if (mock_mem_available_bytes > 0) {
        return mock_mem_available_bytes;
    }

    static pthread_mutex_t mem_lock = PTHREAD_MUTEX_INITIALIZER;
    static uint64_t last_check_ms = 0;
    static size_t cached_mem_bytes = 0;

    uint64_t now = artwork_monotonic_ms();

    pthread_mutex_lock(&mem_lock);
    if (now - last_check_ms < MEMINFO_CACHE_TTL_MS && last_check_ms != 0) {
        size_t result = cached_mem_bytes;
        pthread_mutex_unlock(&mem_lock);
        return result;
    }

    FILE * f = fopen("/proc/meminfo", "r");
    if (!f) {
#ifdef HOST_BUILD
        /* On host simulator without /proc/meminfo, return a healthy default */
        cached_mem_bytes = 64U * 1024U * 1024U;
        last_check_ms = now;
        pthread_mutex_unlock(&mem_lock);
        return cached_mem_bytes;
#else
        /* On target, failure to read /proc/meminfo must fail safe: return 0
         * so optional thumbnail and warmer jobs are refused rather than crashing. */
        cached_mem_bytes = 0;
        last_check_ms = now;
        pthread_mutex_unlock(&mem_lock);
        return 0;
#endif
    }

    char line[128];
    unsigned long mem_avail_kb = 0;
    unsigned long mem_free_kb = 0;
    unsigned long buffers_kb = 0;
    unsigned long cached_kb = 0;
    bool found_avail = false;

    while (fgets(line, sizeof(line), f)) {
        if (sscanf(line, "MemAvailable: %lu kB", &mem_avail_kb) == 1) {
            found_avail = true;
            break;
        }
        sscanf(line, "MemFree: %lu kB", &mem_free_kb);
        sscanf(line, "Buffers: %lu kB", &buffers_kb);
        sscanf(line, "Cached: %lu kB", &cached_kb);
    }
    fclose(f);

    if (found_avail) {
        cached_mem_bytes = (size_t) mem_avail_kb * 1024U;
    } else if (mem_free_kb > 0) {
        cached_mem_bytes = (size_t) (mem_free_kb + buffers_kb + cached_kb) * 1024U;
    } else {
        cached_mem_bytes = 0;
    }

    last_check_ms = now;
    size_t result = cached_mem_bytes;
    pthread_mutex_unlock(&mem_lock);
    return result;
}

/* --- Overflow-Safe Format-Aware Memory Estimation --- */

size_t artwork_estimate_decode_bytes(artwork_format_t fmt, size_t compressed_size,
                                     size_t native_w, size_t native_h,
                                     size_t target_w, size_t target_h,
                                     uint64_t progressive_coeff_bytes,
                                     uint32_t png_native_bpp) {
    if (native_w == 0 || native_h == 0 || target_w == 0 || target_h == 0) return SIZE_MAX;
    /* PNG_STREAMING is allowed up to MAX_PNG_STREAMING_NATIVE_SIDE (8192),
     * not the 4096 every other format is capped at -- that decoder's own
     * peak RAM is bounded by the post-scale output size, not native size
     * (see the decoder_workspace branch below), so the tighter cap doesn't
     * apply. This must track cover_decode.c's own MAX_PNG_STREAMING_NATIVE_
     * SIDE check exactly: a looser check here than the decoder's own would
     * be harmless (the decoder still rejects it), but a TIGHTER one (as
     * this used to be, hardcoded at 4096 for every format) turned every
     * 4097-8192px streaming-eligible PNG into a permanent SIZE_MAX/
     * LOW_MEMORY reject before ever reaching a decoder that was built to
     * handle exactly that range. */
    size_t max_native = (fmt == ARTWORK_FORMAT_PNG_STREAMING) ? MAX_PNG_STREAMING_NATIVE_SIDE : 4096;
    if (native_w > max_native || native_h > max_native || target_w > 1024 || target_h > 1024) return SIZE_MAX;

    /* Native RGB888 output buffer -- not billed for PNG_STREAMING, which
     * never materializes one at native resolution; its real transient
     * costs (bounded by the post-scale size instead) are billed entirely
     * through decoder_workspace below. */
    uint64_t native_bytes = (fmt == ARTWORK_FORMAT_PNG_STREAMING) ? 0ULL
                           : (uint64_t) native_w * (uint64_t) native_h * 3ULL;
    /* Resized target RGB565 buffer */
    uint64_t target_bytes = (uint64_t) target_w * (uint64_t) target_h * 2ULL;

    /* Decoder internal workspace by format:
     * - PNG: LodePNG transiently inflates zlib stream and allocates intermediate
     *   raw scanline buffers with filter bytes (~4 bytes/pixel) + zlib window.
     * - JPEG: tjpgd streams 8x8 MCU blocks into destination; minimal ~32KB buffer.
     *   native_w/h for JPEG is the post-scale RGB888 size, not the source pixel size.
     * - JPEG_PROGRESSIVE / JPEG_LIBJPEG_BASELINE: vendored libjpeg
     *   decoder-only build -- the latter covers both a baseline file whose
     *   chroma sampling tjpgd's whitelist rejects, and any other baseline
     *   file tjpgd itself failed to decode. native_w/h here are ALSO the
     *   post-scale RGB888 size (same convention as tjpgd JPEG) -- the
     *   coefficient buffer (the dominant, dimension-dependent cost, scaling
     *   with the SOF's true native size, not this post-scale size) is
     *   billed separately via progressive_coeff_bytes below for BOTH
     *   formats, not just progressive (see jpeg_probe_t's own comment for
     *   why a baseline file is billed this same worst-case cost).
     * - BMP: uncompressed linear stream; ~16KB overhead. */
    uint64_t decoder_workspace = 64ULL * 1024ULL;
    if (fmt == ARTWORK_FORMAT_PNG) {
        /* Real transient peak, not a flat "4 bytes/pixel" guess: decodeGeneric()
         * inflates the whole image into a packed "scanlines" buffer (row_bytes
         * = ceil(w * png_native_bpp / 8), * h total) that stays allocated while
         * postProcessScanlines() unfilters it into a SEPARATE decoded buffer
         * (row = max(row_bytes, 4*w)) -- both live at once (pair 1). If the
         * PNG's native color mode isn't already 8-bit RGB, lodepng_decode()
         * then allocates a THIRD 4*w*h conversion buffer -- but only AFTER
         * the scanlines buffer has already been freed, while the decoded one
         * is still live (pair 2). These two pairs never coexist, so the real
         * peak is max(pair1, pair2), not their sum -- billing the sum here
         * (confirmed via host-simulated realistic memory pressure, 2026-09-10:
         * under a plausible ~12MB MemAvailable during the background
         * thumbnail warmer, on top of the icc overestimate below, summing
         * both pairs instead of taking their max was enough on its own to
         * push even a routine 480x480 or 600x600 cover -- the overwhelming
         * common case -- over the warmer's admission budget, rejecting
         * decodable album art outright) was an unnecessary extra ~33-50%
         * margin on top of an already-conservative per-pair estimate, not a
         * correctness requirement. A 16-bit RGBA (64bpp) source is exactly
         * the shape that blew this estimate before (see
         * patches/lvgl_runtime_fixes.patch's PNG hunk): row_bytes there is
         * 8*w, twice the old flat 4*w assumption -- pair1/pair2 below still
         * capture that correctly.
         *
         * icc_estimate_bytes is deliberately NOT decode_png_rgb888()'s own
         * COVER_PNG_MAX_ICC_BYTES (1MB) -- that constant is the decoder's
         * hard safety CAP against a pathological/malicious profile, not a
         * realistic size to bill on every PNG's admission estimate. Its own
         * comment already documents real cover-art ICC profiles as "a few KB
         * to low hundreds of KB"; billing the full 1MB worst-case cap
         * unconditionally here was needlessly pessimistic for the common
         * case this estimate actually has to serve. lodepng_inspect() only
         * reads the IHDR chunk (see its own implementation) and returns
         * before reaching a later iCCP chunk, so the real per-file size
         * genuinely isn't cheaply knowable here without a more invasive
         * pre-scan -- this stays a fixed assumption, just a realistic one
         * instead of the hard ceiling. */
        uint32_t bpp = png_native_bpp ? png_native_bpp : 32;
        uint64_t row_bytes = ((uint64_t) native_w * (uint64_t) bpp + 7ULL) / 8ULL;
        uint64_t argb_row_bytes = 4ULL * (uint64_t) native_w;
        uint64_t stride_bytes = row_bytes > argb_row_bytes ? row_bytes : argb_row_bytes;
        uint64_t icc_estimate_bytes = 256ULL * 1024ULL;
        uint64_t pair1_bytes = 2ULL * stride_bytes * (uint64_t) native_h;
        uint64_t pair2_bytes = stride_bytes * (uint64_t) native_h
                              + 4ULL * (uint64_t) native_w * (uint64_t) native_h;
        uint64_t peak_pair_bytes = pair1_bytes > pair2_bytes ? pair1_bytes : pair2_bytes;
        decoder_workspace = peak_pair_bytes + icc_estimate_bytes + (128ULL * 1024ULL);
    } else if (fmt == ARTWORK_FORMAT_PNG_STREAMING) {
        /* Real transient peak for decode_png_streaming() (cover_decode.c):
         * the concatenated IDAT bytes (a genuinely separate allocation from
         * the resident compressed file already billed via compressed_size
         * above, so billed again here), the 32KB tinfl dictionary, two
         * native-row-width filter-reconstruction buffers, three scaled_w
         * column accumulators, and the scaled-resolution RGB888 output
         * buffer itself (native_w/h downscaled by the same 1/2^n
         * jpeg_scale_for_target() picks for JPEG -- this decoder never
         * holds a native-resolution RGB888 buffer, only this smaller one,
         * which is why native_bytes above is zeroed out for this format
         * instead of being billed at the true native size). */
        uint32_t bpp_bytes = png_native_bpp ? (png_native_bpp / 8) : 4;
        uint64_t row_bytes = (uint64_t) native_w * (uint64_t) bpp_bytes + 1ULL;
        uint8_t scale = jpeg_scale_for_target((int) native_w, (int) native_h,
                                              (int) target_w, (int) target_h);
        uint64_t scaled_w = native_w >> scale, scaled_h = native_h >> scale;
        if (scaled_w < 1) scaled_w = 1;
        if (scaled_h < 1) scaled_h = 1;
        /* decode_png_streaming() keeps downscaling past jpeg_scale_for_
         * target()'s own stopping point when needed purely to fit
         * max_side (MAX_DECODED_COVER_SIDE, the cap it actually enforces
         * via rgb888_size_ok()) -- for an extreme-aspect-ratio native
         * image (e.g. 8192x400) where one side is already below its
         * target at scale=0, jpeg_scale_for_target() alone stops too
         * early and this would otherwise bill the pre-extra-scale (much
         * larger) buffer size, over-admitting relative to what the
         * decoder will actually hold. Mirror that same extra downscale
         * here so the two can't drift the way the old 4096-vs-8192 cap
         * mismatch did. */
        while ((scaled_w > MAX_DECODED_COVER_SIDE || scaled_h > MAX_DECODED_COVER_SIDE) &&
               scaled_w > 1 && scaled_h > 1) {
            scale++;
            scaled_w = native_w >> scale;
            scaled_h = native_h >> scale;
            if (scaled_w < 1) scaled_w = 1;
            if (scaled_h < 1) scaled_h = 1;
        }
        uint64_t scaled_rgb888_bytes = scaled_w * scaled_h * 3ULL;
        uint64_t accum_bytes = scaled_w * 4ULL * 3ULL;
        decoder_workspace = (uint64_t) compressed_size /* idat_buf copy */
                           + 32768ULL /* tinfl dictionary */
                           + (2ULL * row_bytes) /* row_curr/row_prev */
                           + accum_bytes
                           + scaled_rgb888_bytes;
    } else if (fmt == ARTWORK_FORMAT_JPEG) {
        decoder_workspace = 32ULL * 1024ULL;
    } else if (fmt == ARTWORK_FORMAT_JPEG_PROGRESSIVE || fmt == ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE) {
        /* 128KiB workspace (Huffman tables, MCU row buffers, jpeg_decompress_
         * struct) -- larger than baseline tjpgd's flat 32KiB since libjpeg's
         * struct/table footprint is genuinely bigger, independent of image
         * size. progressive_coeff_bytes (the real, dimension/sampling-
         * dependent cost) is added on top, not folded into this constant --
         * real and non-zero for JPEG_LIBJPEG_BASELINE too, not just
         * JPEG_PROGRESSIVE: jpeg_probe() computes it unconditionally for
         * every supported SOF0/SOF2, deliberately conservative (billed as
         * if any baseline file might turn out to be sequential-multiscan,
         * since a cheap header probe can't tell single-scan and multiscan
         * apart) so that case doesn't need its own separate rejection. */
        decoder_workspace = 128ULL * 1024ULL + progressive_coeff_bytes;
    } else if (fmt == ARTWORK_FORMAT_BMP) {
        decoder_workspace = 16ULL * 1024ULL;
    }

    uint64_t total = (uint64_t) compressed_size + native_bytes + target_bytes + decoder_workspace;
    if (total > (uint64_t) SIZE_MAX) return SIZE_MAX;
    return (size_t) total;
}

/* --- Memory Admission Check --- */

#define MEM_RESERVE_PLAYER    (2U * 1024U * 1024U)  /* 2 MiB reserve for player cover */
#define MEM_RESERVE_THUMBNAIL (4U * 1024U * 1024U)  /* 4 MiB reserve for visible thumbnails */
#define MEM_RESERVE_WARMER    (8U * 1024U * 1024U)  /* 8 MiB reserve for background warmer */

/* Single source of truth for the per-priority reserve, shared with
 * src/library/metadata.c's metadata_artwork_limit_memory() -- that function
 * bounds the isolated artwork-extraction child process's own RLIMIT_AS
 * ceiling, which used to hardcode this same WARMER reserve regardless of
 * the caller's real priority, holding an interactive PRIO_PLAYER request to
 * the background warmer's own strictest headroom. Unknown/out-of-range
 * values default to the most conservative (WARMER) reserve, same as the
 * fallback this function already had before this was pulled out. */
size_t artwork_reserve_bytes_for_priority(artwork_priority_t prio) {
    if (prio == ARTWORK_PRIO_PLAYER) return MEM_RESERVE_PLAYER;
    if (prio == ARTWORK_PRIO_THUMBNAIL) return MEM_RESERVE_THUMBNAIL;
    return MEM_RESERVE_WARMER;
}

bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes) {
    if (estimated_bytes == SIZE_MAX) return false;

    size_t reserve = artwork_reserve_bytes_for_priority(prio);
    size_t available = system_get_mem_available_bytes();
    if (available < reserve) return false;
    return (available - reserve) >= estimated_bytes;
}
