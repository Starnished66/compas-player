#include "player_artwork_jobs.h"
#include "ui_frame_trace.h"
#include "player_artwork_cache.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include "board_config.h"
#include "audio.h"
#include "bt_cover_art.h"
#include "cover_decode.h"
#include "albumart.h"
#include "metadata_db.h"
#include "artwork_memory.h"
#include "artwork_priority.h"
#include "cover_raster_cache.h"
#include "db_log.h"
#include "http_client.h"
#include "player_artwork_pixels.h"
#include "ui_wake.h"
#include "frosted_glass.h"
#include "artwork_coordinator.h"
#include "player_cover_prefetch.h"
#include "albumart_info.h"
#include "library_player_art.h"
/* Weak so source-inclusion specs can link without main.c's real hook. */
extern void install_thread_crash_altstack(void) __attribute__((weak));
static bool player_cover_decode_cancelled(void * user_data);

static pthread_t cover_decode_thread;
static bool cover_decode_active = false;
static atomic_bool cover_decode_done_flag = false;
static atomic_bool cover_reload_active;
static pthread_mutex_t cover_decode_partial_mutex = PTHREAD_MUTEX_INITIALIZER;
static cover_decode_partial_t cover_decode_partial;
static int cover_decode_result_for_index;
static unsigned int cover_decode_result_generation;
static audio_current_format_info_t cover_decode_result_format;
static bool cover_decode_result_ok;
static uint16_t * cover_decode_result_pixels;
static uint8_t * cover_decode_result_reflection;
static bool cover_decode_result_flat;
static bool cover_decode_result_has_bg_color;
static uint32_t cover_decode_result_bg_color;
static int cover_decode_result_blur_radius, cover_decode_result_blur_passes;
static int cover_decode_result_darken_num, cover_decode_result_darken_den;
static uint32_t cover_decode_result_tint_rgb;
static int cover_decode_result_tint_weight;
static bool cover_decode_result_accent_valid;
static uint32_t cover_decode_result_accent;
static cover_file_id_t cover_decode_result_file;
static bool cover_decode_result_same;
#ifdef UI_PERF_TRACE
static uint64_t cover_decode_result_request_perf_us;
#endif
static bool cover_decode_pending_valid;
/* At most one superseded request is retained. A newer pending request
 * replaces it so a track change during decoding is not silently dropped. */
static cover_decode_request_t cover_decode_pending;
static atomic_uint cover_decode_generation;
/* True while the worker body can still write generated-cache files. Reload
 * waiters acquire this flag before deleting those files. */
static atomic_bool cover_decode_running;
#ifdef PLAYER_ARTWORK_JOBS_TEST
cover_file_id_t player_artwork_jobs_test_last_shown;
#endif
/* Session-scoped negative cache for load_cached_player_cover()'s slow path:
 * a track with no art anywhere (no sidecar, no embedded picture) or with
 * tags too empty for albumart_store_rgb565()'s artist+album requirement
 * can never produce a persistable cache entry, so without this,
 * gui_library_generate_player_cover()'s full fork+isolated-extract fallback
 * would silently re-run on every single play of that track -- exactly the
 * per-play cost this feature exists to eliminate. Small, bounded, path-hash
 * keyed (FNV-1a, same convention already used for cache filenames
 * elsewhere in this codebase); cleared by an app restart and after every
 * library publication, so a database update or metadata refresh retries
 * art added mid-session without per-play mtime tracking. */
/* Bumped by any library publication; the decode worker, the table's only
 * user, empties it on seeing a new value so refreshed art is retried. */
#define PLAYER_COVER_NEGATIVE_CACHE_SIZE 32
static uint64_t player_cover_negative_cache[PLAYER_COVER_NEGATIVE_CACHE_SIZE];
static int player_cover_negative_cache_next;
static atomic_uint player_cover_negative_epoch;
static unsigned player_cover_negative_seen_epoch;

