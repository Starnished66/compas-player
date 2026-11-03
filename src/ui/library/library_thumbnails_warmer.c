#include "library_thumbnails_internal.h"
#include "../../core/ui/display/ui_wake.h"
#include "../../core/process/worker_thread.h"
#include "../../library/metadata/scan/library_scan_storage.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/decode/cover_decode.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../library/artwork/coordinator/artwork_failure_cache.h"
#include "../player/screen/gui_player.h"
#include "../../library/tagcache/core/tagcache.h"
#include "../../core/system/logging/db_log.h"
#include "../../audio/engine/audio.h"
#include "../diagnostics/ui_frame_trace.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sched.h>
#ifdef __linux__
#include <linux/sched.h>
#endif
#include <time.h>
#include <limits.h>

/* Boot-time RAM cache preload: piggybacks on the existing persistent
 * warmer thread (album_thumb_gen_thread_func) to also hand its first
 * ALBUM_THUMBNAIL_CACHE_SIZE decoded thumbnails to the main/LVGL thread
 * for insertion into album_thumbnail_cache[], instead of only writing the
 * on-disk sized BMP sidecar. Single-slot rendezvous (like the on-screen
 * lazy-decode hand-off) -- the warmer thread blocks after staging
 * one result until album_thumbnail_poll_cb() has consumed it, so at most
 * one pending buffer ever exists and the cache array is still only ever
 * written from the main thread. */
int album_thumb_gen_ram_filled;
uint64_t album_thumb_gen_cached_keys[ALBUM_THUMBNAIL_CACHE_SIZE];
thumbnail_key_kind_t album_thumb_gen_cached_key_kinds[ALBUM_THUMBNAIL_CACHE_SIZE];
int album_thumb_gen_cached_key_count;
album_thumb_gen_alias_key_t * album_thumb_gen_known_aliases;
size_t album_thumb_gen_known_alias_count;
static bool album_thumb_gen_user_visible;
static bool album_thumb_gen_force_reload;
static bool album_thumb_gen_single_album;
static bool album_thumb_gen_altstack_already_installed;
static int64_t album_thumb_gen_target_song_id;
static atomic_int album_thumb_gen_error_count;
static pthread_t thumbnail_cover_thread;
static atomic_bool thumbnail_cover_done;
bool thumbnail_cover_thread_joinable;

bool album_thumb_gen_cached_key(thumbnail_key_kind_t key_kind, uint64_t key) {
    for (int i = 0; i < album_thumb_gen_cached_key_count; i++)
        if (album_thumb_gen_cached_key_kinds[i] == key_kind &&
            album_thumb_gen_cached_keys[i] == key) return true;
    return false;
}

/* Persistent warmer state flags */
static pthread_t album_thumb_gen_thread;
atomic_bool album_thumb_gen_active;
static atomic_bool album_thumb_gen_cancel;
static atomic_int album_thumb_gen_generation;
static bool album_thumb_gen_thread_joinable;
atomic_int album_thumb_gen_done_count;
static atomic_int album_thumb_gen_total_count;
atomic_int album_thumb_gen_cache_epoch;

/* playback_pass_epoch records the cache epoch whose playback-time pass
 * finished, so the retry path does not rerun it every backoff; anything that
 * needs real extraction waits for playback to stop. */
static atomic_int album_thumb_gen_playback_pass_epoch = -1;
atomic_bool album_thumb_gen_retry_pending;
static uint32_t album_thumb_gen_retry_tick;

bool album_thumb_gen_should_cancel(int my_generation) {
    return atomic_load(&album_thumb_gen_cancel) ||
           atomic_load(&album_thumb_gen_generation) != my_generation ||
           atomic_load(&album_thumb_gen_cache_epoch) != atomic_load(&album_thumbnail_cache_epoch);
}

void cancel_album_thumbnail_generation(void) {
    if (!atomic_load(&album_thumb_gen_active)) return;
    atomic_store(&album_thumb_gen_cancel, true);
    atomic_fetch_add(&album_thumb_gen_generation, 1);
}

/* Waits for thumbnail generation thread completion. Requires both the
 * joinable flag and a non-NULL thread handle before calling pthread_join(). */
void reap_album_thumbnail_generation(void) {
    if (!album_thumb_gen_thread_joinable) return;
    album_thumb_gen_thread_joinable = false;
    if (!album_thumb_gen_thread) return;
    pthread_join(album_thumb_gen_thread, NULL);
}

/* Dynamic cancellation callbacks */
bool album_thumb_gen_cancel_cb(void * user_data) {
    int my_gen = (int) (intptr_t) user_data;
    return album_thumb_gen_should_cancel(my_gen) ||
           (!album_thumb_gen_user_visible && album_thumbnail_lazy_work_pending());
}

