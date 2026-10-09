/*
 * Monkey's Audio decoder wrapper around the standalone FFmpeg-derived codec
 * core in ape_ffmpeg_core.c. The core is Copyright (c) 2007 Benjamin Zores,
 * based on libdemac by Dave Chapman; see APE_FFMPEG_CORE_LICENSE.txt.
 */
#include "ape_decoder.h"
#include "ape_demux.h"
#include "ape_ffmpeg_core.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

#define APE_WINDOW_BYTES (64U * 1024U)
#define APE_MAX_WHOLE_FRAME_BYTES (8U * 1024U * 1024U)
#define APE_MAX_CONSECUTIVE_ERRORS 5

static int16_t clamp_s16(int32_t v) {
    if (v > INT16_MAX) return INT16_MAX;
    if (v < INT16_MIN) return INT16_MIN;
    return (int16_t)v;
}

struct ape_decoder {
    ape_demux_t *demux;
    ape_core_t *core;
    int channels;
    int bps;
    uint64_t total_pcm_frames;
    uint32_t next_frame;
    uint32_t frame_left;
    uint32_t chunk_frames;
    uint32_t chunk_pos;
    uint32_t consecutive_errors;
    bool fatal;
    uint8_t *window;
    uint32_t window_capacity;
    uint32_t frame_index;
    uint32_t read_pos;
    uint32_t read_left;
    bool pad_pending;
    int32_t *silence;
    const int32_t *left;
    const int32_t *right;
    bool broken_frame;
};

static void swap_words(uint8_t *p, uint32_t size) {
    for (uint32_t i = 0; i + 3 < size; i += 4) {
        uint8_t b = p[i]; p[i] = p[i + 3]; p[i + 3] = b;
        b = p[i + 1]; p[i + 1] = p[i + 2]; p[i + 2] = b;
    }
}

static bool refill_window(ape_core_t *core, void *arg) {
    ape_decoder_t *d = arg;
    const uint8_t *ptr, *end;
    ape_core_window(core, &ptr, &end);
    size_t keep = (size_t)(end - ptr);
    if (keep > d->window_capacity / 2) return false;
    memmove(d->window, ptr, keep);
    size_t room = (d->window_capacity - keep - 2U) & ~(size_t)3U;
    size_t want = d->read_left < room ? d->read_left : room;
    uint32_t got = 0;
    if (want) {
        if (!ape_demux_read_frame_at(d->demux, d->frame_index, d->read_pos,
                                     d->window + keep, (uint32_t)want, &got)) got = 0;
        got &= ~3U;
        if (got < want) d->read_left = 0;
        else d->read_left -= got;
        d->read_pos += got;
        swap_words(d->window + keep, got);
    }
    size_t len = keep + got;
    bool more = got > 0;
    if (!d->read_left && d->pad_pending) {
        d->window[len++] = 0;
        d->window[len++] = 0;
        d->pad_pending = false;
        more = true;
    }
    ape_core_set_window(core, d->window, d->window + len);
    return more;
}

static bool start_frame(ape_decoder_t *d, uint32_t index) {
    uint32_t raw_size = 0;
    uint32_t capacity = ape_demux_get_frame_size(d->demux, index);
    if (!capacity) return false;
    uint32_t blocks = index + 1 == ape_demux_get_frame_count(d->demux)
        ? ape_demux_get_final_frame_blocks(d->demux)
        : ape_demux_get_blocks_per_frame(d->demux);
    d->frame_index = index;
    int rc;
    if (ape_core_whole_frames(d->core)) {
        if (capacity > APE_MAX_WHOLE_FRAME_BYTES - 8U) return false;
        uint32_t need = capacity + 8U;
        if (need > d->window_capacity) {
            free(d->window);
            d->window = malloc(need);
            d->window_capacity = d->window ? need : 0;
            if (!d->window) return false;
        }
        if (!ape_demux_read_frame(d->demux, index, d->window, d->window_capacity, &raw_size) || raw_size < 8)
            return false;
        swap_words(d->window, raw_size & ~3U);
        size_t len = raw_size & ~3U;
        if (ape_demux_get_fileversion(d->demux) < 3950) { d->window[len++] = 0; d->window[len++] = 0; }
        rc = ape_core_start_frame(d->core, d->window, d->window + len,
                                  ape_demux_get_frame_skip(d->demux, index), blocks, NULL, NULL);
    } else {
        if (d->window_capacity != APE_WINDOW_BYTES) {
            uint8_t *grown = realloc(d->window, APE_WINDOW_BYTES);
            if (!grown) return false;
            d->window = grown;
            d->window_capacity = APE_WINDOW_BYTES;
        }
        d->read_pos = 0;
        d->read_left = capacity;
        d->pad_pending = ape_demux_get_fileversion(d->demux) < 3950;
        ape_core_set_window(d->core, d->window, d->window);
        rc = ape_core_start_frame(d->core, d->window, d->window,
                                  ape_demux_get_frame_skip(d->demux, index), blocks,
                                  refill_window, d);
    }
    if (rc < 0) return false;
    d->next_frame = index + 1;
    d->frame_left = blocks;
    d->broken_frame = false;
    return true;
}