/* Keep only the newest request that arrives during a decode or reload. If it
 * were dropped, the visible cover could remain stuck on an earlier track. */

static void cover_decode_partial_clear(void) {
    pthread_mutex_lock(&cover_decode_partial_mutex);
    uint16_t * pixels = cover_decode_partial.pixels;
    memset(&cover_decode_partial, 0, sizeof(cover_decode_partial));
    pthread_mutex_unlock(&cover_decode_partial_mutex);
    free(pixels);
}

static cover_decode_partial_t cover_decode_partial_take(void) {
    pthread_mutex_lock(&cover_decode_partial_mutex);
    cover_decode_partial_t result = cover_decode_partial;
    memset(&cover_decode_partial, 0, sizeof(cover_decode_partial));
    pthread_mutex_unlock(&cover_decode_partial_mutex);
    return result;
}

static void cover_decode_partial_publish(const cover_decode_request_t * req,
                                          const uint16_t * pixels,
                                          const cover_file_id_t * file,
                                          bool accent_valid, uint32_t accent) {
    if (!pixels || atomic_load_explicit(&cover_reload_active, memory_order_relaxed) ||
        player_cover_decode_cancelled((void *) (uintptr_t) req->generation)) return;
    size_t bytes = (size_t) COVER_ART_WIDTH * COVER_ART_HEIGHT * sizeof(uint16_t);
    uint16_t * copy = malloc(bytes);
    if (!copy) return; /* final result remains available as the safe fallback */
    memcpy(copy, pixels, bytes);
    pthread_mutex_lock(&cover_decode_partial_mutex);
    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed) ||
        player_cover_decode_cancelled((void *) (uintptr_t) req->generation)) {
        pthread_mutex_unlock(&cover_decode_partial_mutex);
        free(copy);
        return;
    }
    uint16_t * old = cover_decode_partial.pixels;
    cover_decode_partial = (cover_decode_partial_t) {
        .valid = true, .for_index = req->for_index, .generation = req->generation,
        .pixels = copy, .file = *file, .accent_valid = accent_valid, .accent = accent
#ifdef UI_PERF_TRACE
        , .request_perf_us = req->request_perf_us
#endif
    };
    pthread_mutex_unlock(&cover_decode_partial_mutex);
    free(old);
    ui_wake_notify();
}


/* Cover art decoding (cover_decode_to_rgb565()) and reflection blur
 * computation (compute_reflection_bytes()) are offloaded to a background
 * pthread to avoid blocking the UI thread. Frost parameters (radius,
 * passes, darken ratio, or flat styling) are snapshotted on the UI thread
 * into each request before launching the worker thread, avoiding data races
 * against live player_layout_config modifications. launch_cover_decode_req()
 * spawns the worker thread, and poll_cover_decode() (called periodically
 * from update_timer_cb) applies the decoded RGB565 buffers or flat styling
 * to the image widgets once ready. */


#define EXTERNAL_COVER_MAX_BYTES (4U * 1024U * 1024U)
#define COVER_DECODE_THREAD_STACK_SIZE (256 * 1024)

static uint64_t player_cover_path_hash(const char * path) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (const unsigned char * p = (const unsigned char *) path; *p; p++)
        hash = (hash ^ *p) * UINT64_C(1099511628211);
    return hash;
}

static bool player_cover_decode_cancelled(void * user_data) {
    unsigned int generation = (unsigned int) (uintptr_t) user_data;
    return atomic_load_explicit(&cover_decode_generation, memory_order_relaxed) != generation;
}

static bool player_cover_negative_cache_hit(const char * path) {
    unsigned epoch = atomic_load_explicit(&player_cover_negative_epoch, memory_order_relaxed);
    if (epoch != player_cover_negative_seen_epoch) {
        memset(player_cover_negative_cache, 0, sizeof(player_cover_negative_cache));
        player_cover_negative_cache_next = 0;
        player_cover_negative_seen_epoch = epoch;
    }
    uint64_t h = player_cover_path_hash(path);
    for (int i = 0; i < PLAYER_COVER_NEGATIVE_CACHE_SIZE; i++)
        if (player_cover_negative_cache[i] == h) return true;
    return false;
}

