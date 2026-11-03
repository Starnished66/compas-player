#include "library_thumbnails_internal.h"
#include "../../core/ui/display/ui_wake.h"
#include "../../library/artwork/decode/library_artwork_internal.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../../core/system/logging/db_log.h"
#include "../../audio/engine/audio.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

atomic_int album_boot_preload_state;
static album_thumbnail_handoff_t album_boot_preload_result;

bool album_thumbnail_commit_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return false;
    bool committed = false, alias_stored = false, changed = false, row_resolved = false; /* changed: the cache holds something new to repaint */
    if (album_boot_preload_result.cache_epoch == atomic_load(&album_thumbnail_cache_epoch)) {
        if (album_boot_preload_result.kind == ALBUM_BOOT_RESULT_ALBUM) {
            album_thumbnail_cache_commit(album_boot_preload_result.key_kind,
                album_boot_preload_result.key, album_boot_preload_result.song_id,
                album_boot_preload_result.pixels, album_boot_preload_result.have_source_mtime,
                album_boot_preload_result.source_mtime);
            committed = changed = true;
        } else {
            if ((album_boot_preload_result.artist_has_alias ||
                 album_boot_preload_result.artist_negative_confirmed) &&
                artist_thumbnail_alias_cache_commit(album_boot_preload_result.artist_key,
                    album_boot_preload_result.artist_group_kind,
                    album_boot_preload_result.artist_has_alias,
                    album_boot_preload_result.alias_album_key,
                    album_boot_preload_result.alias_song_id)) {
                artist_thumbnail_alias_settled(album_boot_preload_result.artist_key,
                    album_boot_preload_result.artist_group_kind, album_boot_preload_result.artist_has_alias);
                committed = changed = alias_stored = true;
            }
            if (album_boot_preload_result.pixels && album_boot_preload_result.first_screen) {
                album_thumbnail_cache_entry_t * there = album_thumbnail_cache_find(
                    album_boot_preload_result.key_kind, album_boot_preload_result.key);
                if (there && there->pixels) {
                    /* A warm rerun reached a row that is already in RAM: replacing it would only
                     * detach and repaint the same pixels. */
                    free(album_boot_preload_result.pixels);
                    album_boot_preload_result.pixels = NULL;
                    committed = true;
                }
            }
            if (album_boot_preload_result.pixels) {
                album_thumbnail_cache_commit(album_boot_preload_result.key_kind,
                    album_boot_preload_result.key, album_boot_preload_result.song_id,
                    album_boot_preload_result.pixels, false, 0);
                committed = changed = true;
            }
        }
        album_boot_preload_result.pixels = NULL;
        /* An album's pixels do not resolve the artist row when its alias could not be stored. */
        row_resolved = alias_stored || (committed && album_boot_preload_result.key_kind == THUMBNAIL_KEY_ARTIST);
        if (album_boot_preload_result.first_screen && row_resolved &&
            album_boot_preload_result.kind == ALBUM_BOOT_RESULT_ARTIST) {
            /* Record the row where the press prewarm looks, and pin what holds its pixels. */
            int epoch = album_boot_preload_result.cache_epoch;
            int k = album_boot_preload_result.artist_group_kind == METADATA_DB_GROUP_ALBUM_ARTIST ? 1 : 0;
            if (prewarm_done_epoch[k] != epoch || prewarm_done_first[k] != 0 || !prewarm_boot_pending[k]) {
                prewarm_done_epoch[k] = epoch;
                prewarm_done_first[k] = 0;
                prewarm_keys_count[k] = 0;
                prewarm_boot_pending[k] = true;
            }
            bool recorded = false;
            for (int j = 0; j < prewarm_keys_count[k]; j++)
                if (prewarm_keys[k][j] == album_boot_preload_result.artist_key) { recorded = true; break; }
            if (!recorded && prewarm_keys_count[k] < PREWARM_MAX)
                prewarm_keys[k][prewarm_keys_count[k]++] = album_boot_preload_result.artist_key;
            if (first_screen_pin_epoch != epoch) album_thumbnail_unpin_all();
            first_screen_pin_epoch = epoch;
            album_thumbnail_pin(THUMBNAIL_KEY_ARTIST, album_boot_preload_result.artist_key);
            if (album_boot_preload_result.artist_has_alias)
                album_thumbnail_pin(THUMBNAIL_KEY_ALBUM, album_boot_preload_result.alias_album_key);
        }
    } else {
        free(album_boot_preload_result.pixels);
        album_boot_preload_result.pixels = NULL;
    }
    /* The warmer now runs while list screens are open: repaint only when a
     * row on screen actually shows what was committed, not once per album. */
    /* An artist result may carry an album's pixels too: check both keys. */
    if (changed &&
        ((album_boot_preload_result.kind == ALBUM_BOOT_RESULT_ARTIST && row_resolved &&
          thumbnail_active_list_shows(THUMBNAIL_KEY_ARTIST, album_boot_preload_result.artist_key)) ||
         thumbnail_active_list_shows(album_boot_preload_result.key_kind, album_boot_preload_result.key)))
        refresh_thumbnail_list_visible(album_thumbnail_active_list);
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
    return true;
}

