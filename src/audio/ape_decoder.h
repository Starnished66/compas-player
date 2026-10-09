#ifndef APE_DECODER_H
#define APE_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* FFmpeg-derived Monkey's Audio decoder, supporting file versions 3.80-3.99,
 * 8/16/24-bit mono and stereo, and all supported compression levels. */

#include "decoder_result.h"

typedef struct ape_decoder ape_decoder_t;

ape_decoder_t * ape_open_file(const char * path);

unsigned int ape_get_channels(const ape_decoder_t * dec);
unsigned int ape_get_sample_rate(const ape_decoder_t * dec);
unsigned int ape_get_bits_per_sample(const ape_decoder_t * dec);
uint64_t ape_get_total_pcm_frame_count(const ape_decoder_t * dec);

decoder_read_result_t ape_read_pcm_frames_s16(ape_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
/* Samples retain the encoded depth and are right-justified (including S24). */
decoder_read_result_t ape_read_pcm_frames_s32(ape_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out);
bool ape_seek_to_pcm_frame(ape_decoder_t * dec, uint64_t frame_index);

void ape_close(ape_decoder_t * dec);

#endif /* APE_DECODER_H */