static void player_cover_negative_cache_add(const char * path) {
    player_cover_negative_cache[player_cover_negative_cache_next] = player_cover_path_hash(path);
    player_cover_negative_cache_next = (player_cover_negative_cache_next + 1) % PLAYER_COVER_NEGATIVE_CACHE_SIZE;
}

/* Local playback uses the generated 480px BMP. Never decode the original
 * embedded/sidecar image directly here: cache generation owns that
 * expensive work -- normally the background warmer, or, for a track it
 * hasn't reached yet, the one-time-per-session fallback below. */
/* The generated player-size cache for these tags. require_fresh compares its
 * stored source mtime with this track's; a reload's fallback skips that, as
 * the file was just rebuilt, possibly from a sibling track with another mtime. */
/* shown is the file already on screen: when it is this same file, nothing
 * is decoded and *out_same is set instead of *out_pixels. The file is
 * identified before it is read, so a rebuild racing the read can only cause
 * an extra decode later, never a stale cover. */
/* Generated file identity, independent of the queue slot and layout.
 * Sharing its JPEG keeps same-album changes on the existing pixel fast path. */
static uint64_t bt_cover_file_key(const cover_file_id_t * id) {
    if (!id->valid) return 0;
    uint64_t values[] = { (uint64_t) id->dev, (uint64_t) id->ino,
                          (uint64_t) id->mtime_sec, (uint64_t) id->mtime_nsec,
                          (uint64_t) id->ctime_sec, (uint64_t) id->ctime_nsec,
                          (uint64_t) id->size };
    uint64_t key = 14695981039346656037ULL;
    for (size_t i = 0; i < sizeof(values); i++) {
        key ^= ((const unsigned char *) values)[i];
        key *= 1099511628211ULL;
    }
    return key ? key : 1;
}


