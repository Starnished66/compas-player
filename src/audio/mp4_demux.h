#ifndef MP4_DEMUX_H
#define MP4_DEMUX_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* Minimal MP4/ISO-BMFF demuxer: locates one audio track's codec config,
 * compact sample offsets/sizes and stts timeline, with validated single-edit
 * and iTunSMPB gapless metadata. No video, multi-track selection beyond the
 * first audio track, or fragmented MP4 support. */

typedef struct mp4_demux mp4_demux_t;

mp4_demux_t * mp4_demux_open(const char * path);
/* Takes ownership of a seekable FILE on success and failure. */
mp4_demux_t * mp4_demux_open_stream(FILE * stream);

/* Codec fourcc only ("alac"/"mp4a"). Opens and closes a demux; long files
 * stay compact so this is safe as an extension-dispatch peek. */
bool mp4_demux_peek_codec(const char * path, char out_fourcc[5]);
/* Like mp4_demux_peek_codec(), but reads from an already-open seekable FILE
 * and restores its original position without taking ownership. */
bool mp4_demux_peek_codec_stream(FILE * stream, char out_fourcc[5]);

/* Forward-only top-level scan. Does not take ownership of file. On success
 * *out_bytes is NULL when moov is larger than 4 MiB (the caller keeps
 * ordinary IO) or a malloc'd span of moov, extended back to offset 0 when
 * that prefix was already read without a backward seek. *out_offset is the
 * file offset of the first byte. False means no moov or a read failed, and
 * *out_bytes is NULL. */
bool mp4_cache_moov_prefix(FILE * file, uint8_t ** out_bytes, uint64_t * out_offset,
                           uint64_t * out_length);

/* 4-character codec identifier from the sample description, e.g. "alac" or
 * "mp4a" (AAC). Not null-terminated by convention, so this returns exactly
 * 4 bytes; out_fourcc must have room for 5 (this null-terminates for
 * convenience). */
void mp4_demux_get_codec_fourcc(const mp4_demux_t * d, char out_fourcc[5]);

/* Raw bytes of the codec-specific config box contents (the ALAC magic
 * cookie, or the esds descriptor payload for AAC) -- handed to the actual
 * codec decoder's init function as-is. */
const uint8_t * mp4_demux_get_codec_config(const mp4_demux_t * d, uint32_t * out_size);

uint32_t mp4_demux_get_sample_count(const mp4_demux_t * d);

/* PCM frames represented by one demuxed "sample" (i.e. one compressed
 * access unit) -- e.g. 4096 for typical ALAC, 1024 for AAC-LC. Assumes a
 * constant value throughout the track (true for the vast majority of real
 * files); the last sample may represent fewer. */
uint32_t mp4_demux_get_frames_per_sample(const mp4_demux_t * d);

/* PCM-frame duration from all stts entries, converted from the media
 * timescale at the active decoder sample rate. Validated gapless metadata
 * replaces the raw duration when present. */
uint64_t mp4_demux_get_total_pcm_frame_count(const mp4_demux_t * d);
/* Set the decoder's actual output rate (for example HE-AAC SBR can differ
 * from the sample-entry rate) before querying timeline or gapless values. */
void mp4_demux_set_pcm_sample_rate(mp4_demux_t * d, uint32_t sample_rate);

/* Timeline in decoded PCM frames. Samples are mapped using stts and the
 * media timescale, with validated encoder priming/padding removed. */
bool mp4_demux_get_gapless_trim(const mp4_demux_t * d, uint64_t * priming_frames, uint64_t * playable_frames);
bool mp4_demux_map_pcm_frame(const mp4_demux_t * d, uint64_t pcm_frame,
                             uint32_t * sample_index, uint64_t * discard_frames);
bool mp4_demux_get_sample_pcm_duration(const mp4_demux_t * d, uint32_t sample_index,
                                       uint64_t * duration_frames);

/* Reads sample_index's compressed bytes into buf (caller-provided,
 * buf_size bytes). Returns false if the index is out of range or the
 * sample doesn't fit in buf_size. */
bool mp4_demux_read_sample(mp4_demux_t * d, uint32_t sample_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size);

void mp4_demux_close(mp4_demux_t * d);

#endif /* MP4_DEMUX_H */
