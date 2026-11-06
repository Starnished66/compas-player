#include "gui_album_cover.h"
#include "../../assets/gui_image_dsc.h"
#include "../../../library/artwork/decode/library_artwork_internal.h"
#include "../../../library/artwork/albumart/albumart.h"
#include "../../../library/artwork/albumart/albumart_info.h"
#include "../../../library/artwork/decode/cover_decode.h"
#include "../../../library/artwork/support/library_player_art.h"
#include "../../../library/artwork/coordinator/artwork_coordinator.h"
#include "../../../library/metadata/db/metadata_db.h"
#include "../../../core/process/worker_thread.h"
#include "../../../core/ui/display/ui_wake.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>

extern void install_thread_crash_altstack(void) __attribute__((weak));

#define COVER_JOB_STACK (512 * 1024)
#define COVER_SOURCE_MAX_BYTES (4U * 1024U * 1024U)

typedef struct {
    pthread_t thread;
    atomic_bool cancel, done;
    int64_t song_id;
    int px;
    uint16_t * pixels;
} job_t;

static job_t * job;
static int64_t want_id, want_px;          /* what the page asked for last */
static int64_t ready_id, failed_id;
static uint16_t * ready_pixels;
static lv_image_dsc_t ready_dsc;

static bool job_cancelled(void * user) { return atomic_load_explicit(&((job_t *) user)->cancel, memory_order_relaxed); }

uint16_t * gui_album_cover_decode(int64_t song_id, int px, bool allow_generate, bool (*cancelled)(void *), void * user) {
    uint16_t * pixels = NULL;
    song_row_t row;
    if (!metadata_db_get_song_by_id(song_id, &row)) return NULL;
    albumart_info_t info;
    albumart_info_from_song_row(&row, &info);
    char found[PATH_MAX];
    /* The 480 px cache the warmer builds is the sharp source. */
    if (albumart_generated_cache_fresh(&info, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX,
                                       found, sizeof(found))) {
        uint8_t * data = NULL;
        uint32_t size = 0;
        if (albumart_load_file_ex(found, &data, &size, COVER_SOURCE_MAX_BYTES,
                                  ARTWORK_PRIO_THUMBNAIL) == ALBUMART_LOAD_OK) {
            if (cover_decode_to_rgb565_ex(data, size, px, px, ARTWORK_PRIO_THUMBNAIL,
                                          cancelled, user, &pixels) != COVER_DECODE_OK) {
                free(pixels);
                pixels = NULL;
            }
            free(data);
        }
    }
    if (!pixels && allow_generate && !(cancelled && cancelled(user))) {
        uint16_t * large = NULL;
        bool no_art = false;
        if (gui_library_generate_player_cover(row.path, row.tags.artist, row.tags.album,
                                              row.tags.album_artist, ALBUM_PLAYER_CACHE_PX, cancelled, user,
                                              &large, &no_art) && large) {
            pixels = cover_resize_rgb565(large, ALBUM_PLAYER_CACHE_PX, ALBUM_PLAYER_CACHE_PX, px, px);
            free(large);
        }
    }
    return pixels;
}

static void * job_main(void * arg) {
    job_t * j = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    if (install_thread_crash_altstack) install_thread_crash_altstack();
    j->pixels = gui_album_cover_decode(j->song_id, j->px, true, job_cancelled, j);
    atomic_store_explicit(&j->done, true, memory_order_release);
    ui_wake_notify(); /* the page swaps the sharp cover in on the next loop pass */
    return NULL;
}

static void drop_ready(void) {
    free(ready_pixels);
    ready_pixels = NULL;
    ready_id = 0;
}

static void start_job(void) {
    job_t * j = calloc(1, sizeof(*j));
    if (!j) return;
    j->song_id = want_id;
    j->px = (int) want_px;
    if (worker_thread_create(&j->thread, COVER_JOB_STACK, job_main, j) != 0) { free(j); failed_id = want_id; }
    else job = j;
}

void gui_album_cover_request(int64_t song_id, int px) {
    if (song_id <= 0 || px <= 0) return;
    if (ready_id && ready_id != song_id) drop_ready();
    want_id = song_id;
    want_px = px;
    if (job && job->song_id != song_id) atomic_store_explicit(&job->cancel, true, memory_order_relaxed);
    /* Start decoding now rather than at the next housekeeping poll. */
    if (!job && want_id != ready_id && want_id != failed_id) start_job();
}

bool gui_album_cover_busy(void) { return job != NULL; }

bool gui_album_cover_poll(bool start_allowed) {
    bool became_ready = false;
    if (job && atomic_load_explicit(&job->done, memory_order_acquire)) {
        pthread_join(job->thread, NULL);
        bool usable = job->pixels && !atomic_load_explicit(&job->cancel, memory_order_relaxed) &&
                      job->song_id == want_id;
        if (usable) {
            drop_ready();
            ready_pixels = job->pixels;
            job->pixels = NULL;
            ready_id = job->song_id;
            gui_image_dsc_init_rgb565(&ready_dsc, ready_pixels, job->px);
            became_ready = true;
        } else if (!job->pixels && job->song_id == want_id && !atomic_load(&job->cancel)) {
            failed_id = job->song_id; /* do not retry until another album is shown */
        }
        free(job->pixels);
        free(job);
        job = NULL;
    }
    if (!job && start_allowed && want_id > 0 && want_id != ready_id && want_id != failed_id) start_job();
    return became_ready;
}

const lv_image_dsc_t * gui_album_cover_get(int64_t song_id) {
    return ready_pixels && ready_id == song_id ? &ready_dsc : NULL;
}

void gui_album_cover_release(void) {
    if (job) atomic_store_explicit(&job->cancel, true, memory_order_relaxed);
    want_id = 0;
    drop_ready();
    failed_id = 0;
}

void gui_album_cover_shutdown(void) {
    if (job) {
        atomic_store_explicit(&job->cancel, true, memory_order_relaxed);
        pthread_join(job->thread, NULL);
        free(job->pixels);
        free(job);
        job = NULL;
    }
    gui_album_cover_release();
}