/* ---- Persistent (post-scan) album thumbnail generation ----------------
 * Runs once after Update Music Database completes, writing sized BMP files
 * in short bounded batches with generous cooperative delays so CPU and SD
 * activity never cause overheating or starve audio playback. Suspends
 * automatically when audio is playing, visible lazy work is pending, or
 * memory is low. */
#define ALBUM_THUMB_GEN_INTER_BATCH_US 1000000 /* 1.0 s pause between batches */

/* Artist cache keys of every artist named on an album's tracks (full tag
 * and each split name, matching how artist rows are grouped). */
static bool album_artist_cache_keys(const char * album, const char * album_artist,
                                    uint64_t ** out_keys, size_t * out_count) {
    *out_keys = NULL;
    *out_count = 0;
    size_t capacity = 0;
    song_row_t songs[32];
    for (int offset = 0;;) {
        int count = metadata_db_get_album_songs(album, album_artist, offset, songs,
                                                (int) (sizeof(songs) / sizeof(songs[0])));
        if (count <= 0) break;
        for (int i = 0; i < count; i++) {
            char names[TAGCACHE_ARTIST_SPLIT_MAX + 1][TAGCACHE_TAG_MAX];
            int n = tagcache_artist_names(songs[i].tags.artist, names, TAGCACHE_ARTIST_SPLIT_MAX);
            if (n < 0) n = 0;
            snprintf(names[n++], sizeof(names[0]), "%s", songs[i].tags.artist);
            for (int j = 0; j < n; j++) {
                if (!names[j][0]) continue;
                uint64_t key = albumart_artist_thumbnail_key(names[j]);
                bool seen = false;
                for (size_t k = 0; k < *out_count && !seen; k++) seen = (*out_keys)[k] == key;
                if (seen) continue;
                if (*out_count == capacity) {
                    size_t next = capacity ? capacity * 2 : 16;
                    uint64_t * grown = realloc(*out_keys, next * sizeof(*grown));
                    if (!grown) return false;
                    *out_keys = grown;
                    capacity = next;
                }
                (*out_keys)[(*out_count)++] = key;
            }
        }
        offset += count;
        if (count < (int) (sizeof(songs) / sizeof(songs[0]))) break;
    }
    return true;
}

/* Decodes an already found thumbnail cache file; NULL on any load or decode failure. */
static uint16_t * album_thumb_gen_read_thumbnail_file(const char * path, artwork_priority_t prio, int generation) {
    uint16_t * pixels = NULL;
    uint8_t * data = NULL;
    uint32_t size = 0;
    if (albumart_load_file_ex(path, &data, &size, THUMBNAIL_SIDECAR_MAX_BYTES, prio) == ALBUMART_LOAD_OK &&
        cover_decode_to_rgb565_ex(data, size, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX, prio,
                                  album_thumb_gen_cancel_cb, (void *) (intptr_t) generation,
                                  &pixels) != COVER_DECODE_OK) {
        free(pixels);
        pixels = NULL;
    }
    free(data);
    return pixels;
}

/* The metadata database still describes the card in the slot and the card is mounted. */
static bool thumb_storage_ok(void) {
    bool storage_ok = metadata_db_storage_current();
#ifndef HOST_BUILD
    storage_ok = storage_ok && sd_card_root_is_mounted();
#endif
    return storage_ok;
}

/* What one warmer run was asked to do and what it has done so far. */
typedef struct {
    int generation;
    bool user_visible, force_reload, single_album;
    artwork_priority_t decode_prio;
    uint64_t started_ms;
    int generated, cached, missing, failed;
#ifdef UI_PERF_TRACE
    int perf_generated, perf_skipped, perf_missing, perf_failed;
#endif
} album_warm_run_t;

/* A reload starts by deleting the generated caches it is going to rebuild. False ends the run. */
static bool album_warm_prepare_reload(const album_warm_run_t * run) {
    if (!thumb_storage_ok()) {
        atomic_fetch_add(&album_thumb_gen_error_count, 1);
        return false;
    }
    /* gui_player_begin_cover_reload() already invalidated any player
     * decode; wait here, off the UI thread, until one still running can
     * no longer write an old cover back after the deletion below. */
    while (!gui_player_cover_decode_idle()) {
        if (album_thumb_gen_should_cancel(run->generation)) return false;
        usleep(10000);
    }
    bool deleted = false;
    if (run->single_album) {
        song_row_t target_song;
        if (!metadata_db_get_song_by_id(album_thumb_gen_target_song_id, &target_song)) {
            atomic_fetch_add(&album_thumb_gen_error_count, 1);
            return false;
        }
        albumart_info_t info;
        albumart_info_from_song_row(&target_song, &info);
        /* Compilations: every track artist may hold a stale "no art"
         * marker with no alias pointing at this album. */
        uint64_t * artist_keys = NULL;
        size_t artist_key_count = 0;
        bool keys_ok = album_artist_cache_keys(target_song.tags.album, target_song.tags.album_artist,
                                               &artist_keys, &artist_key_count);
        deleted = keys_ok && albumart_delete_generated_cache_for_album(&info, artist_keys, artist_key_count);
        free(artist_keys);
    } else {
        deleted = albumart_delete_all_generated_caches();
    }
    if (!deleted) {
        atomic_fetch_add(&album_thumb_gen_error_count, 1);
        return false;
    }
    return true;
}