static bool next_chunk(ape_decoder_t *d) {
    if (d->fatal) return false;
    if (!d->frame_left) {
        if (d->next_frame >= ape_demux_get_frame_count(d->demux)) return false;
        uint32_t index = d->next_frame;
        if (!start_frame(d, index)) {
            d->consecutive_errors++;
            if (d->consecutive_errors >= APE_MAX_CONSECUTIVE_ERRORS) {
                d->fatal = true;
                return false;
            }
            d->next_frame = index + 1;
            d->frame_left = index + 1 == ape_demux_get_frame_count(d->demux)
                ? ape_demux_get_final_frame_blocks(d->demux)
                : ape_demux_get_blocks_per_frame(d->demux);
            d->left = d->right = d->silence;
            d->broken_frame = true;
            d->chunk_frames = d->frame_left < APE_CORE_CHUNK ? d->frame_left : APE_CORE_CHUNK;
            d->frame_left -= d->chunk_frames;
            d->chunk_pos = 0;
            return true;
        }
    }

    bool whole = ape_core_whole_frames(d->core) && !d->broken_frame;
    uint32_t count = whole ? d->frame_left
                           : (d->frame_left < APE_CORE_CHUNK ? d->frame_left : APE_CORE_CHUNK);
    if (d->broken_frame || ape_core_decode(d->core, (int)count, &d->left, &d->right) < 0) {
        if (!d->broken_frame) {
            d->consecutive_errors++;
            if (d->consecutive_errors >= APE_MAX_CONSECUTIVE_ERRORS) {
                d->fatal = true;
                return false;
            }
        }
        d->left = d->right = d->silence;
        d->broken_frame = true;
        if (count > APE_CORE_CHUNK) count = APE_CORE_CHUNK;
    } else {
        d->consecutive_errors = 0;
    }
    d->chunk_frames = count;
    d->chunk_pos = 0;
    d->frame_left -= count;
    return true;
}

ape_decoder_t *ape_open_file(const char *path) {
    ape_demux_t *demux = ape_demux_open(path);
    if (!demux) return NULL;
    int version = ape_demux_get_fileversion(demux);
    int compression = ape_demux_get_compression_level(demux);
    int channels = (int)ape_demux_get_channels(demux);
    int bps = (int)ape_demux_get_bits_per_sample(demux);
    ape_decoder_t *d = calloc(1, sizeof(*d));
    if (!d) { ape_demux_close(demux); return NULL; }
    d->demux = demux;
    d->channels = channels;
    d->bps = bps;
    d->total_pcm_frames = ape_demux_get_total_samples(demux);
    d->window = malloc(APE_WINDOW_BYTES);
    d->window_capacity = d->window ? APE_WINDOW_BYTES : 0;
    d->silence = calloc(APE_CORE_CHUNK, sizeof(*d->silence));
    d->core = ape_core_new(version, compression, 0, bps, channels);
    if (!d->window || !d->silence || !d->core) {
        ape_close(d);
        return NULL;
    }
    return d;
}

