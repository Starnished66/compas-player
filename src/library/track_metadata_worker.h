#ifndef TRACK_METADATA_WORKER_H
#define TRACK_METADATA_WORKER_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "metadata.h"
#include "audio.h"

enum { TRACK_METADATA_CURRENT, TRACK_METADATA_NEXT, TRACK_METADATA_FAVORITE, TRACK_METADATA_LANES };
typedef struct {
    uint64_t token;
    char path[PATH_MAX];
    bool read_file;
    bool read_audio;
    uint64_t audio_generation;
    bool probe;
    track_metadata_t metadata; /* Scalar-only seed for remote catalogs. */
} track_metadata_request_t;
typedef struct {
    track_metadata_request_t request;
    bool favorite;
    bool has_probe;
    audio_current_format_info_t probe;
} track_metadata_result_t;
/* Copies scalar values; latest request wins in each lane. CURRENT and NEXT
 * have separate slots so display work cannot replace gapless preparation. */
bool track_metadata_worker_start(void);
bool track_metadata_history_failed(void);
bool track_metadata_submit(int lane, const track_metadata_request_t * request);
bool track_metadata_take(int lane, track_metadata_result_t * result);
/* Play-count events are ordered separately and never coalesced with display
 * requests. False means history could not be queued; playback is independent. */
bool track_metadata_count_play(const char * path);
#endif