/* During playback, only read an already generated thumbnail cover (thumbnail priority, which the
 * coordinator admits during playback). Anything needing real extraction waits for playback to stop.
 * False ends the run. */
static bool album_warm_playback_album(album_warm_run_t * run, const song_row_t * song,
                                      const albumart_info_t * info, uint64_t album_key) {
    if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE) {
        /* RAM budget full: further reads would be decoded only
         * to be freed. End this playback pass now. */
        atomic_store(&album_thumb_gen_playback_pass_epoch,
                     atomic_load(&album_thumb_gen_cache_epoch));
        atomic_store(&album_thumb_gen_retry_pending, true);
        return false;
    }
    uint16_t * cached_pixels = NULL;
    char found[PATH_MAX];
    if (albumart_generated_cache_fresh(info, ALBUM_THUMBNAIL_PX, ALBUM_THUMBNAIL_PX,
                                       found, sizeof(found)))
        cached_pixels = album_thumb_gen_read_thumbnail_file(found, ARTWORK_PRIO_THUMBNAIL,
                                                            run->generation);
    if (!cached_pixels) {
        atomic_store(&album_thumb_gen_retry_pending, true);
    } else {
        album_thumb_gen_ram_filled++;
        album_thumbnail_handoff_t handoff = {
            .kind = ALBUM_BOOT_RESULT_ALBUM,
            .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
            .key_kind = THUMBNAIL_KEY_ALBUM,
            .key = album_key,
            .song_id = song->id,
            .pixels = (uint8_t *) cached_pixels,
        };
        if (!album_thumb_gen_handoff_result(run->generation, &handoff)) return false;
        run->cached++;
    }
    atomic_fetch_add(&album_thumb_gen_done_count, 1);
    usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
    return true;
}

