#ifndef AUDIO_TEMPO_H
#define AUDIO_TEMPO_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
/* Audio-thread-owned streaming speech tempo. Input/output are interleaved
 * S16 PCM; all positions are in original source frames. No shared globals. */
typedef struct audio_tempo audio_tempo_t;
audio_tempo_t * audio_tempo_create(unsigned rate, unsigned channels, double speed, uint64_t source_frame);
void audio_tempo_destroy(audio_tempo_t * tempo);
bool audio_tempo_feed(audio_tempo_t * tempo, const int16_t * pcm, size_t frames);
bool audio_tempo_flush(audio_tempo_t * tempo);
size_t audio_tempo_read(audio_tempo_t * tempo, int16_t * pcm, size_t capacity);
bool audio_tempo_drained(const audio_tempo_t * tempo);
uint64_t audio_tempo_source_fed(const audio_tempo_t * tempo);
uint64_t audio_tempo_advance(audio_tempo_t * tempo, size_t delivered_frames, bool fully_delivered);
#endif