static bool load_generated_player_cover(const albumart_info_t * info, const char * track_path,
                                        unsigned int generation, uint64_t bt_art_token, bool require_fresh,
                                        const cover_file_id_t * shown, cover_file_id_t * out_id,
                                        bool * out_same, uint16_t ** out_pixels) {
    char found[PATH_MAX];
    bool have = require_fresh
        ? albumart_generated_cache_fresh(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                         found, sizeof(found))
        : albumart_generated_cache_find(info, ALBUMART_PLAYER_CACHE_SIZE, ALBUMART_PLAYER_CACHE_SIZE,
                                        found, sizeof(found));
    if (!have) return false;
    struct stat st = {0};
    cover_file_id_t id = { .valid = false };
    if (stat(found, &st) == 0) {
        id = (cover_file_id_t) { .valid = true, .dev = st.st_dev, .ino = st.st_ino,
            .mtime_sec = st.st_mtim.tv_sec, .mtime_nsec = st.st_mtim.tv_nsec,
            .ctime_sec = st.st_ctim.tv_sec, .ctime_nsec = st.st_ctim.tv_nsec, .size = st.st_size };
    }
    if (shown && cover_file_id_same(&id, shown)) {
        /* The displayed pixels do not depend on Bluetooth's JPEG cache.
         * Restore its optional publication from a retained raster when
         * available, without decoding/blurring the visible cover again. */
        if (!bt_cover_art_reuse(bt_art_token, bt_cover_file_key(&id)) &&
            system_get_mem_available_bytes() >= player_raster_cache_memory_floor()) {
            cover_raster_key_t shown_key = { .file = st, .width = COVER_ART_WIDTH, .height = COVER_ART_HEIGHT };
            uint16_t *retained = cover_raster_cache_get(&shown_key);
            if (retained) {
                bt_cover_art_publish_rgb565_async(bt_art_token, retained, COVER_ART_WIDTH, COVER_ART_HEIGHT,
                                           bt_cover_file_key(&id));
                free(retained);
            }
        }
        *out_id = id;
        *out_same = true;
        DB_LOG("ART_PLAYER", "cache_same path=%s file=%s", track_path, found);
        return true;
    }
    const char *cache_flag = getenv("COMPAS_ARTWORK_REUSE");
    bool reuse = (!cache_flag || strcmp(cache_flag, "0") != 0) && id.valid;
    cover_raster_key_t key = { .file = st, .width = COVER_ART_WIDTH, .height = COVER_ART_HEIGHT };
    if (reuse && system_get_mem_available_bytes() < player_raster_cache_memory_floor()) {
        player_cover_prefetch_cancel();
        cover_raster_cache_clear();
        reuse = false;
    }
    if (reuse) {
        *out_pixels = cover_raster_cache_get(&key);
        if (*out_pixels) {
            *out_id = id;
            DB_LOG("ART_PLAYER", "raster_reuse path=%s bytes=%zu", track_path, cover_raster_cache_bytes());
            return true;
        }
    }
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(found, &data, &size, EXTERNAL_COVER_MAX_BYTES,
                              ARTWORK_PRIO_PLAYER) != ALBUMART_LOAD_OK)
        return false;
    cover_decode_result_t result = cover_decode_to_rgb565_ex(data, size,
        COVER_ART_WIDTH, COVER_ART_HEIGHT, ARTWORK_PRIO_PLAYER,
        player_cover_decode_cancelled, (void *) (uintptr_t) generation, out_pixels);
    free(data);
    if (result != COVER_DECODE_OK) return false;
    /* A replaced cache file may race the read. Only retain a raster if its
     * complete identity still matches after decoding. */
    struct stat after;
    if (reuse && stat(found, &after) == 0 &&
        st.st_dev == after.st_dev && st.st_ino == after.st_ino && st.st_size == after.st_size &&
        st.st_mtim.tv_sec == after.st_mtim.tv_sec && st.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        st.st_ctim.tv_sec == after.st_ctim.tv_sec && st.st_ctim.tv_nsec == after.st_ctim.tv_nsec &&
        system_get_mem_available_bytes() >= player_raster_cache_memory_floor())
        cover_raster_cache_put(&key, *out_pixels);
    *out_id = id;
    DB_LOG("ART_PLAYER", "cache_hit path=%s file=%s", track_path, found);
    return true;
}

static bool load_cached_player_cover(const char * track_path, const char * artist, const char * album,
                                      const char * album_artist, unsigned int generation,
                                      uint64_t bt_art_token, bool force_source, const cover_file_id_t * shown,
                                      cover_file_id_t * out_id, bool * out_same, uint16_t ** out_pixels) {
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    albumart_info_t info;
    albumart_info_from_path_tags(track_path, artist, album, album_artist, &info);
    if (!force_source && load_generated_player_cover(&info, track_path, generation, bt_art_token, true,
                                                     shown, out_id, out_same, out_pixels))
        return true;

    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    if (!force_source && player_cover_negative_cache_hit(track_path)) {
        DB_LOG("ART_PLAYER", "no_art_known path=%s", track_path);
        return false;
    }

    /* Cache miss -- this track's album hasn't been warmed yet (freshly
     * added, or the background scan just hasn't reached it). Generate and
     * persist the cache now so every later play of this track hits the
     * fast path above. */
    bool no_art_confirmed = false;
    if (gui_library_generate_player_cover(track_path, artist, album, album_artist,
                                          player_cover_decode_cancelled,
                                          (void *) (uintptr_t) generation,
                                          out_pixels, &no_art_confirmed)) {
        DB_LOG("ART_PLAYER", "cache_miss_generated path=%s", track_path);
        return true;
    }
    if (player_cover_decode_cancelled((void *) (uintptr_t) generation)) return false;
    /* A forced re-read prefers the track's own art, but a track without any
     * may still have album art the warmer rebuilt from a sibling track. */
    if (force_source && load_generated_player_cover(&info, track_path, generation, bt_art_token, false,
                                                    NULL, out_id, out_same, out_pixels))
        return true;
    /* Only remember a confirmed "no art anywhere" result -- a transient one
     * (coordinator busy, isolated-helper fork/timeout) must retry on the
     * next play, not get silently and permanently suppressed. */
    DB_LOG("ART_PLAYER", "cache_miss_failed path=%s confirmed=%d", track_path, no_art_confirmed);
    if (no_art_confirmed) player_cover_negative_cache_add(track_path);
    return false;
}

