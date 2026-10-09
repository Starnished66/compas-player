#ifndef CAF_DECODER_H
#define CAF_DECODER_H

#include <stdbool.h>
#include <stdint.h>
#include "decoder_result.h"

typedef struct caf_decoder caf_decoder_t;
typedef void (*caf_tag_callback_t)(void *user, const char *key, const char *value);

caf_decoder_t *caf_open_file(const char *path);
unsigned int caf_get_channels(const caf_decoder_t *dec);
unsigned int caf_get_sample_rate(const caf_decoder_t *dec);
unsigned int caf_get_bits_per_sample(const caf_decoder_t *dec);
uint64_t caf_get_total_pcm_frame_count(const caf_decoder_t *dec);
bool caf_is_alac(const caf_decoder_t *dec);
decoder_read_result_t caf_read_pcm_frames_s16(caf_decoder_t *dec, uint64_t frames, int16_t *out);
/* S32 samples are right-justified signed 24-bit values, matching S24_LE. */
decoder_read_result_t caf_read_pcm_frames_s32(caf_decoder_t *dec, uint64_t frames, int32_t *out);
bool caf_seek_to_pcm_frame(caf_decoder_t *dec, uint64_t frame);
void caf_close(caf_decoder_t *dec);

/* Emits the standard CAF strings using TITLE, ARTIST, ALBUM, DATE,
 * TRACKNUMBER and GENRE keys. */
void caf_get_tags(const char *path, caf_tag_callback_t callback, void *user);

#endif