void album_thumbnail_discard_boot_preload(void) {
    int expected = ALBUM_BOOT_PRELOAD_PENDING;
    if (!atomic_compare_exchange_strong(&album_boot_preload_state, &expected,
                                         ALBUM_BOOT_PRELOAD_CLAIMED))
        return;
    free(album_boot_preload_result.pixels);
    album_boot_preload_result.pixels = NULL;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
}

/* The handoff of an artist row: its alias / no-art outcome plus any pixels the resolution decoded. */
album_thumbnail_handoff_t artist_handoff(int cache_epoch, uint64_t artist_key,
                                                metadata_db_group_kind_t group_kind, int64_t first_song_id,
                                                const artist_thumbnail_resolution_t * resolution,
                                                bool first_screen) {
    return (album_thumbnail_handoff_t) {
        .kind = ALBUM_BOOT_RESULT_ARTIST,
        .cache_epoch = cache_epoch,
        .key_kind = resolution->pixel_key_kind,
        .key = resolution->pixel_key,
        .song_id = resolution->pixel_song_id > 0 ? resolution->pixel_song_id : first_song_id,
        .pixels = (uint8_t *) resolution->pixels,
        .artist_key = artist_key,
        .artist_group_kind = group_kind,
        .artist_has_alias = resolution->has_alias,
        .artist_negative_confirmed = resolution->negative_confirmed,
        .alias_album_key = resolution->alias_album_key,
        .alias_song_id = resolution->alias_song_id,
        .first_screen = first_screen
    };
}

bool album_thumb_gen_handoff_result(int my_generation,
                                            const album_thumbnail_handoff_t * result) {
    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            free(result->pixels);
            return false;
        }
        usleep(5000);
    }
    if (album_thumb_gen_should_cancel(my_generation)) {
        free(result->pixels);
        return false;
    }

    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_CLAIMED);
    album_boot_preload_result = *result;
    atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_PENDING);
    ui_wake_notify(); /* commit on the next loop pass, not the next 50 ms poll */

    while (atomic_load(&album_boot_preload_state) != ALBUM_BOOT_PRELOAD_EMPTY) {
        if (album_thumb_gen_should_cancel(my_generation)) {
            int expected_state = ALBUM_BOOT_PRELOAD_PENDING;
            if (atomic_compare_exchange_strong(&album_boot_preload_state, &expected_state,
                                               ALBUM_BOOT_PRELOAD_CLAIMED)) {
                free(album_boot_preload_result.pixels);
                album_boot_preload_result.pixels = NULL;
                atomic_store(&album_boot_preload_state, ALBUM_BOOT_PRELOAD_EMPTY);
                return false;
            }
        }
        usleep(5000);
    }
    return true;
}