/* Read native FLAC STREAMINFO without opening a second decoder. This is
 * bounded I/O and avoids shared codec initialization or whole-file scans. */
static bool player_read_flac_format(const char * path, audio_current_format_info_t * out) {
    unsigned char header[42];
    FILE * f = fopen(path, "rb");
    if (!f) return false;
    size_t n = fread(header, 1, sizeof(header), f);
    fclose(f);
    if (n != sizeof(header) || memcmp(header, "fLaC", 4) != 0 ||
        (header[4] & 0x7f) != 0 || header[5] != 0 || header[6] != 0 || header[7] != 34)
        return false;
    /* STREAMINFO bytes 10..13 pack rate (20 bits), channels (3), depth (5). */
    unsigned int rate = ((unsigned int) header[18] << 12) |
                        ((unsigned int) header[19] << 4) | (header[20] >> 4);
    unsigned int depth = (((header[20] & 1u) << 4) | (header[21] >> 4)) + 1;
    if (!rate || depth < 4) return false;
    /* A truncated path would attach this format snapshot to the wrong file. */
    size_t path_len = strlen(path);
    if (path_len >= sizeof(out->path)) return false;
    memset(out, 0, sizeof(*out));
    out->valid = true;
    memcpy(out->path, path, path_len + 1);
    out->source_sample_rate = rate;
    out->source_bit_depth = depth;
    return true;
}

