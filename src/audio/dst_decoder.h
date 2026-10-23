/* Stereo Direct Stream Transfer (DST) frame decoder.
 * Adapted from FFmpeg dstdec.c, Copyright (c) 2014 Peter Ross.
 * SPDX-License-Identifier: LGPL-2.1-or-later
 */
#ifndef COMPAS_DST_DECODER_H
#define COMPAS_DST_DECODER_H
#include <stddef.h>
#include <stdint.h>

#define DST_STEREO_CHANNEL_BYTES 4704u
#define DST_STEREO_FRAME_BYTES (DST_STEREO_CHANNEL_BYTES * 2u)
typedef struct dst_decoder dst_decoder_t;
dst_decoder_t *dst_decoder_create(void);
void dst_decoder_destroy(dst_decoder_t *decoder);
/* Decode one stereo DST frame to interleaved MSB-first bytes (L,R,...).
 * Returns DST_STEREO_FRAME_BYTES on success, -1 on invalid/truncated input. */
int dst_decoder_decode(dst_decoder_t *decoder, const uint8_t *input,
                       size_t input_size,
                       uint8_t output[DST_STEREO_FRAME_BYTES]);
#endif