bool album_thumb_gen_warm_artist_aliases(int my_generation) {
    if (!atomic_load(&artist_thumbnail_work_enabled)) return true;
    const metadata_db_group_kind_t kinds[] = {
        METADATA_DB_GROUP_ARTIST,
        METADATA_DB_GROUP_ALBUM_ARTIST
    };
    int counts[2], unused;
    metadata_db_get_group_counts(&counts[0], &counts[1], &unused);

    group_row_t rows[ALBUM_THUMB_GEN_BATCH];
    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
        metadata_db_group_kind_t group_kind = kinds[kind_index];
        for (int offset = 0; offset < counts[kind_index];) {
            if (album_thumb_gen_should_cancel(my_generation)) return false;
            if (audio_is_playing() || album_thumbnail_lazy_work_pending() ||
                !artwork_check_memory_admission(ARTWORK_PRIO_WARMER,
                                                ALBUM_ART_METADATA_START_BYTES)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                return false;
            }

            int n = metadata_db_get_groups_page(group_kind, offset,
                                                ALBUM_THUMB_GEN_BATCH, rows);
            if (n <= 0) break;
            for (int i = 0; i < n; i++) {
                if (album_thumb_gen_should_cancel(my_generation)) return false;
                if (!atomic_load(&artist_thumbnail_work_enabled)) return true;
                if (audio_is_playing() || album_thumbnail_lazy_work_pending()) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    return false;
                }

                uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
                bool alias_known = false;
                for (size_t k = 0; k < album_thumb_gen_known_alias_count && !alias_known; k++)
                    alias_known = album_thumb_gen_known_aliases[k].key == artist_key &&
                                  album_thumb_gen_known_aliases[k].kind == group_kind;
                if (alias_known || album_thumb_gen_cached_key(THUMBNAIL_KEY_ARTIST, artist_key)) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                artist_thumbnail_resolution_t resolution;
                artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(
                    rows[i].first_song_id, group_kind, rows[i].name, ARTWORK_PRIO_WARMER, false,
                    artist_key, album_thumb_gen_cancel_cb, (void *) (intptr_t) my_generation,
                    &resolution);
                if (result == ARTIST_THUMB_OWN_IMAGE) {
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }
                if (result == ARTIST_THUMB_CANCELLED) {
                    if (!album_thumb_gen_should_cancel(my_generation))
                        atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    return false;
                }
                if (result == ARTIST_THUMB_TEMPORARY) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    free(resolution.pixels);
                    atomic_fetch_add(&album_thumb_gen_done_count, 1);
                    continue;
                }

                if (resolution.has_alias || resolution.negative_confirmed || resolution.pixels) {
                    album_thumbnail_handoff_t handoff = artist_handoff(
                        atomic_load(&album_thumb_gen_cache_epoch), artist_key, group_kind,
                        rows[i].first_song_id, &resolution, false);
                    if (!album_thumb_gen_handoff_result(my_generation, &handoff)) return false;
                } else {
                    free(resolution.pixels);
                }
                atomic_fetch_add(&album_thumb_gen_done_count, 1);
                usleep(ALBUM_THUMB_GEN_INTER_ALBUM_US); /* same cooling yield as the album pass */
            }
            offset += n;
            if (n < ALBUM_THUMB_GEN_BATCH) break;
        }
    }
    return true;
}

/* First screen of Artists and Album Artist, decoded into the RAM cache before the long album pass
 * below starts, so a first open (even soon after boot) finds the rows ready. Same worker, same
 * yields as the rest of the warm-up: it stops for playback, visible-row work, low memory and
 * cancellation, hands every result to the UI thread through the same one-slot rendezvous (so the
 * epoch check applies), and honours the artist-image and lazy settings. It adds no buffers: each
 * decoded thumbnail goes into the existing LRU cache and counts against the album pass's RAM
 * budget (album_thumb_gen_ram_filled), so the cache never holds more than its 250 entries and
 * the warmed rows are not the first to be evicted. Runs once per cache epoch. Returns false when
 * the pass must end (yield or cancel; the retry runs it again). */
#define ALBUM_THUMB_GEN_SCREEN_ROWS 12
#define ALBUM_THUMB_GEN_SCREEN_GAP_US 30000
atomic_int album_thumb_gen_screens_epoch = -1;
int album_thumb_gen_warm_waited_epoch = -1; /* worker only: the epoch whose warm waited for playback */

/* The warm's own cancel callback: the warmer's usual rules plus playback, so a decode that is
 * already running at thumbnail priority stops when playback starts instead of finishing its
 * extraction beside it. */
static bool album_thumb_gen_warm_cancel_cb(void * user_data) {
    return album_thumb_gen_cancel_cb(user_data) || audio_is_playing();
}

/* Appends an album the warm just handed over, so the album pass does not decode it a second time.
 * Worker thread: it owns the list while a run is active. */
static bool album_thumb_gen_note_cached(thumbnail_key_kind_t key_kind, uint64_t key) {
    if (album_thumb_gen_cached_key_count >= ALBUM_THUMBNAIL_CACHE_SIZE ||
        album_thumb_gen_cached_key(key_kind, key)) return false;
    album_thumb_gen_cached_keys[album_thumb_gen_cached_key_count] = key;
    album_thumb_gen_cached_key_kinds[album_thumb_gen_cached_key_count++] = key_kind;
    return true;
}

/* True when the snapshot says this artist row is already resolved with nothing left to load (a
 * "no art" marker). Alias rows are re-resolved on a retry: the snapshot stays at 16 bytes a row. */
static bool album_thumb_gen_alias_row_warm(uint64_t artist_key, metadata_db_group_kind_t kind) {
    for (size_t k = 0; k < album_thumb_gen_known_alias_count; k++) {
        const album_thumb_gen_alias_key_t * a = &album_thumb_gen_known_aliases[k];
        if (a->key != artist_key || a->kind != kind) continue;
        return !a->has_album;
    }
    return false;
}

