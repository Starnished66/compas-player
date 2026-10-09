#ifndef APE_DEMUX_H
#define APE_DEMUX_H

#include <stdbool.h>
#include <stdint.h>

/* Monkey's Audio container/index reader for file versions 3.80 through 3.99,
 * modeled after FFmpeg's libavformat/ape.c (LGPL-2.1+). */

typedef struct ape_demux ape_demux_t;

ape_demux_t * ape_demux_open(const char * path);

unsigned int ape_demux_get_channels(const ape_demux_t * d);
unsigned int ape_demux_get_sample_rate(const ape_demux_t * d);
unsigned int ape_demux_get_bits_per_sample(const ape_demux_t * d);
int ape_demux_get_fileversion(const ape_demux_t * d);
int ape_demux_get_compression_level(const ape_demux_t * d);
uint64_t ape_demux_get_total_samples(const ape_demux_t * d);

uint32_t ape_demux_get_frame_count(const ape_demux_t * d);
uint32_t ape_demux_get_blocks_per_frame(const ape_demux_t * d);
uint32_t ape_demux_get_final_frame_blocks(const ape_demux_t * d);
uint32_t ape_demux_get_frame_size(const ape_demux_t * d, uint32_t frame_index);
bool ape_demux_read_frame_at(ape_demux_t * d, uint32_t frame_index, uint32_t relative_offset,
                             uint8_t * buffer, uint32_t bytes, uint32_t * out_bytes);

/* Reads frame_index's compressed bytes (already including its 0-3 byte skip
 * padding at the front, matching the on-disk layout) into buf. out_size is
 * always a multiple of 4 (the format requires 32-bit-word-aligned frame
 * data). Returns false if the index is out of range or doesn't fit buf_size. */
bool ape_demux_read_frame(ape_demux_t * d, uint32_t frame_index, uint8_t * buf, uint32_t buf_size, uint32_t * out_size);

/* Number of bytes at the front of the frame's data (after the above read)
 * that must be skipped before range-decoding starts -- word-alignment
 * padding introduced by where the frame happens to fall in the file. */
uint32_t ape_demux_get_frame_skip(const ape_demux_t * d, uint32_t frame_index);

void ape_demux_close(ape_demux_t * d);

#endif /* APE_DEMUX_H */