/* Decodes (or reads) one album's thumbnail and hands it over. False ends the run. */
static bool album_warm_decode_album(album_warm_run_t * run, const song_row_t * song,
                                    const albumart_info_t * info, uint64_t album_key, bool * batch_worked) {
    uint32_t art_mtime = albumart_source_mtime(info);
    time_t source_mtime = album_source_mtime_with_art_mtime(song, art_mtime);

    artwork_fail_reason_t fail_reason;
    if (artwork_failure_cache_is_blocked(song->id, source_mtime, &fail_reason)) {
        if (fail_reason == ARTWORK_FAIL_TEMPORARY)
            atomic_store(&album_thumb_gen_retry_pending, true);
        run->failed++;
#ifdef UI_PERF_TRACE
        run->perf_skipped++;
#endif
        atomic_fetch_add(&album_thumb_gen_done_count, 1);
        return true;
    }

    char thumb_found[PATH_MAX];
    char player_found[PATH_MAX];
    /* Still enter the decode call below for the first ALBUM_THUMBNAIL_
     * CACHE_SIZE albums even when both persistent caches already exist
     * -- on every boot after the very first one this is nearly always
     * true for the whole library, and skipping straight to `continue`
     * here (as every album past the RAM budget still does) would mean
     * the RAM-preload hand-off below never runs at all on a normal
     * boot. album_thumbnail_load_or_decode_ex() already returns the
     * existing thumbnail sidecar's pixels immediately in this case without
     * touching the original source file, so this costs one cheap BMP
     * read+decode per album instead of a real cache regeneration. */
    bool art_cached =
        album_thumbnail_sized_cache_hit_with_art_mtime(info, art_mtime, thumb_found, sizeof(thumb_found)) &&
        album_player_cache_hit_with_art_mtime(info, art_mtime, player_found, sizeof(player_found));
    if (album_thumb_gen_ram_filled >= ALBUM_THUMBNAIL_CACHE_SIZE && art_cached) {
        run->cached++;
#ifdef UI_PERF_TRACE
        run->perf_skipped++;
#endif
        atomic_fetch_add(&album_thumb_gen_done_count, 1);
        return true;
    }

    /* Both caches are fresh: read the thumbnail file found above rather
     * than re-running the freshness checks inside the decode call. Any
     * read failure falls back to it for corrupt-cache handling. A
     * reload keeps the decode call, which records whether the
     * thumbnail came from a user's file (album_thumbnail_reload_persisted). */
    uint16_t * pixels = art_cached && !run->force_reload
        ? album_thumb_gen_read_thumbnail_file(thumb_found, run->decode_prio, run->generation) : NULL;
    /* The decode call may extract from the source, so it is paced. */
    bool extracted = !pixels;
    if (extracted) *batch_worked = true;
    if (!pixels)
        album_thumbnail_load_or_decode_ex(song, run->decode_prio,
                                          album_thumb_gen_cancel_cb, (void *) (intptr_t) run->generation,
                                          &pixels);
    /* One failure-cache lookup for the failed decode: it takes a mutex and can drop an expired entry. */
    artwork_fail_reason_t failed_reason = ARTWORK_FAIL_NONE;
    bool failed_blocked = !pixels && artwork_failure_cache_is_blocked(song->id, source_mtime, &failed_reason);
    if (!pixels && ((!run->user_visible && audio_is_playing()) || album_thumb_gen_should_cancel(run->generation) ||
        (failed_blocked && failed_reason == ARTWORK_FAIL_TEMPORARY)))
        atomic_store(&album_thumb_gen_retry_pending, true);
    if (pixels) { artwork_failure_cache_note_success(song->id); run->generated++; } else { run->failed++; }
    if (run->force_reload && pixels && !album_thumbnail_reload_persisted(song)) {
        /* Decoded but not saved (e.g. card full): the next boot
         * would lose it, so report the reload as incomplete. */
        atomic_fetch_add(&album_thumb_gen_error_count, 1);
    }
    if (run->force_reload && !pixels && (!failed_blocked || failed_reason == ARTWORK_FAIL_TEMPORARY))
        atomic_fetch_add(&album_thumb_gen_error_count, 1);
    if (!pixels)
        DB_LOG("ART_CACHE", "album_failed song=%lld path=%s reason=%s",
               (long long) song->id, song->path,
               !failed_blocked ? "unrecorded" :
               (failed_reason == ARTWORK_FAIL_PERMANENT ? "PERMANENT" : "TEMPORARY"));

#ifdef UI_PERF_TRACE
    if (pixels) run->perf_generated++; else run->perf_failed++;
#endif
    bool keep_in_ram = album_thumb_gen_ram_filled < ALBUM_THUMBNAIL_CACHE_SIZE;
    if (keep_in_ram) album_thumb_gen_ram_filled++;
    if (keep_in_ram || (!run->user_visible && album_thumbnail_lazy_work_pending())) {
        if (album_thumb_gen_should_cancel(run->generation)) {
            free(pixels);
            return false;
        }
        album_thumbnail_handoff_t handoff = {
            .kind = ALBUM_BOOT_RESULT_ALBUM,
            .cache_epoch = atomic_load(&album_thumb_gen_cache_epoch),
            .key_kind = THUMBNAIL_KEY_ALBUM,
            .key = album_key,
            .song_id = song->id,
            .pixels = (uint8_t *) pixels,
            .have_source_mtime = pixels == NULL,
            .source_mtime = source_mtime
        };
        if (!album_thumb_gen_handoff_result(run->generation, &handoff)) return false;
    } else {
        free(pixels);
    }
    atomic_fetch_add(&album_thumb_gen_done_count, 1);

    if (!run->user_visible && album_thumbnail_lazy_work_pending()) {
        atomic_store(&album_thumb_gen_retry_pending, true);
        return false;
    }

    int diag_done = atomic_load(&album_thumb_gen_done_count);
    if ((diag_done % 50) == 0) {
        DB_LOG("ART_CACHE", "progress done=%d total=%d generated=%d cached=%d missing=%d failed=%d elapsed_ms=%llu rss_kb=%ld",
               diag_done, atomic_load(&album_thumb_gen_total_count), run->generated, run->cached, run->missing, run->failed,
               (unsigned long long) (db_log_now_ms() - run->started_ms), db_log_rss_kb());
    }

    if (!run->user_visible && extracted) usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US);
    return true;
}