static bool album_thumb_gen_warm_first_screens_run(int my_generation, int * warmed, int * skipped,
                                                    const char ** reason) {
    int epoch = atomic_load(&album_thumb_gen_cache_epoch);
    if (atomic_load(&album_thumb_gen_screens_epoch) == epoch) { *reason = "already"; return true; }
    if (!atomic_load(&artist_thumbnail_work_enabled)) { *reason = "disabled"; return true; }
    if (thumbnail_policy(LIBRARY_THUMB_POLICY_LAZY)) { *reason = "lazy"; return true; }
    if (audio_is_playing()) { *reason = "playing"; return true; } /* runs after playback stops, see the end of the pass */
    const metadata_db_group_kind_t kinds[] = { METADATA_DB_GROUP_ARTIST, METADATA_DB_GROUP_ALBUM_ARTIST };
    group_row_t rows[ALBUM_THUMB_GEN_SCREEN_ROWS];
    bool transient = false;
    for (size_t kind_index = 0; kind_index < sizeof(kinds) / sizeof(kinds[0]); kind_index++) {
        metadata_db_group_kind_t group_kind = kinds[kind_index];
        int n = metadata_db_get_groups_page(group_kind, 0, ALBUM_THUMB_GEN_SCREEN_ROWS, rows);
        for (int i = 0; i < n; i++) {
            if (album_thumb_gen_should_cancel(my_generation)) { *reason = "cancel"; return false; }
            if (!atomic_load(&artist_thumbnail_work_enabled)) { *reason = "disabled"; return true; }
            if (audio_is_playing()) { *reason = "playing"; return true; }
            if (album_thumbnail_lazy_work_pending() ||
                !artwork_check_memory_admission(ARTWORK_PRIO_WARMER, ALBUM_ART_METADATA_START_BYTES)) {
                atomic_store(&album_thumb_gen_retry_pending, true);
                *reason = "yield";
                return false;
            }
            uint64_t artist_key = albumart_artist_thumbnail_key(rows[i].name);
            if (album_thumb_gen_cached_key(THUMBNAIL_KEY_ARTIST, artist_key) ||
                album_thumb_gen_alias_row_warm(artist_key, group_kind)) {
                (*skipped)++;
                continue;
            }
            artist_thumbnail_resolution_t resolution;
            artist_thumbnail_result_t result = artist_thumbnail_load_or_decode(
                rows[i].first_song_id, group_kind, rows[i].name, ARTWORK_PRIO_THUMBNAIL, true,
                artist_key, album_thumb_gen_warm_cancel_cb, (void *) (intptr_t) my_generation, &resolution);
            if (result == ARTIST_THUMB_CANCELLED) {
                bool hard = album_thumb_gen_should_cancel(my_generation);
                if (!hard) atomic_store(&album_thumb_gen_retry_pending, true);
                free(resolution.pixels);
                *reason = hard ? "cancel" : audio_is_playing() ? "playing" : "yield";
                return false;
            }
            if (result == ARTIST_THUMB_TEMPORARY || result == ARTIST_THUMB_OWN_IMAGE ||
                !(resolution.has_alias || resolution.negative_confirmed || resolution.pixels)) {
                if (result == ARTIST_THUMB_TEMPORARY) {
                    atomic_store(&album_thumb_gen_retry_pending, true);
                    transient = true; /* this epoch is not warmed yet: the retry takes the row again */
                }
                free(resolution.pixels);
                (*skipped)++;
                continue;
            }
            bool has_pixels = resolution.pixels != NULL;
            thumbnail_key_kind_t pixel_kind = resolution.pixel_key_kind;
            uint64_t pixel_key = resolution.pixel_key;
            album_thumbnail_handoff_t handoff = artist_handoff(epoch, artist_key, group_kind,
                                                               rows[i].first_song_id, &resolution, true);
            if (!album_thumb_gen_handoff_result(my_generation, &handoff)) { *reason = "cancel"; return false; }
            /* A key the snapshot already holds is already in the budget (a rerun's duplicate is dropped
             * by the commit). */
            if (has_pixels && album_thumb_gen_note_cached(pixel_kind, pixel_key) &&
                album_thumb_gen_ram_filled < ALBUM_THUMBNAIL_CACHE_SIZE) album_thumb_gen_ram_filled++;
            (*warmed)++;
            usleep(ALBUM_THUMB_GEN_SCREEN_GAP_US);
        }
    }
    if (transient) { *reason = "transient"; return true; }
    atomic_store(&album_thumb_gen_screens_epoch, epoch);
    *reason = "done";
    return true;
}

bool album_thumb_gen_warm_first_screens(int my_generation) {
    int warmed = 0, skipped = 0;
    const char * reason = "";
    bool ok = album_thumb_gen_warm_first_screens_run(my_generation, &warmed, &skipped, &reason);
    if (strcmp(reason, "playing") == 0) album_thumb_gen_warm_waited_epoch = atomic_load(&album_thumb_gen_cache_epoch);
    if (strcmp(reason, "already") != 0)
        DB_LOG("ART_ARTIST", "first_screen_warm warmed=%d skipped=%d reason=%s", warmed, skipped, reason);
    return ok;
}
