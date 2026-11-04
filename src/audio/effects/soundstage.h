#ifndef SOUNDSTAGE_H
#define SOUNDSTAGE_H

#include <stdbool.h>
#include <stddef.h>

/* Soundstage: a headphone spatial stage placed after stereo width in the PEQ
 * chain. Mid/side EQ, then early reflections, then a binaural virtual-speaker
 * head model (Brown and Duda spherical head, shuffler form). Disabled or
 * neutral, the stage is idle and the output is bit-identical. */
typedef struct {
    bool enabled;
    double speaker_angle_deg;   /* 10..60, default 30 */
    double binaural_amount;     /* 0..1, default 0 */
    double side_db;             /* -12..6, default 0 */
    double side_low_db;         /* -12..6, default 0 */
    double side_low_hz;         /* 50..500, default 150 */
    double side_high_db;        /* -12..6, default 0 */
    double side_high_hz;        /* 2000..12000, default 5000 */
    double reflections;         /* 0..1, default 0 */
    double room_size;           /* 0..1, default 0.5 */
    double damping;             /* 0..1, default 0.5 */
} soundstage_config_t;

/* Disabled and neutral. */
void soundstage_defaults(soundstage_config_t * config);
/* Transient plugin-owned settings. All-or-nothing: false (and nothing
 * changes) when any field is out of range or non-finite. */
bool soundstage_set_config(const soundstage_config_t * config);
void soundstage_get_config(soundstage_config_t * config);
/* Defaults, plus a discontinuity so no history survives. */
void soundstage_reset(void);
void soundstage_note_pcm_discontinuity(void);

/* Name, location and inclusive range of every numeric field: the one table
 * that config validation and the Lua binding both use. */
typedef struct {
    const char * name;
    size_t offset;
    double min, max;
} soundstage_field_t;
extern const soundstage_field_t soundstage_fields[];
extern const size_t soundstage_field_count;

/* PCM owner only. begin_block returns true when the stage will process this
 * block; the caller then runs soundstage_frames on interleaved stereo float
 * frames (units are the sample scale of the stream). A block with
 * channels != 2 is left alone. */
bool soundstage_begin_block(unsigned int rate, int channels, bool s32, size_t frames);
/* ceiling is the immediate stereo-linked peak limit, as for crossfeed.
 * Returns true when some frame exceeded the ceiling or was not finite (it
 * was then scaled or passed dry); false means every output is within it. */
bool soundstage_frames(float * lr, size_t frames, float ceiling);

#endif