/* One album row of the warm pass. False ends the run. */
static bool album_warm_one_album(album_warm_run_t * run, const group_row_t * row, bool * batch_worked) {
    if (album_thumb_gen_should_cancel(run->generation)) return false;

    if (run->force_reload && !thumb_storage_ok()) {
        atomic_fetch_add(&album_thumb_gen_error_count, 1);
        return false;
    }

    /* Check suspension before each album */
    if (!run->user_visible && album_thumbnail_lazy_work_pending()) {
        atomic_store(&album_thumb_gen_retry_pending, true);
        return false;
    }

    song_row_t song;
    if (!metadata_db_get_song_by_id(row->first_song_id, &song)) {
        run->missing++;
        if (run->force_reload) atomic_fetch_add(&album_thumb_gen_error_count, 1);
#ifdef UI_PERF_TRACE
        run->perf_missing++;
#endif
        atomic_fetch_add(&album_thumb_gen_done_count, 1);
        return true;
    }

    uint64_t album_key = song_album_thumbnail_key(&song);
    if (album_thumb_gen_cached_key(THUMBNAIL_KEY_ALBUM, album_key)) {
        run->cached++;
#ifdef UI_PERF_TRACE
        run->perf_skipped++;
#endif
        atomic_fetch_add(&album_thumb_gen_done_count, 1);
        return true;
    }

    albumart_info_t info;
    albumart_info_from_song_row(&song, &info);

    if (!run->user_visible && audio_is_playing()) {
        if (!album_warm_playback_album(run, &song, &info, album_key)) return false;
        *batch_worked = true;
        return true;
    }
    return album_warm_decode_album(run, &song, &info, album_key, batch_worked);
}

/* The album pass: every page of albums (or the one album of a single-album reload), paced so the
 * card gets rest between batches that extracted anything. False ends the run. */
static bool album_warm_albums(album_warm_run_t * run) {
    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    int offset = 0;
    for (;;) {
        /* Pacing only protects the card from extraction work; a batch of
         * already cached albums is not followed by a pause. */
        bool batch_worked = false;
        if (album_thumb_gen_should_cancel(run->generation)) break;

        /* Yield to visible lazy work or memory pressure. Playback uses the
         * cache-only branch instead of extracting new artwork. */
        if ((!run->user_visible && album_thumbnail_lazy_work_pending()) ||
            !artwork_check_memory_admission(run->decode_prio, ALBUM_ART_METADATA_START_BYTES)) {
            if (run->user_visible) atomic_fetch_add(&album_thumb_gen_error_count, 1);
            else atomic_store(&album_thumb_gen_retry_pending, true);
            return false;
        }

        int n;
        if (run->single_album) {
            if (offset > 0) break;
            memset(rows, 0, sizeof(rows));
            rows[0].first_song_id = album_thumb_gen_target_song_id;
            n = 1;
        } else {
            n = metadata_db_get_albums_page_filtered(NULL, offset, ALBUM_THUMB_GEN_BATCH, rows);
        }
        if (n <= 0) break;
        for (int i = 0; i < n; i++)
            if (!album_warm_one_album(run, &rows[i], &batch_worked)) return false;
        offset += n;
        if (run->single_album || n < ALBUM_THUMB_GEN_BATCH) break;

        /* Pause between batches to give CPU/SD bus complete rest */
        if (!run->user_visible && batch_worked)
            for (int p = 0; p < 10; p++) {
                if (album_thumb_gen_should_cancel(run->generation)) return false;
                if (album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    return false;
                }
                usleep(ALBUM_THUMB_GEN_INTER_BATCH_US / 10);
            }
    }
    return true;
}

static void album_thumb_gen_run(album_warm_run_t * run) {
    int my_generation = run->generation;
    if (album_thumb_gen_should_cancel(my_generation)) return;
    if (!run->user_visible && album_thumbnail_lazy_work_pending()) {
        atomic_store(&album_thumb_gen_retry_pending, true);
        return;
    }

    if (run->force_reload && !album_warm_prepare_reload(run)) return;

    if (!run->user_visible && !run->single_album && !run->force_reload &&
        !album_thumb_gen_warm_first_screens(my_generation))
        return;

    if (!album_warm_albums(run)) return;
    if (!run->user_visible && !run->single_album && audio_is_playing()) {
        /* The playback-time cache-only pass is done for this cache epoch;
         * the artist pass and any skipped extraction wait for playback to
         * stop. */
        atomic_store(&album_thumb_gen_playback_pass_epoch, atomic_load(&album_thumb_gen_cache_epoch));
        atomic_store(&album_thumb_gen_retry_pending, true);
        return;
    }
    /* Playback was running when this pass began and has stopped since: the first screens that waited
     * for it run now (a no-op when they were already warmed). */
    if (!run->user_visible && !run->single_album && !album_thumb_gen_should_cancel(my_generation) &&
        album_thumb_gen_warm_waited_epoch == atomic_load(&album_thumb_gen_cache_epoch) &&
        !album_thumb_gen_warm_first_screens(my_generation))
        return;
    if (!run->user_visible && !run->single_album && !album_thumb_gen_should_cancel(my_generation) &&
        !album_thumb_gen_warm_artist_aliases(my_generation))
        return;
}