/* Player-size JPEG/PNG cover. Album thumbnails use a 4 MiB stack. */
static void * cover_decode_thread_func(void * arg) {
#ifdef __linux__
    (void) prctl(PR_SET_NAME, "cover");
#endif
    if (install_thread_crash_altstack) install_thread_crash_altstack(); /* see its own comment (main.c) */
    cover_decode_request_t * req = (cover_decode_request_t *) arg;
    uint16_t * pixels = NULL;
#ifdef UI_PERF_TRACE
    uint32_t work_begin = (uint32_t)(ui_perf_now_us() / 1000), load_done = work_begin, bt_done = work_begin;
#endif
    bool ok = false;
    bool same = false;
    cover_file_id_t file_id = { .valid = false };

    memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
    if (req->local_track_path[0]) {
        if (!audio_get_current_format_info(&cover_decode_result_format) ||
            strcmp(cover_decode_result_format.path, req->local_track_path) != 0) {
            memset(&cover_decode_result_format, 0, sizeof(cover_decode_result_format));
            /* Restored paused tracks have no decoder yet. Probe off the UI
             * thread; never start playback merely to populate the badge. */
            player_read_flac_format(req->local_track_path, &cover_decode_result_format);
        }
    }

    if (req->stream_url[0] != '\0') {
        int status = 0;
        uint8_t * body = NULL;
        size_t body_size = 0;
        if (http_get_to_buffer(req->stream_url, req->stream_verify_tls, &status, &body, &body_size) && status == 200) {
            req->picture_data = body;
            req->picture_size = (uint32_t) body_size;
        } else {
            free(body);
            /* Left NULL/0 -- decode below no-ops, same as "no embedded art"
             * for a local file. No error surfaced beyond that; a failed
             * cover-art fetch isn't worth blocking or retrying playback
             * over. */
        }
        ok = req->picture_data && req->picture_size > 0 &&
             cover_decode_to_rgb565(req->picture_data, req->picture_size, COVER_ART_WIDTH, COVER_ART_HEIGHT, &pixels);
        free(req->picture_data);
        req->picture_data = NULL;
    } else if (req->local_track_path[0]) {
        ok = load_cached_player_cover(req->local_track_path, req->artist, req->album,
                                       req->album_artist, req->generation, req->bt_art_token, req->force_source,
                                       &req->shown_cover, &file_id, &same, &pixels);
    }

#ifdef UI_PERF_TRACE
    load_done = (uint32_t)(ui_perf_now_us() / 1000);
#endif
    bool cancelled = player_cover_decode_cancelled((void *) (uintptr_t) req->generation);
    if (cancelled) {
        free(pixels);
        pixels = NULL;
        ok = false;
        same = false;
    }

    if (ok && pixels)
        bt_cover_art_publish_rgb565_async(req->bt_art_token, pixels, COVER_ART_WIDTH, COVER_ART_HEIGHT,
                                      bt_cover_file_key(&file_id));
    else if (!ok && !cancelled)
        bt_cover_art_clear(req->bt_art_token);

#ifdef UI_PERF_TRACE
    bt_done = (uint32_t)(ui_perf_now_us() / 1000);
#endif
    uint32_t accent = 0;
    bool accent_valid = ok && pixels && player_cover_accent_from_rgb565(pixels, &accent);

    /* Publish an owned display copy now; the worker's pixels remain private
     * until reflection computation has finished. */
    if (ok && pixels && !same)
        cover_decode_partial_publish(req, pixels, &file_id, accent_valid, accent);

    uint8_t * reflection = NULL;
    unsigned int reflection_cache_epoch = player_reflection_cache_epoch();
    if (ok && pixels && !req->flat) {
        player_frost_params_t params = {
            .flat = false, .blur_radius = req->blur_radius, .blur_passes = req->blur_passes,
            .darken_num = req->darken_num, .darken_den = req->darken_den,
            .tint_rgb = req->tint_rgb, .tint_weight = req->tint_weight
        };
        reflection = player_reflection_cache_get(&file_id, &params);
        bool reflection_cache_hit = reflection != NULL;
#ifdef UI_PERF_TRACE
        if (reflection_cache_hit)
            printf("PERF frost_cache reused=1 source=worker generation=%u\n", req->generation);
#endif
        if (!reflection)
            reflection = player_compute_reflection_bytes_cancelable((const uint8_t *) pixels,
                                                                     req->blur_radius,
                                                                     req->blur_passes,
                                                                     req->darken_num,
                                                                     req->darken_den,
                                                                     req->tint_rgb,
                                                                     req->tint_weight,
                                                                     player_cover_decode_cancelled,
                                                                     (void *) (uintptr_t) req->generation);
        if (reflection && !reflection_cache_hit && reflection_cache_epoch ==
            player_reflection_cache_epoch())
            player_reflection_cache_put(&file_id, &params, reflection, reflection_cache_epoch,
                                       req->generation, player_artwork_jobs_cache_write_stale);
    }
    if (player_cover_decode_cancelled((void *) (uintptr_t) req->generation)) {
        free(pixels);
        pixels = NULL;
        free(reflection);
        reflection = NULL;
        ok = false;
        same = false;
        accent_valid = false;
        accent = 0;
    }

#ifdef UI_PERF_TRACE
    printf("PERF player_cover_work_ms index=%d load=%u bluetooth_queue=%u effects=%u total=%u flat=%d same=%d ok=%d\n",
           req->for_index, (unsigned)(load_done - work_begin), (unsigned)(bt_done - load_done),
           (unsigned)((uint32_t)(ui_perf_now_us() / 1000) - bt_done),
           (unsigned)((uint32_t)(ui_perf_now_us() / 1000) - work_begin),
           req->flat, same, ok);
#endif
    cover_decode_result_for_index = req->for_index;
    cover_decode_result_generation = req->generation;
    cover_decode_result_ok = ok;
    cover_decode_result_pixels = pixels;
    cover_decode_result_reflection = reflection;
    cover_decode_result_flat = req->flat;
    cover_decode_result_has_bg_color = req->has_bg_color;
    cover_decode_result_bg_color = req->bg_color;
    cover_decode_result_blur_radius = req->blur_radius;
    cover_decode_result_blur_passes = req->blur_passes;
    cover_decode_result_darken_num = req->darken_num;
    cover_decode_result_darken_den = req->darken_den;
    cover_decode_result_tint_rgb = req->tint_rgb;
    cover_decode_result_tint_weight = req->tint_weight;
    cover_decode_result_accent_valid = accent_valid;
    cover_decode_result_accent = accent;
    cover_decode_result_file = file_id;
    cover_decode_result_same = same;
#ifdef UI_PERF_TRACE
    cover_decode_result_request_perf_us = req->request_perf_us;
#endif
    free(req);
    atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    atomic_store_explicit(&cover_decode_done_flag, true, memory_order_release);
    ui_wake_notify(); /* written last -- poll_cover_decode() only checks this flag */
    return NULL;
}


