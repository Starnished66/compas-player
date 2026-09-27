#ifndef DSD_DECODER_H
#define DSD_DECODER_H

#include <stdbool.h>
#include <stdint.h>

/* DSF (Sony DSD Stream File) and DFF (Philips DSDIFF) container reader,
 * converting the raw 1-bit DSD bitstream to PCM with dsd_filter.h's
 * lookup-table decimator. DSD64, DSD128 and DSD256 in the 44.1 kHz and 48 kHz
 * families are supported; output is twice the base rate (88.2 or 96 kHz)
 * with the ultrasonic noise-shaping energy filtered out. DSD512 and DST
 * (compressed DFF) are rejected at open. The output rate is therefore NOT
 * the file's nominal DSD rate -- callers must use dsd_get_pcm_sample_rate(). */

#include "decoder_result.h"

typedef struct dsd_decoder dsd_decoder_t;

dsd_decoder_t * dsd_open_file(const char * path);

unsigned int dsd_get_channels(const dsd_decoder_t * dec);
unsigned int dsd_get_source_sample_rate(const dsd_decoder_t * dec);
unsigned int dsd_get_pcm_sample_rate(const dsd_decoder_t * dec);
uint64_t dsd_get_total_pcm_frame_count(const dsd_decoder_t * dec);

decoder_read_result_t dsd_read_pcm_frames_s16(dsd_decoder_t * dec, uint64_t frames_to_read, int16_t * buffer_out);
/* 24-bit samples right-justified in int32 (the S24_LE layout the other wide
 * decoders produce). */
decoder_read_result_t dsd_read_pcm_frames_s32(dsd_decoder_t * dec, uint64_t frames_to_read, int32_t * buffer_out);
bool dsd_seek_to_pcm_frame(dsd_decoder_t * dec, uint64_t frame_index);

void dsd_close(dsd_decoder_t * dec);

#endif /* DSD_DECODER_H */
