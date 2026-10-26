#ifndef ARTWORK_MEMORY_H
#define ARTWORK_MEMORY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "artwork_priority.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ARTWORK_FORMAT_UNKNOWN = 0,
    ARTWORK_FORMAT_JPEG,
    ARTWORK_FORMAT_PNG,
    ARTWORK_FORMAT_BMP,
    ARTWORK_FORMAT_PNG_STREAMING,
    /* Progressive (SOF2) JPEG -- decoded via a separate vendored libjpeg
     * fallback (src/library/artwork/decode/cover_decode.c), never tjpgd (which rejects
     * SOF2 outright). Its memory profile is fundamentally different from
     * baseline ARTWORK_FORMAT_JPEG: dominated by a coefficient buffer that
     * scales with NATIVE image dimensions, not the requested output size --
     * see artwork_estimate_decode_bytes()'s progressive_coeff_bytes
     * parameter. */
    ARTWORK_FORMAT_JPEG_PROGRESSIVE,
    /* Any baseline (SOF0) JPEG decoded via the libjpeg fallback instead of
     * tjpgd: either its chroma sampling factors fail tjpgd's own minimal
     * whitelist (e.g. vertical-only/4:4:0 subsampling -- valid JPEG, just
     * rare), or tjpgd's own jd_prepare()/jd_decomp() failed on it despite
     * looking like an ordinary baseline file. Shares PROGRESSIVE's larger
     * libjpeg workspace allowance AND (unlike an earlier version of this
     * format) DOES carry a real progressive_coeff_bytes-shaped cost:
     * jpeg_probe() now computes that estimate unconditionally for every
     * supported SOF0/SOF2, not just progressive, because a cheap header
     * probe can't tell single-scan and sequential-multiscan SOF0 apart --
     * see artwork_estimate_decode_bytes(). Passing 0 here for this format
     * would silently reopen the exact OOM risk that estimate exists to
     * close for a genuinely multiscan baseline file. */
    ARTWORK_FORMAT_JPEG_LIBJPEG_BASELINE,
} artwork_format_t;

/* Returns current MemAvailable in bytes from /proc/meminfo (cached briefly). */
size_t system_get_mem_available_bytes(void);

/* Test hook: inject mock available memory (0 to disable mock) */
void system_set_mock_mem_available(size_t bytes);

/* Calculates estimated peak memory for a decode using format-specific overhead.
 * native_w/native_h are the POST-SCALE dimensions the decoder will actually
 * produce for ARTWORK_FORMAT_JPEG/JPEG_PROGRESSIVE (matching this JPEG
 * decode's own jpeg_scale_for_target() policy), and the true NATIVE (source)
 * dimensions for PNG/BMP, which don't scale during decode. For
 * PNG_STREAMING, pass the true native (source) dimensions too -- this
 * function derives its own post-scale size internally (mirroring
 * decode_png_streaming()'s own jpeg_scale_for_target()-based downscale,
 * extended the same way past its normal stopping point when needed to fit
 * MAX_DECODED_COVER_SIDE) since the row-buffer sizing cost genuinely
 * depends on the true native width, not the post-scale one.
 * progressive_coeff_bytes is the real coefficient-buffer size from a prior
 * jpeg_probe() call -- ignored for every format except JPEG_PROGRESSIVE and
 * JPEG_LIBJPEG_BASELINE (both libjpeg-decoded), where it's the dominant
 * cost (see jpeg_probe_t's own doc comment, cover_decode.h, for why this
 * can't be derived from native_w/native_h alone: it depends on the SOF's
 * true native dimensions and component sampling factors, not the post-scale
 * output size this function otherwise bills, and jpeg_probe() computes it
 * unconditionally for every supported SOF0/SOF2, not just progressive).
 * Pass 0 only for ARTWORK_FORMAT_JPEG (tjpgd) or a non-JPEG format.
 * png_native_bpp is the PNG's real IHDR bits-per-pixel (from
 * lodepng_get_bpp() on the inspected color mode) -- ignored for every
 * format except PNG and PNG_STREAMING, where the decoder's real transient
 * workspace (the inflated scanline buffer, live at the same time as the
 * decoded pixel buffer for PNG; the two native-row filter buffers for
 * PNG_STREAMING) scales with it directly: a 16-bit-per-channel RGBA PNG
 * (64bpp) needs a scanline row 8x wider than an 8-bit grayscale one at the
 * same pixel dimensions, not the flat "4 bytes/pixel" this used to assume
 * regardless of real bit depth. Pass 0 for every other format.
 * Returns estimated bytes, or SIZE_MAX on overflow / invalid dimensions. */
size_t artwork_estimate_decode_bytes(artwork_format_t fmt, size_t compressed_size,
                                     size_t native_w, size_t native_h,
                                     size_t target_w, size_t target_h,
                                     uint64_t progressive_coeff_bytes,
                                     uint32_t png_native_bpp);

/* Checks if MemAvailable satisfies (reserve + estimated_bytes). */
bool artwork_check_memory_admission(artwork_priority_t prio, size_t estimated_bytes);

/* The per-priority reserve artwork_check_memory_admission() itself uses --
 * exported as the single source of truth for src/library/metadata/parsing/metadata.c's own
 * child-process memory-ceiling use of the same reserve tiers. Unknown/
 * out-of-range values return the most conservative (WARMER) reserve. */
size_t artwork_reserve_bytes_for_priority(artwork_priority_t prio);

#ifdef __cplusplus
}
#endif

#endif /* ARTWORK_MEMORY_H */