static void cover_decode_start_request(cover_decode_request_t r, const cover_file_id_t * shown_file) {
    if (atomic_load_explicit(&cover_reload_active, memory_order_relaxed)) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending.shown_cover = (cover_file_id_t) { .valid = false };
        cover_decode_pending_valid = true;
        return;
    }
    if (cover_decode_active) {
        free(cover_decode_pending.picture_data);
        cover_decode_pending = r;
        cover_decode_pending.shown_cover = (cover_file_id_t) { .valid = false };
        cover_decode_pending_valid = true;
        return;
    }
    cover_decode_request_t * req = malloc(sizeof(*req));
    if (!req) {
        free(r.picture_data);
        return;
    }
    *req = r;
    /* Snapshot only now: pending requests may start after the UI applied another result. */
    req->shown_cover = shown_file ? *shown_file : (cover_file_id_t) { .valid = false };
#ifdef PLAYER_ARTWORK_JOBS_TEST
    player_artwork_jobs_test_last_shown = req->shown_cover;
#endif
    atomic_store_explicit(&cover_decode_done_flag, false, memory_order_relaxed);
    cover_decode_active = true;
    /* Publish busy before the thread exists, so reload cannot race an unscheduled writer. */
    atomic_store_explicit(&cover_decode_running, true, memory_order_release);
    pthread_attr_t attr;
    pthread_attr_t * attr_ptr = NULL;
    bool attr_initialized = pthread_attr_init(&attr) == 0;
    if (attr_initialized && pthread_attr_setstacksize(&attr, COVER_DECODE_THREAD_STACK_SIZE) == 0)
        attr_ptr = &attr;
    int create_rc = pthread_create(&cover_decode_thread, attr_ptr, cover_decode_thread_func, req);
    if (attr_initialized) pthread_attr_destroy(&attr);
    if (create_rc != 0) {
        free(req->picture_data);
        free(req);
        cover_decode_active = false;
        atomic_store_explicit(&cover_decode_running, false, memory_order_release);
    }
}

void player_artwork_jobs_submit(cover_decode_request_t request, const cover_file_id_t * shown_file) {
    cover_decode_start_request(request, shown_file);
}

void player_artwork_jobs_park_retry(cover_decode_request_t request) {
    free(cover_decode_pending.picture_data);
    cover_decode_pending = request;
    cover_decode_pending.shown_cover = (cover_file_id_t) { .valid = false };
    cover_decode_pending_valid = true;
}


bool player_artwork_jobs_take_pending(cover_decode_request_t * request) {
    if (!request || !cover_decode_pending_valid) return false;
    *request=cover_decode_pending;
    memset(&cover_decode_pending,0,sizeof(cover_decode_pending));
    cover_decode_pending_valid=false;
    return true;
}