static void * album_thumb_gen_thread_func(void * arg) {
    if (!album_thumb_gen_altstack_already_installed)
        install_thread_crash_altstack(); /* see its own comment (main.c) */
    album_thumb_gen_altstack_already_installed = false;
    int my_generation = (int) (intptr_t) arg;
    bool user_visible = album_thumb_gen_user_visible;
    worker_lower_priority(user_visible ? WORKER_NICE_BACKGROUND : WORKER_NICE_IDLE);
    album_warm_run_t run = {
        .generation = my_generation,
        .user_visible = user_visible,
        .force_reload = album_thumb_gen_force_reload,
        .single_album = album_thumb_gen_single_album,
        /* A user-started reload may run during playback, which the coordinator
         * refuses at warmer priority. */
        .decode_prio = user_visible ? ARTWORK_PRIO_THUMBNAIL : ARTWORK_PRIO_WARMER
    };
    album_art_enable_player_cache_materialization();
    run.started_ms = db_log_enabled() ? db_log_now_ms() : 0;
    DB_LOG("ART_CACHE", "worker_begin generation=%d total=%d rss_kb=%ld", my_generation,
           atomic_load(&album_thumb_gen_total_count), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    uint64_t perf_start_us = ui_perf_now_us();
#endif
    atomic_store(&album_thumb_gen_done_count, 0);

    album_thumb_gen_run(&run);

    DB_LOG("ART_CACHE", "worker_end generation=%d done=%d total=%d generated=%d cached=%d missing=%d failed=%d cancelled=%d elapsed_ms=%llu rss_kb=%ld",
           my_generation, atomic_load(&album_thumb_gen_done_count), atomic_load(&album_thumb_gen_total_count),
           run.generated, run.cached, run.missing, run.failed, (int) album_thumb_gen_should_cancel(my_generation),
           (unsigned long long) (db_log_now_ms() - run.started_ms), db_log_rss_kb());
#ifdef UI_PERF_TRACE
    printf("PERF album_thumb_gen done=%d generated=%d skipped=%d missing=%d failed=%d us=%llu cancelled=%d\n",
           atomic_load(&album_thumb_gen_done_count), run.perf_generated, run.perf_skipped, run.perf_missing,
           run.perf_failed, (unsigned long long) (ui_perf_now_us() - perf_start_us),
           (int) album_thumb_gen_should_cancel(my_generation));
#endif
    atomic_store(&album_thumb_gen_active, false);
    ui_wake_notify(); /* a visible-row decode may have been waiting for this worker to yield */
    return NULL;
}

/* Called once, right after Update Music Database's own scan thread is
 * joined and the database is already fully committed (see this function's
 * own call site) -- never blocks the caller waiting for generation itself
 * to finish, only (briefly) for a STILL-RUNNING previous pass to notice
 * it's been superseded and exit, which happens within roughly one album's
 * worth of decode work given the cancellation check at the top of every
 * iteration. */
static int prepare_album_thumbnail_generation(bool user_visible, bool force_reload,
                                             bool single_album, int64_t target_song_id) {
    cancel_album_thumbnail_generation();
    reap_album_thumbnail_generation();
    (void) album_thumbnail_commit_boot_preload();
    album_thumb_gen_user_visible = user_visible;
    album_thumb_gen_force_reload = force_reload;
    album_thumb_gen_single_album = single_album;
    album_thumb_gen_target_song_id = target_song_id;
    album_thumb_gen_altstack_already_installed = false;
    atomic_store(&album_thumb_gen_error_count, 0);
    atomic_store(&album_thumb_gen_retry_pending, false);
    album_thumb_gen_ram_filled = 0;
    album_thumb_gen_cached_key_count = 0;
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; i++) {
        album_thumbnail_cache_entry_t * entry = &album_thumbnail_cache[i];
        if (!entry->known || !entry->pixels) continue;
        if (album_thumb_gen_cached_key_count >= ALBUM_THUMBNAIL_CACHE_SIZE) break;
        album_thumb_gen_cached_keys[album_thumb_gen_cached_key_count++] = entry->key;
        album_thumb_gen_cached_key_kinds[album_thumb_gen_cached_key_count - 1] = entry->key_kind;
        album_thumb_gen_ram_filled++; /* every entry with pixels uses a slot, whatever its kind */
    }
    album_thumb_gen_retry_tick = lv_tick_get();

    int artist_count = 0, album_artist_count = 0, album_count = 0;
    metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
    (void) artist_thumbnail_alias_cache_resize(artist_count, album_artist_count);
    free(album_thumb_gen_known_aliases);
    album_thumb_gen_known_aliases = NULL;
    album_thumb_gen_known_alias_count = 0;
    if (artist_thumbnail_alias_cache_capacity)
        album_thumb_gen_known_aliases = malloc(artist_thumbnail_alias_cache_capacity *
                                               sizeof(*album_thumb_gen_known_aliases));
    for (size_t i = 0; album_thumb_gen_known_aliases && i < artist_thumbnail_alias_cache_capacity; i++)
        if (artist_thumbnail_alias_cache[i].known)
            album_thumb_gen_known_aliases[album_thumb_gen_known_alias_count++] =
                (album_thumb_gen_alias_key_t) { artist_thumbnail_alias_cache[i].artist_key,
                                                artist_thumbnail_alias_cache[i].artist_group_kind,
                                                artist_thumbnail_alias_cache[i].has_album };
    atomic_store(&album_thumb_gen_done_count, 0);
    int total = single_album ? 1 : album_count;
    if (!user_visible && !single_album && atomic_load(&artist_thumbnail_work_enabled))
        total += artist_count + album_artist_count;
    atomic_store(&album_thumb_gen_total_count, total);
    atomic_store(&album_thumb_gen_cancel, false);
    atomic_store(&album_thumb_gen_cache_epoch, atomic_load(&album_thumbnail_cache_epoch));
    int generation = atomic_fetch_add(&album_thumb_gen_generation, 1) + 1;
    atomic_store(&album_thumb_gen_active, true);
    /* Only start_next_album_thumbnail()'s own two trigger points used to
     * need this timer, and both already resume it. The RAM-preload hand-off
     * now also depends on it ticking to drain album_boot_preload_
     * pending -- without this, a cold boot's first generation pass would
     * stage exactly one result and then block forever with nothing left to
     * consume it. */
    thumbnail_poll_resume();
    DB_LOG("ART_CACHE", "start generation=%d albums=%d rss_kb=%ld",
           generation, single_album ? 1 : album_count, db_log_rss_kb());
    return generation;
}