unsigned int ape_get_channels(const ape_decoder_t *d) { return d ? (unsigned)d->channels : 0; }
unsigned int ape_get_sample_rate(const ape_decoder_t *d) { return d ? ape_demux_get_sample_rate(d->demux) : 0; }
unsigned int ape_get_bits_per_sample(const ape_decoder_t *d) { return d ? (unsigned)d->bps : 0; }
uint64_t ape_get_total_pcm_frame_count(const ape_decoder_t *d) { return d ? d->total_pcm_frames : 0; }

decoder_read_result_t ape_read_pcm_frames_s16(ape_decoder_t *d, uint64_t requested, int16_t *out) {
    decoder_read_result_t result = {0, DECODER_READ_OK};
    if (!d || (!out && requested) || requested > SIZE_MAX / ((size_t)d->channels * sizeof(*out))) {
        result.status = DECODER_READ_FATAL_ERROR; return result;
    }
    while (result.frames < requested) {
        if (d->chunk_pos == d->chunk_frames && !next_chunk(d)) {
            result.status = d->fatal ? (result.frames ? DECODER_READ_OK : DECODER_READ_FATAL_ERROR)
                                     : (result.frames ? DECODER_READ_OK : DECODER_READ_EOF);
            break;
        }
        uint64_t n = d->chunk_frames - d->chunk_pos;
        if (n > requested - result.frames) n = requested - result.frames;
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t pos = d->chunk_pos + (uint32_t)i;
            for (int c = 0; c < d->channels; ++c) {
                int32_t v = c ? d->right[pos] : d->left[pos];
                int shift = d->bps > 16 ? d->bps - 16 : 0;
                if (d->bps < 16) v *= 1 << (16 - d->bps);
                out[(result.frames + i) * d->channels + c] = clamp_s16(v >> shift);
            }
        }
        d->chunk_pos += (uint32_t)n;
        result.frames += n;
    }
    return result;
}

decoder_read_result_t ape_read_pcm_frames_s32(ape_decoder_t *d, uint64_t requested, int32_t *out) {
    decoder_read_result_t result = {0, DECODER_READ_OK};
    if (!d || (!out && requested) || requested > SIZE_MAX / ((size_t)d->channels * sizeof(*out))) {
        result.status = DECODER_READ_FATAL_ERROR; return result;
    }
    while (result.frames < requested) {
        if (d->chunk_pos == d->chunk_frames && !next_chunk(d)) {
            result.status = d->fatal ? (result.frames ? DECODER_READ_OK : DECODER_READ_FATAL_ERROR)
                                     : (result.frames ? DECODER_READ_OK : DECODER_READ_EOF);
            break;
        }
        uint64_t n = d->chunk_frames - d->chunk_pos;
        if (n > requested - result.frames) n = requested - result.frames;
        for (uint64_t i = 0; i < n; ++i) {
            uint32_t pos = d->chunk_pos + (uint32_t)i;
            for (int c = 0; c < d->channels; ++c) {
                int32_t v = c ? d->right[pos] : d->left[pos];
                out[(result.frames + i) * d->channels + c] = v;
            }
        }
        d->chunk_pos += (uint32_t)n;
        result.frames += n;
    }
    return result;
}

bool ape_seek_to_pcm_frame(ape_decoder_t *d, uint64_t frame) {
    if (!d) return false;
    if (frame > d->total_pcm_frames) frame = d->total_pcm_frames;
    d->chunk_frames = d->chunk_pos = d->frame_left = 0;
    d->consecutive_errors = 0;
    d->fatal = false;
    uint32_t blocks = ape_demux_get_blocks_per_frame(d->demux);
    uint32_t index = frame == d->total_pcm_frames ? ape_demux_get_frame_count(d->demux)
                                                  : (uint32_t)(frame / blocks);
    d->next_frame = index;
    if (index == ape_demux_get_frame_count(d->demux)) return true;
    if (!start_frame(d, index)) return false;
    uint32_t skip = (uint32_t)(frame % blocks);
    while (skip) {
        if (!next_chunk(d)) return false;
        uint32_t n = d->chunk_frames;
        if (n > skip) n = skip;
        d->chunk_pos = n;
        skip -= n;
    }
    return true;
}

void ape_close(ape_decoder_t *d) {
    if (!d) return;
    ape_core_free(d->core);
    ape_demux_close(d->demux);
    free(d->window);
    free(d->silence);
    free(d);
}
