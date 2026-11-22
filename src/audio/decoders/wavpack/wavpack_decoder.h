#ifndef WAVPACK_DECODER_H
#define WAVPACK_DECODER_H

#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include "../../engine/decoder_result.h"

typedef struct wavpack_decoder wavpack_decoder_t;
typedef struct WavpackContext WavpackContext;
typedef struct wavpack_stream wavpack_stream_t;

/* One FILE-based open shared by PCM and DSD WavPack, so both can read through
 * the engine's read-ahead. Takes ownership of f (closed on failure). With
 * OPEN_WVC and a path, a plain "<path>c" correction file is used if present. */
WavpackContext *wavpack_open_stream_context(FILE *f, int64_t size, const char *path, int flags,
                                            char *error, wavpack_stream_t **out);
/* Closes the context and every FILE the stream owns. */
void wavpack_close_stream_context(WavpackContext *ctx, wavpack_stream_t *stream);

wavpack_decoder_t *wavpack_open_stream(FILE *f, int64_t size, const char *path);
unsigned int wavpack_get_channels(const wavpack_decoder_t *dec);
unsigned int wavpack_get_sample_rate(const wavpack_decoder_t *dec);
unsigned int wavpack_get_bits_per_sample(const wavpack_decoder_t *dec);
uint64_t wavpack_get_total_pcm_frame_count(const wavpack_decoder_t *dec);
decoder_read_result_t wavpack_read_pcm_frames_s16(wavpack_decoder_t *dec, uint64_t frames, int16_t *out);
/* S32 samples are right-justified signed 24-bit values, matching S24_LE. */
decoder_read_result_t wavpack_read_pcm_frames_s32(wavpack_decoder_t *dec, uint64_t frames, int32_t *out);
bool wavpack_seek_to_pcm_frame(wavpack_decoder_t *dec, uint64_t frame);
void wavpack_close(wavpack_decoder_t *dec);

typedef void (*wavpack_tag_callback_t)(void *user, const char *key, const char *value);
void wavpack_get_tags(const char *path, wavpack_tag_callback_t callback, void *user);
/* Caller frees the returned image. Oversized or absent covers return NULL. */
unsigned char *wavpack_get_cover(const char *path, size_t max_size, size_t *out_size);

#endif