bool player_artwork_jobs_active(void) { return cover_decode_active; }
bool player_artwork_jobs_pending(void) { return cover_decode_pending_valid; }
bool player_artwork_jobs_done(void) { return atomic_load_explicit(&cover_decode_done_flag, memory_order_acquire); }
bool player_artwork_jobs_running_idle(void) { return !atomic_load_explicit(&cover_decode_running, memory_order_acquire); }
bool player_artwork_jobs_reload_active(void) { return atomic_load_explicit(&cover_reload_active, memory_order_relaxed); }
unsigned int player_artwork_jobs_generation(void) { return atomic_load_explicit(&cover_decode_generation, memory_order_relaxed); }
unsigned int player_artwork_jobs_advance_generation(void) { return atomic_fetch_add_explicit(&cover_decode_generation, 1u, memory_order_relaxed) + 1u; }
void player_artwork_jobs_clear_pending(void) {
    if (cover_decode_pending_valid) free(cover_decode_pending.picture_data);
    memset(&cover_decode_pending, 0, sizeof(cover_decode_pending)); cover_decode_pending_valid = false;
}
void player_artwork_jobs_partial_clear(void) { cover_decode_partial_clear(); }
cover_decode_partial_t player_artwork_jobs_take_partial(void) {
    return cover_decode_partial_take();
}
bool player_artwork_jobs_take_result(player_artwork_result_t * result) {
    if (!result || !atomic_load_explicit(&cover_decode_done_flag,memory_order_acquire)) return false;
    cover_decode_active=false;
    pthread_join(cover_decode_thread,NULL);
    *result=(player_artwork_result_t){.for_index=cover_decode_result_for_index,.generation=cover_decode_result_generation,
        .format=cover_decode_result_format,.ok=cover_decode_result_ok,.pixels=cover_decode_result_pixels,
        .reflection=cover_decode_result_reflection,.flat=cover_decode_result_flat,
        .has_bg_color=cover_decode_result_has_bg_color,.bg_color=cover_decode_result_bg_color,
        .blur_radius=cover_decode_result_blur_radius,.blur_passes=cover_decode_result_blur_passes,
        .darken_num=cover_decode_result_darken_num,.darken_den=cover_decode_result_darken_den,
        .tint_rgb=cover_decode_result_tint_rgb,.tint_weight=cover_decode_result_tint_weight,
        .accent_valid=cover_decode_result_accent_valid,.accent=cover_decode_result_accent,
        .file=cover_decode_result_file,.same=cover_decode_result_same
#ifdef UI_PERF_TRACE
        ,.request_perf_us=cover_decode_result_request_perf_us
#endif
    };
    cover_decode_result_pixels=NULL; cover_decode_result_reflection=NULL;
    return true;
}
void player_artwork_jobs_begin_reload(void) { atomic_store_explicit(&cover_reload_active,true,memory_order_relaxed); }
void player_artwork_jobs_end_reload(void) { atomic_store_explicit(&cover_reload_active,false,memory_order_relaxed); }
void player_artwork_jobs_cancel_worker(void) {
    if (cover_decode_active) {
        pthread_join(cover_decode_thread,NULL); cover_decode_active=false; cover_decode_partial_clear();
        free(cover_decode_result_pixels); free(cover_decode_result_reflection);
        cover_decode_result_pixels=NULL; cover_decode_result_reflection=NULL;
        atomic_store_explicit(&cover_decode_done_flag,false,memory_order_relaxed);
    }
}
void player_artwork_jobs_invalidate_negatives(void) { atomic_fetch_add_explicit(&player_cover_negative_epoch,1u,memory_order_relaxed); }
bool player_artwork_jobs_cache_write_stale(unsigned int generation) {
    return player_cover_decode_cancelled((void *)(uintptr_t)generation) ||
           atomic_load_explicit(&cover_reload_active,memory_order_relaxed);
}
