#ifndef PLAYER_ARTWORK_JOBS_H
#define PLAYER_ARTWORK_JOBS_H
#include <stdbool.h>
#include <stdint.h>
#include <limits.h>
#include "../../../audio/engine/audio.h"
#include "player_artwork_cache.h"

/* A stream_url selects the streamed-art path; otherwise local_track_path is
 * decoded. picture_data is an optional owned stream body and transfers with
 * the request. shown_cover is captured only when a request actually launches. */
typedef struct {
    int for_index;
    unsigned int generation;
#ifdef UI_PERF_TRACE
    uint64_t request_perf_us;
#endif
    uint64_t bt_art_token;
    bool force_source;
    cover_file_id_t shown_cover;
    uint8_t * picture_data;
    uint32_t picture_size;
    char stream_url[1536];
    bool stream_verify_tls;
    char local_track_path[PATH_MAX];
    char artist[128];
    char album[128];
    char album_artist[128];
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius;
    int blur_passes;
    int darken_num;
    int darken_den;
    uint32_t tint_rgb;
    int tint_weight;
} cover_decode_request_t;

/* partial.pixels transfers to the caller from take_partial. */
typedef struct {
    bool valid;
    int for_index;
    unsigned int generation;
    uint16_t * pixels;
    cover_file_id_t file;
    bool accent_valid;
    uint32_t accent;
#ifdef UI_PERF_TRACE
    uint64_t request_perf_us;
#endif
} cover_decode_partial_t;

/* Every non-NULL result buffer transfers exactly once from take_result and
 * belongs to the caller, including when ok is false. */
typedef struct {
    int for_index;
    unsigned int generation;
    audio_current_format_info_t format;
    bool ok;
    uint16_t * pixels;
    uint8_t * reflection;
    bool flat;
    bool has_bg_color;
    uint32_t bg_color;
    int blur_radius, blur_passes, darken_num, darken_den;
    uint32_t tint_rgb;
    int tint_weight;
    bool accent_valid;
    uint32_t accent;
    cover_file_id_t file;
    bool same;
#ifdef UI_PERF_TRACE
    uint64_t request_perf_us;
#endif
} player_artwork_result_t;

/* submit and park_retry take ownership of request.picture_data, including on
 * replacement or launch failure. shown_file is read synchronously only when
 * the request actually starts; parked requests carry no shown-file snapshot. */
void player_artwork_jobs_submit(cover_decode_request_t request, const cover_file_id_t * shown_file);
/* Retry path used after a same-file result mismatch; it parks without starting mid-apply. */
void player_artwork_jobs_park_retry(cover_decode_request_t request);
/* take_pending transfers request.picture_data to the caller. */
bool player_artwork_jobs_take_pending(cover_decode_request_t * request);
/* active owns UI join/result handling; running_idle is the acquire-loaded
 * worker-body completion state used by reload waiters. pending is queue state. */
bool player_artwork_jobs_active(void);
bool player_artwork_jobs_pending(void);
bool player_artwork_jobs_done(void);
bool player_artwork_jobs_running_idle(void);
bool player_artwork_jobs_reload_active(void);
unsigned int player_artwork_jobs_generation(void);
unsigned int player_artwork_jobs_advance_generation(void);
void player_artwork_jobs_clear_pending(void);
void player_artwork_jobs_partial_clear(void);
cover_decode_partial_t player_artwork_jobs_take_partial(void);
bool player_artwork_jobs_take_result(player_artwork_result_t * result);
void player_artwork_jobs_begin_reload(void);
void player_artwork_jobs_end_reload(void);
void player_artwork_jobs_cancel_worker(void);
void player_artwork_jobs_invalidate_negatives(void);
bool player_artwork_jobs_cache_write_stale(unsigned int generation);
#endif
