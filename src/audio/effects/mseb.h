#ifndef MSEB_H
#define MSEB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MSEB_CONTROL_COUNT 10
#define MSEB_FILTER_COUNT 13

/* Signed -100..100: temperature, bass extension, bass texture, thickness,
 * vocal position, female overtones, LF/HF sibilance, impulse, air. */
typedef struct {
    bool enabled;
    int values[MSEB_CONTROL_COUNT];
    double width; /* Independent stock Sound Field matrix, 0..2, neutral 1. */
} mseb_config_t;

/* Transient plugin-owned settings. Validation is all-or-nothing. */
bool mseb_set_config(const mseb_config_t * config);
void mseb_get_config(mseb_config_t * config);
void mseb_reset(void);
void mseb_note_pcm_discontinuity(void);

/* PCM owner only. Return true when MSEB owns tone processing for this block;
 * the caller then skips manual PEQ, retaining independent crossfeed. */
bool mseb_process(int16_t * samples, size_t frames, int channels, unsigned int rate);
bool mseb_process_s32(int32_t * samples, size_t frames, int channels, unsigned int rate);

/* Read-only response of the configured filters plus stock compensation.
 * No processing history is read or changed; rate=0 selects 44100 Hz. */
double mseb_get_compensation_db(unsigned int rate);
void mseb_get_response_db(const double * frequencies, double * response,
                          size_t count, unsigned int rate);

#endif