static bool launch_album_thumbnail_generation(int generation) {
    bool created = worker_thread_create(&album_thumb_gen_thread, ALBUM_COVER_DECODE_THREAD_STACK_SIZE,
                                        album_thumb_gen_thread_func, (void *) (intptr_t) generation) == 0;
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        DB_LOG("ART_CACHE", "thread_create_failed generation=%d rss_kb=%ld", generation, db_log_rss_kb());
    } else {
        album_thumb_gen_thread_joinable = true;
    }
    return created;
}

static void start_album_thumbnail_generation(void) {
    int generation = prepare_album_thumbnail_generation(false, false, false, 0);
    (void) launch_album_thumbnail_generation(generation);
}

/* The album whose failure-cache entries the cover worker forgets before it reads that cache.
 * Written by the UI thread before the thread is created and read only by that thread, which is
 * joined before the next start. */
static char thumbnail_cover_clear_album[METADATA_DB_TEXT_MAX];
static char thumbnail_cover_clear_artist[METADATA_DB_TEXT_MAX];
static bool thumbnail_cover_clear_pending;

static void thumbnail_cover_clear_album_failures(int my_generation) {
    /* A failed allocation still clears, a few rows at a time, so the reload can retry the art. */
    song_row_t fallback[4];
    song_row_t *songs = malloc(64 * sizeof(*songs));
    int batch = songs ? 64 : (int) (sizeof(fallback) / sizeof(fallback[0]));
    song_row_t *rows = songs ? songs : fallback;
    int offset = 0;
    /* A cancelled reload stops here; entries already cleared only cost a retry. */
    while (!album_thumb_gen_should_cancel(my_generation)) {
        int count = metadata_db_get_album_songs(thumbnail_cover_clear_album, thumbnail_cover_clear_artist,
                                                offset, rows, batch);
        if (count <= 0) break;
        for (int i = 0; i < count; i++) artwork_failure_cache_note_success(rows[i].id);
        offset += count;
        if (count < batch) break;
    }
    free(songs);
}

static void *thumbnail_cover_thread_func(void *arg) {
    install_thread_crash_altstack();
#ifdef __linux__
    struct sched_param param = { .sched_priority = 0 };
    (void)sched_setscheduler(0, SCHED_BATCH, &param);
#endif
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    album_thumb_gen_altstack_already_installed = true;
    /* Before the reload reads the failure cache, on this thread. */
    if (thumbnail_cover_clear_pending) thumbnail_cover_clear_album_failures((int) (intptr_t) arg);
    album_thumb_gen_thread_func(arg);
    atomic_store_explicit(&thumbnail_cover_done, true, memory_order_release);
    return NULL;
}

void library_thumbnails_start_warmup(void) { start_album_thumbnail_generation(); }

void library_thumbnails_retry_warmup(void) {
    if (album_thumb_gen_thread_joinable && !atomic_load(&album_thumb_gen_active))
        reap_album_thumbnail_generation();
    if (thumbnail_policy(LIBRARY_THUMB_POLICY_WARMUP_RETRY) ||
        !atomic_load(&album_thumb_gen_retry_pending) || atomic_load(&album_thumb_gen_active) ||
        atomic_load(&album_thumbnail_active) || album_thumbnail_lazy_work_pending() ||
        (audio_is_playing() && atomic_load(&album_thumb_gen_playback_pass_epoch) ==
                                atomic_load(&album_thumbnail_cache_epoch)) ||
        lv_tick_elaps(album_thumb_gen_retry_tick) < 15000) return;
#ifndef HOST_BUILD
    if (!sd_card_root_is_mounted()) return;
#endif
    if (!artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES)) return;
    start_album_thumbnail_generation();
}

void library_thumbnails_cancel_warmup(void) {
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
}

void library_thumbnails_invalidate_storage(void) {
    album_thumbnail_cache_epoch_advance();
    cancel_album_thumbnail_generation();
    atomic_store(&album_thumb_gen_retry_pending, false);
}

bool library_thumbnails_start_cover_reload(const library_thumbnail_cover_request_t *request,
                                           library_thumbnail_show_busy_cb_t show_busy) {
    if (!request) return false;
    int generation = prepare_album_thumbnail_generation(true, true,
        request->scope == LIBRARY_THUMB_COVER_ALBUM, request->target_song_id);
    atomic_store(&thumbnail_cover_done, false);
    thumbnail_cover_clear_pending = request->scope == LIBRARY_THUMB_COVER_ALBUM && request->album &&
                                    request->album_artist;
    if (thumbnail_cover_clear_pending) {
        snprintf(thumbnail_cover_clear_album, sizeof(thumbnail_cover_clear_album), "%s", request->album);
        snprintf(thumbnail_cover_clear_artist, sizeof(thumbnail_cover_clear_artist), "%s", request->album_artist);
    }
    if (show_busy) show_busy(request->scope);
    bool created = worker_thread_create(&thumbnail_cover_thread, ALBUM_COVER_DECODE_THREAD_STACK_SIZE,
                                        thumbnail_cover_thread_func, (void *)(intptr_t)generation) == 0;
    if (!created) {
        atomic_store(&album_thumb_gen_active, false);
        atomic_store(&album_thumb_gen_cancel, true);
        return false;
    }
    thumbnail_cover_thread_joinable = true;
    return true;
}

void library_thumbnails_cancel_cover_reload(void) { cancel_album_thumbnail_generation(); }

void library_thumbnails_join_cover_abort(void) {
    if (!thumbnail_cover_thread_joinable) return;
    thumbnail_cover_thread_joinable = false;
    pthread_join(thumbnail_cover_thread, NULL);
    atomic_store(&thumbnail_cover_done, false);
}

void library_thumbnails_finish_cover_reload(void) {
    if (thumbnail_cover_thread_joinable) {
        thumbnail_cover_thread_joinable = false;
        pthread_join(thumbnail_cover_thread, NULL);
    }
    atomic_store(&thumbnail_cover_done, false);
    (void)album_thumbnail_commit_boot_preload();
    refresh_all_thumbnail_lists();
}

void library_thumbnails_get_status(library_thumbnail_status_t *out) {
    if (!out) return;
    *out = (library_thumbnail_status_t) {
        .lazy_active = atomic_load(&album_thumbnail_active),
        .warmup_active = atomic_load(&album_thumb_gen_active),
        .cover_active = thumbnail_cover_thread_joinable,
        .cover_done = atomic_load_explicit(&thumbnail_cover_done, memory_order_acquire),
        .cover_cancelled = atomic_load(&album_thumb_gen_cancel),
        .cover_done_count = atomic_load(&album_thumb_gen_done_count),
        .cover_total_count = atomic_load(&album_thumb_gen_total_count),
        .cover_error_count = atomic_load(&album_thumb_gen_error_count),
        .decoded_cache_entries = 0
    };
#ifdef UI_PERF_TRACE
    for (int i = 0; i < ALBUM_THUMBNAIL_CACHE_SIZE; ++i)
        if (album_thumbnail_cache[i].pixels) out->decoded_cache_entries++;
#endif
}

