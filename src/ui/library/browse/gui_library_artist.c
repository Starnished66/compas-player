/* The artist page: its albums, the artist-songs reader and the collection menus. */
#include "../../text/i18n/i18n.h"
#include "gui_library_internal.h"
#include "gui_artist_data.h"
#include "gui_artist_page.h"
#include "../../../core/process/worker_thread.h"
#include "../../diagnostics/ui_frame_trace.h"
#include "../library_thumbnails.h"
#include "../../../library/tagcache/core/tagcache.h"
#include "../../../core/ui/display/ui_wake.h"
#include "gui_library_async.h"
#include <limits.h>
#include <stdatomic.h>
#include <pthread.h>
#include <strings.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void artist_album_more_click_cb(int index);
static const char * artist_albums_song_filter(void);

/* Song count of the artist row that was just tapped (-1: unknown), so opening
 * the page does not look the group up again on the UI thread. */
int artist_open_song_hint = -1;

static metadata_db_album_sort_t artist_albums_current_sort;

lv_obj_t * collection_menu_popup = NULL;
lv_obj_t * collection_menu_backdrop = NULL;
lv_obj_t * album_collection_menu_popup = NULL;
lv_obj_t * album_collection_menu_backdrop = NULL;
static bool collection_menu_is_album;
static char collection_menu_name[128];
static char collection_menu_album_artist[128];
static metadata_db_group_kind_t collection_menu_artist_kind;
static int collection_menu_song_count;
/* Credited artist the album menu was opened under, empty when it was not
 * opened from an Artists drill-down. */
static char collection_menu_artist[128];

/* Drill-down target for Artists and Album Artists: tapping either shows the
 * artist's own albums (regrouping just their songs by album, via
 * build_groups_by_indices) rather than dumping straight into a flat song
 * list -- tapping an album from there lands on show_group_songs() same as
 * every other terminal list. One persistent screen shared by both callers,
 * same rebuild-in-place approach as group_songs_screen. */
static int artist_albums_group_count;
/* The compact-list page provider may run on its fetch worker while a rapid
 * drill-down replaces the screen's current artist. Snapshot both fields as
 * one pair so an abandoned fetch never observes a torn query. */
static pthread_mutex_t artist_albums_state_mutex = PTHREAD_MUTEX_INITIALIZER;
/* Name/kind of the artist or album artist this screen is currently showing
 * -- set by show_artist_albums(), read back by artist_album_row_click_cb()'s
 * own "All Songs" row (index 0, see that function's own comment) to fetch
 * every song credited to them, flattened across all their albums. */
static char artist_albums_current_name[128];
static metadata_db_group_kind_t artist_albums_current_kind;

static const char * artist_albums_song_filter(void) {
    return artist_albums_current_kind == METADATA_DB_GROUP_ARTIST ? artist_albums_current_name : NULL;
}


/* Now-playing indicator bar -- same "recreated fresh every populate call"
 * lifecycle as group_songs_now_playing_bar (see its own comment):
 * populate_indexed_list()'s lv_obj_clean(artist_albums_list) destroys
 * whatever was here before, same as every row. */
/* This drill-down is a compact list too: fixed row count regardless of how
 * many albums one artist owns, and therefore able to share the exact album
 * artwork decorator/cache used by Music -> Albums. */
/* Sort scratch for artist_albums_show_all_songs() below -- carries the same
 * owned path/title an eventual group_song_entry_t needs, plus just enough
 * of each song's own tags to order the flattened list the same way a real
 * album listing would (see cmp_artist_song_sort_entry()'s own comment).
 * Freed after the final group_song_entry_t[] is built; its path/title
 * pointers are MOVED there, not copied again. */
typedef struct {
    char * path;
    char * title;
    int64_t song_id;
    uint64_t artwork_key;
    char album[128];
    char album_artist[128];
    int32_t disc_number;
    int32_t track_number;
    int32_t album_rank; /* the album's place in the page's album order; 0 for every song when sorted by name */
    bool show_disc_header;
} artist_song_sort_entry_t;

/* Sorts songs by the album's rank (the page's album order; all equal when
 * sorted by name), then album name, album artist, disc number, track number,
 * and file path. Grouping by both album and album artist prevents collision
 * between same-titled albums from different artists, while sorting by disc
 * and track preserves listening order within each album. */
static int cmp_artist_song_sort_entry(const void * a, const void * b) {
    const artist_song_sort_entry_t * ea = (const artist_song_sort_entry_t *) a;
    const artist_song_sort_entry_t * eb = (const artist_song_sort_entry_t *) b;
    if (ea->album_rank != eb->album_rank) return ea->album_rank < eb->album_rank ? -1 : 1;
    int c = strcasecmp(ea->album, eb->album);
    if (c) return c;
    c = strcasecmp(ea->album_artist, eb->album_artist);
    if (c) return c;
    int32_t da = ea->disc_number > 0 ? ea->disc_number : 1;
    int32_t db = eb->disc_number > 0 ? eb->disc_number : 1;
    if (da != db) return da < db ? -1 : 1;
    int32_t ta = ea->track_number, tb = eb->track_number;
    if (ta > 0 || tb > 0) {
        if (ta <= 0) return 1;
        if (tb <= 0) return -1;
        if (ta != tb) return ta < tb ? -1 : 1;
    }
    return strcasecmp(ea->path, eb->path);
}

/* Fetches every song credited to artist_albums_current_name (flattened
 * across all its albums) and hands it to show_group_songs_take_ownership()
 * -- the "All Songs" row prepended at index 0 by show_artist_albums().
 *
 * Exact track count is queried via metadata_db_get_group_offset() /
 * metadata_db_get_groups_page() so buffer sizing matches accurately. */
#define ARTIST_ALBUMS_ALL_SONGS_CAP 4000

static group_song_entry_t * artist_songs_collect(const char * artist_name, metadata_db_group_kind_t artist_kind,
                                                 metadata_db_album_sort_t album_sort, const atomic_bool * cancel,
                                                 int * out_count, int * out_real_total) {
    *out_count = 0;
    int64_t offset = metadata_db_get_group_offset(artist_kind, artist_name, NULL);
    group_row_t artist_row;
    int real_total = (offset >= 0 && metadata_db_get_groups_page(artist_kind, (int) offset, 1,
                                                                  &artist_row) == 1)
                          ? artist_row.song_count
                          : 0;
    if (real_total <= 0) return NULL;
    int total = real_total > ARTIST_ALBUMS_ALL_SONGS_CAP ? ARTIST_ALBUMS_ALL_SONGS_CAP : real_total;

    /* calloc, not malloc -- a failure partway through the fetch loop below
     * frees every one of these `total` slots unconditionally (see that
     * branch's own comment), relying on the not-yet-reached ones staying
     * NULL from this zero-init rather than holding garbage. */
    artist_song_sort_entry_t * sort_entries = calloc((size_t) total, sizeof(*sort_entries));
    if (!sort_entries) return NULL;

    int n = 0;
    bool failed = false;
    song_row_t page[64];
    while (n < total) {
        if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) { failed = true; break; }
        int want = total - n;
        if (want > 64) want = 64;
        int got = artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                      ? metadata_db_get_album_artist_songs(artist_name, n, page, want)
                      : metadata_db_get_artist_songs(artist_name, n, page, want);
        if (got <= 0) break;
        for (int i = 0; i < got; i++) {
            char title[192];
            format_music_submenu_identity(&page[i], title, sizeof(title));
            artist_song_sort_entry_t * dst = &sort_entries[n + i];
            dst->path = strdup(page[i].path);
            dst->title = strdup(title);
            dst->song_id = page[i].id;
            dst->artwork_key = song_album_thumbnail_key(&page[i]);
            snprintf(dst->album, sizeof(dst->album), "%s", page[i].tags.album);
            snprintf(dst->album_artist, sizeof(dst->album_artist), "%s", page[i].tags.album_artist);
            dst->disc_number = page[i].tags.disc_number;
            dst->track_number = page[i].tags.track_number;
            if (!dst->path || !dst->title) {
                failed = true;
                break;
            }
        }
        if (failed) break;
        n += got;
        if (got < want) break;
    }

    if (failed || n <= 0) {
        /* `total`, not `n` -- covers the current, partially-filled batch
         * (whatever this failure landed on) plus every never-reached slot,
         * all still NULL/zeroed from the calloc above. free(NULL) is a
         * no-op, so this is safe regardless of exactly how far the loop
         * got. */
        for (int i = 0; i < total; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return NULL;
    }

    /* Rank each song's album by the order the artist page shows. One lookup per distinct
     * (album, album artist) pair; not-found albums rank after every found one. */
    if (album_sort != METADATA_DB_ALBUM_SORT_NAME) {
        struct { char album[128], album_artist[128]; int32_t rank; } * seen = NULL;
        int seen_count = 0, seen_cap = 0;
        for (int i = 0; i < n && !failed; i++) {
            if (cancel && atomic_load_explicit(cancel, memory_order_relaxed)) { failed = true; break; }
            artist_song_sort_entry_t * e = &sort_entries[i];
            int hit = -1;
            for (int k = seen_count - 1; k >= 0; k--) {
                if (strcmp(seen[k].album, e->album) == 0 && strcmp(seen[k].album_artist, e->album_artist) == 0) {
                    hit = k;
                    break;
                }
            }
            if (hit < 0) {
                if (seen_count == seen_cap) {
                    int cap = seen_cap ? seen_cap * 2 : 16;
                    void * grown = realloc(seen, (size_t) cap * sizeof(*seen));
                    if (!grown) { failed = true; break; }
                    seen = grown;
                    seen_cap = cap;
                }
                int64_t pos = metadata_db_get_album_for_group_sorted_offset(artist_kind, artist_name, album_sort,
                                                                            e->album, e->album_artist);
                snprintf(seen[seen_count].album, sizeof(seen[seen_count].album), "%s", e->album);
                snprintf(seen[seen_count].album_artist, sizeof(seen[seen_count].album_artist), "%s", e->album_artist);
                seen[seen_count].rank = pos < 0 || pos >= INT32_MAX ? INT32_MAX : (int32_t) pos;
                hit = seen_count++;
            }
            e->album_rank = seen[hit].rank;
        }
        free(seen);
        if (failed) {
            for (int i = 0; i < total; i++) {
                free(sort_entries[i].path);
                free(sort_entries[i].title);
            }
            free(sort_entries);
            return NULL;
        }
    }

    /* Sort by album order, then disc and track order within each album. */
    qsort(sort_entries, (size_t) n, sizeof(*sort_entries), cmp_artist_song_sort_entry);

    /* Disc headers belong to an album's own page; across an artist's catalog they
     * would interleave with other albums, so this list shows none. */
    for (int i = 0; i < n; i++) sort_entries[i].show_disc_header = false;

    /* Transfer ownership of path and title strings into a group_song_entry_t
     * array for show_group_songs_take_ownership() to avoid duplicate allocations. */
    group_song_entry_t * entries = malloc(sizeof(*entries) * (size_t) n);
    if (!entries) {
        for (int i = 0; i < n; i++) {
            free(sort_entries[i].path);
            free(sort_entries[i].title);
        }
        free(sort_entries);
        return NULL;
    }
    for (int i = 0; i < n; i++) {
        entries[i].path = sort_entries[i].path;
        entries[i].title = sort_entries[i].title;
        entries[i].song_id = sort_entries[i].song_id;
        entries[i].artwork_key = sort_entries[i].artwork_key;
        entries[i].disc_number = sort_entries[i].disc_number > 0 ? sort_entries[i].disc_number : 1;
        entries[i].show_disc_header = sort_entries[i].show_disc_header;
    }
    free(sort_entries);

    *out_count = n;
    *out_real_total = real_total;
    return entries;
}

/* Truncation notice, the refresh identity and the screen, for a collected list. */
static void artist_songs_show(const char * artist_name, metadata_db_group_kind_t artist_kind,
                              group_song_entry_t * entries, int n, int real_total, bool push_screen) {
    /* Disclose truncation via a toast if the songs exceed the display cap. */
    if (real_total > ARTIST_ALBUMS_ALL_SONGS_CAP) {
        char msg[96];
        snprintf(msg, sizeof(msg), TR("Showing first %d of %d songs"), n, real_total);
        show_info_toast(msg);
    }

    group_songs_refresh_source = GROUP_SONGS_REFRESH_ARTIST;
    if (artist_name != group_songs_refresh_artist_name)
        snprintf(group_songs_refresh_artist_name, sizeof(group_songs_refresh_artist_name), "%s",
                 artist_name);
    group_songs_refresh_artist_kind = artist_kind;
    show_group_songs_take_ownership(artist_name, entries, n, push_screen);
    /* entries is now owned by group_songs_entries -- do not free here. */
}

/* Plays an artist's collected songs. The shared Songs page is rewritten to them only when
 * `populate` allows it (the user is still on the artist page and no group page is shown or in
 * the back stack); otherwise they play straight from `entries`, which are then freed, and the
 * page, and any album load it waits for, are left alone. */
static void artist_play_collected(const char * name, metadata_db_group_kind_t kind, group_song_entry_t * entries,
                                  int n, int real_total, play_mode_t mode, bool populate) {
    if (populate && !library_screen_in_navigation(group_songs_screen)) {
        artist_songs_show(name, kind, entries, n, real_total, false);
        (void) play_current_group(mode);
        return;
    }
    if (real_total > ARTIST_ALBUMS_ALL_SONGS_CAP) {
        char msg[96];
        snprintf(msg, sizeof(msg), TR("Showing first %d of %d songs"), n, real_total);
        show_info_toast(msg);
    }
    (void) play_group_entries(entries, n, name, false, mode);
    free_group_song_entries(entries, n);
}

bool artist_albums_show_all_songs_internal(bool push_screen, const char * artist_name,
                                                    metadata_db_group_kind_t artist_kind) {
    int n = 0, real_total = 0;
    group_song_entry_t * entries = artist_songs_collect(artist_name, artist_kind, effective_group_album_sort(artist_kind), NULL,
                                                        &n, &real_total);
    if (!entries) return false;
    artist_songs_show(artist_name, artist_kind, entries, n, real_total, push_screen);
    return true;
}

/* The artist page's Songs row reads the artist's songs on a worker, so the page
 * does not freeze while hundreds of rows are queried, formatted and sorted. A
 * touch on the row starts the read (prefetch); the tap adopts it. Same rules as
 * the genre read: one at a time, the screen is pushed once the songs are in,
 * and leaving the page drops the result. Only the poll touches the LVGL tree. */
typedef struct {
    char name[128];
    metadata_db_group_kind_t kind;
    metadata_db_album_sort_t album_sort; /* the page's album order when the read was created (UI thread) */
    int32_t catalog_generation;
    lv_obj_t * origin;
    bool left, prefetch;
    bool autoplay;          /* Play or Shuffle asked for this read: play the page once it is shown */
    bool push_page;         /* with autoplay: the Songs page is also shown (the menu); not for the page's own buttons */
    play_mode_t play_mode;
    uint32_t prefetch_tick;
    bool landing_waiting;   /* the songs are in but a touch or a menu holds the push back */
    uint32_t landing_since;
    atomic_bool cancel, done;
    atomic_int fin;         /* worker/owner handoff, see gui_library_async.h */
    group_song_entry_t * entries;
    int count, real_total;
#ifdef UI_PERF_TRACE
    uint32_t read_us;       /* trace builds: how long the read took on the worker */
#endif
} artist_songs_request_t;
#define ARTIST_SONGS_PREFETCH_TTL_MS 3000
static artist_songs_request_t * artist_songs_job;
static pthread_t artist_songs_thread;
static lv_timer_t * artist_songs_timer;
static atomic_int artist_songs_orphans; /* given-up artist reads still running */
/* The latest Songs tap that arrived while a given-up read and a stalled active one already
 * exist: it starts when the active read ends. */
static struct {
    bool set;
    char name[128];
    metadata_db_group_kind_t kind;
    lv_obj_t * origin;
    bool autoplay;          /* the waiting tap is a Play or Shuffle of the artist page */
    play_mode_t play_mode;
    bool push_page;         /* a Songs tap is waiting too: open the page when the songs land */
} artist_songs_pending;
static void artist_songs_origin_unloaded_cb(lv_event_t * e);

/* Records a waiting Songs tap. A Play or Shuffle already waiting for the same artist keeps its
 * playback intent and gains the page: the tap opens Songs and the songs still play. */
static void artist_songs_set_pending(const char * name, metadata_db_group_kind_t kind) {
    bool keep_play = artist_songs_pending.set && artist_songs_pending.autoplay &&
                     artist_songs_pending.kind == kind && strcmp(artist_songs_pending.name, name) == 0;
    artist_songs_pending.set = true;
    artist_songs_pending.push_page = true;
    if (!keep_play) artist_songs_pending.autoplay = false;
    if (name != artist_songs_pending.name)
        snprintf(artist_songs_pending.name, sizeof(artist_songs_pending.name), "%s", name);
    artist_songs_pending.kind = kind;
    artist_songs_pending.origin = lv_screen_active();
    lv_obj_remove_event_cb(artist_songs_pending.origin, artist_songs_origin_unloaded_cb);
    lv_obj_add_event_cb(artist_songs_pending.origin, artist_songs_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
}
static bool artist_songs_launch(const char * name, metadata_db_group_kind_t kind, bool prefetch);
static void artist_songs_launch_pending(void);

static void free_artist_songs_request(artist_songs_request_t * request) {
    if (!request) return;
    free_group_song_entries(request->entries, request->count);
    free(request);
}

static void * artist_songs_worker(void * arg) {
    artist_songs_request_t * request = arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
#ifdef UI_PERF_TRACE
    uint64_t read_start = ui_perf_now_us();
#endif
    request->entries = artist_songs_collect(request->name, request->kind, request->album_sort, &request->cancel,
                                            &request->count, &request->real_total);
    if (atomic_load_explicit(&request->cancel, memory_order_relaxed)) {
        /* Given up or superseded while it ran: nothing of it is published. */
        free_group_song_entries(request->entries, request->count);
        request->entries = NULL;
        request->count = 0;
    }
#ifdef UI_PERF_TRACE
    request->read_us = (uint32_t) (ui_perf_now_us() - read_start);
#endif
    atomic_store_explicit(&request->done, true, memory_order_release);
    if (library_async_worker_done(&request->fin, &artist_songs_orphans)) free_artist_songs_request(request); /* the owner gave it up */
    ui_wake_notify();
    return NULL;
}

/* Starts the waiting tap's read; a waiting Play or Shuffle carries its playback intent with it. */
static void artist_songs_launch_pending(void) {
    artist_songs_pending.set = false;
    if (artist_songs_pending.origin != lv_screen_active()) return;
    if (!artist_songs_launch(artist_songs_pending.name, artist_songs_pending.kind, false) ||
        !artist_songs_pending.autoplay || !artist_songs_job)
        return;
    artist_songs_job->autoplay = true;
    artist_songs_job->push_page = artist_songs_pending.push_page;
    artist_songs_job->play_mode = artist_songs_pending.play_mode;
}

static void artist_songs_poll(lv_timer_t * timer) {
    artist_songs_request_t * job = artist_songs_job;
    if (!job) {
        if (artist_songs_pending.set) {
            /* The tap that found two stalled reads waits here for a slot (the last tap wins). */
            if (library_async_kind_saturated(&artist_songs_orphans)) {
                lv_timer_set_period(timer, POLL_SATURATED_MS);
                return;
            }
            lv_timer_set_period(timer, POLL_FAST_MS);
            artist_songs_launch_pending();
            if (artist_songs_job) return;
        }
        lv_timer_set_period(timer, POLL_FAST_MS);
        lv_timer_pause(timer);
        return;
    }
    if (job->prefetch) {
        if (lv_tick_elaps(job->prefetch_tick) > ARTIST_SONGS_PREFETCH_TTL_MS) {
            job->left = true;
            atomic_store_explicit(&job->cancel, true, memory_order_relaxed);
        }
        if (!job->left) return; /* waiting for a tap, finished or not */
    }
    if (!atomic_load_explicit(&job->done, memory_order_acquire)) return;
    if (gui_navigation_transition_in_progress()) return;
    if (!job->left && !atomic_load_explicit(&job->cancel, memory_order_relaxed) &&
        job->origin == lv_screen_active() && job->album_sort != effective_group_album_sort(job->kind)) {
        /* The album sort changed while this read ran (e.g. from the remote): the page now shows
         * another album order, so read again in that order with the same Songs/Play intent. */
        char name[sizeof(job->name)];
        snprintf(name, sizeof(name), "%s", job->name);
        metadata_db_group_kind_t kind = job->kind;
        bool autoplay = job->autoplay, push_page = job->push_page;
        play_mode_t play_mode = job->play_mode;
        pthread_join(artist_songs_thread, NULL); /* finished: returns at once */
        artist_songs_job = NULL;
        free_artist_songs_request(job);
        if (!artist_songs_launch(name, kind, false) || !autoplay) return;
        if (artist_songs_job) {
            artist_songs_job->autoplay = true;
            artist_songs_job->push_page = push_page;
            artist_songs_job->play_mode = play_mode;
        } else if (artist_songs_pending.set) {
            artist_songs_pending.autoplay = true;
            artist_songs_pending.push_page = push_page;
            artist_songs_pending.play_mode = play_mode;
        }
        return;
    }
    bool wanted = !job->left && !atomic_load_explicit(&job->cancel, memory_order_relaxed) && job->entries &&
                  !library_rescan_active &&
                  job->catalog_generation == tagcache_generation();
    landing_verdict_t verdict = wanted ? landing_verdict(job->origin, &job->landing_waiting, &job->landing_since)
                                       : LANDING_DROP;
    /* Playback is not a navigation: a Play or Shuffle the user asked for plays when its songs are
     * in, whatever the landing rules say about pushing the page (a held finger, a menu, another
     * screen). Only a changed catalog or a failed read ends it. */
    bool play_only = job->autoplay && verdict != LANDING_PUSH && job->entries && !library_rescan_active &&
                     !atomic_load_explicit(&job->cancel, memory_order_relaxed) &&
                     job->catalog_generation == tagcache_generation();
    if (play_only) verdict = LANDING_DROP;
    else if (verdict == LANDING_WAIT) return; /* the finished read waits for the pointer to be released */
    pthread_join(artist_songs_thread, NULL); /* finished: returns at once */
    artist_songs_job = NULL;
    lv_timer_pause(timer);
    if (verdict == LANDING_PUSH || play_only) {
#ifdef UI_PERF_TRACE
        ui_frame_trace_event(UI_FRAME_TRACE_EVENT_SPAN, UI_TRACE_SPAN_ARTIST_SONGS_READ, job->read_us);
#endif
        group_song_entry_t * entries = job->entries;
        int n = job->count;
        job->entries = NULL;
        job->count = 0;
        /* The shared Songs page is rewritten only where the user still is: the artist page that asked
         * for the Play, with no group page shown or in the back stack. Otherwise (the page was left,
         * a menu or a finger held the push back) the songs play straight from the collected entries
         * and the page, and any album load it waits for, are left alone. */
        if (play_only) {
            artist_play_collected(job->name, job->kind, entries, n, job->real_total, job->play_mode,
                                  !job->push_page && !job->left && job->origin == lv_screen_active());
        } else {
            if (job->autoplay && !job->push_page) {
                /* The artist page's own Play or Shuffle: the same navigation rule as a read that
                 * landed with a finger held (no group page is rewritten in the back stack). */
                artist_play_collected(job->name, job->kind, entries, n, job->real_total, job->play_mode,
                                      !job->left && job->origin == lv_screen_active());
            } else {
                artist_songs_show(job->name, job->kind, entries, n, job->real_total, verdict == LANDING_PUSH);
                if (job->autoplay) (void) play_current_group(job->play_mode);
            }
        }
    }
    free_artist_songs_request(job);
    if (artist_songs_pending.set) artist_songs_launch_pending();
}

void artist_songs_ready_timer(void) {
    if (artist_songs_timer && !lv_timer_get_paused(artist_songs_timer) &&
        (artist_songs_job ? atomic_load_explicit(&artist_songs_job->done, memory_order_acquire)
                          : (artist_songs_pending.set && !library_async_kind_saturated(&artist_songs_orphans))))
        lv_timer_ready(artist_songs_timer);
}

bool artist_songs_busy(void) {
    return artist_songs_job && !(artist_songs_job->prefetch &&
                                 atomic_load_explicit(&artist_songs_job->done, memory_order_acquire));
}

static void artist_songs_origin_unloaded_cb(lv_event_t * e) {
    if (artist_songs_pending.set && lv_event_get_target(e) == artist_songs_pending.origin)
        artist_songs_pending.set = false;
    if (!artist_songs_job || lv_event_get_target(e) != artist_songs_job->origin) return;
    artist_songs_job->left = true;
    if (artist_songs_job->autoplay) return; /* a Play the user asked for finishes */
    atomic_store_explicit(&artist_songs_job->cancel, true, memory_order_relaxed);
}

static lv_timer_t * artist_songs_press_timer;
void cancel_artist_songs_load(void) {
    artist_songs_pending.set = false;
    if (artist_songs_press_timer) { lv_timer_delete(artist_songs_press_timer); artist_songs_press_timer = NULL; }
    if (artist_songs_job) {
        /* Never joined here, see cancel_genre_load(). */
        artist_songs_job->autoplay = false;
        atomic_store_explicit(&artist_songs_job->cancel, true, memory_order_relaxed);
        pthread_detach(artist_songs_thread);
        if (library_async_abandon(&artist_songs_job->fin, &artist_songs_orphans)) free_artist_songs_request(artist_songs_job);
        artist_songs_job = NULL;
    }
    if (artist_songs_timer) lv_timer_delete(artist_songs_timer);
    artist_songs_timer = NULL;
}

static bool artist_songs_start(bool prefetch) {
    if (artist_songs_job && !atomic_load_explicit(&artist_songs_job->done, memory_order_acquire) &&
        library_async_kind_has_orphan(&artist_songs_orphans)) {
        /* A given-up read is still stalled and so is the active one: keep it, drop its result and
         * start this tap's read when it ends (the last tap wins) instead of adding a third. */
        artist_songs_job->left = true;
        artist_songs_job->autoplay = false; /* a superseded Play never plays (the newer tap wins) */
        atomic_store_explicit(&artist_songs_job->cancel, true, memory_order_relaxed);
        artist_songs_set_pending(artist_albums_current_name, artist_albums_current_kind);
        if (artist_songs_timer) lv_timer_resume(artist_songs_timer);
        return true;
    }
    cancel_artist_songs_load();
    return artist_songs_launch(artist_albums_current_name, artist_albums_current_kind, prefetch);
}

static bool artist_songs_launch(const char * name, metadata_db_group_kind_t kind, bool prefetch) {
    if (library_async_kind_saturated(&artist_songs_orphans)) {
        /* Two stalled reads already: a tap is kept as the pending request and starts when one ends. */
        if (prefetch) return false;
        if (!artist_songs_timer) artist_songs_timer = lv_timer_create(artist_songs_poll, 25, NULL);
        if (!artist_songs_timer) return false;
        artist_songs_set_pending(name, kind);
        lv_timer_resume(artist_songs_timer);
        return true;
    }
    artist_songs_request_t * request = calloc(1, sizeof(*request));
    if (!request) return false;
    snprintf(request->name, sizeof(request->name), "%s", name);
    request->kind = kind;
    request->album_sort = effective_group_album_sort(kind);
    request->catalog_generation = tagcache_generation();
    request->origin = lv_screen_active();
    request->prefetch = prefetch;
    request->prefetch_tick = lv_tick_get();
    atomic_init(&request->cancel, false);
    atomic_init(&request->done, false);
    if (!artist_songs_timer) artist_songs_timer = lv_timer_create(artist_songs_poll, 25, NULL);
    if (!artist_songs_timer || pthread_create(&artist_songs_thread, NULL, artist_songs_worker, request) != 0) {
        free(request);
        if (artist_songs_timer) { lv_timer_delete(artist_songs_timer); artist_songs_timer = NULL; }
        return false;
    }
    lv_timer_resume(artist_songs_timer);
    artist_songs_job = request;
    lv_obj_remove_event_cb(request->origin, artist_songs_origin_unloaded_cb);
    lv_obj_add_event_cb(request->origin, artist_songs_origin_unloaded_cb, LV_EVENT_SCREEN_UNLOADED, NULL);
    return true;
}

/* The songs a finished read of this artist already prepared (collected, sorted, disc-marked), for
 * Play and Shuffle from the artist menu: they need the same list the Songs page shows, so it is
 * taken over instead of read and sorted again. False when there is no such finished read. */
static bool artist_songs_take_prepared(const char * name, metadata_db_group_kind_t kind,
                                       group_song_entry_t ** entries, int * count, int * real_total) {
    artist_songs_request_t * job = artist_songs_job;
    if (!job || !atomic_load_explicit(&job->done, memory_order_acquire) ||
        atomic_load_explicit(&job->cancel, memory_order_relaxed) || job->left || !job->entries ||
        job->catalog_generation != tagcache_generation() || library_rescan_active || job->kind != kind ||
        job->album_sort != effective_group_album_sort(kind) || strcmp(job->name, name) != 0)
        return false;
    pthread_join(artist_songs_thread, NULL); /* finished: returns at once */
    *entries = job->entries;
    *count = job->count;
    *real_total = job->real_total;
    job->entries = NULL;
    job->count = 0;
    artist_songs_job = NULL;
    free_artist_songs_request(job);
    if (artist_songs_timer) lv_timer_delete(artist_songs_timer);
    artist_songs_timer = NULL;
    return true;
}

/* Play or Shuffle from the artist menu while this artist's read is still running: the read
 * is adopted with the playback intent and plays when it lands, instead of collecting the
 * songs again on the UI thread. False when there is no such read (a finished one is taken
 * by artist_songs_take_prepared(), no read at all is collected in place). */
static bool artist_songs_adopt_running(const char * name, metadata_db_group_kind_t kind, play_mode_t mode,
                                       bool push_page) {
    artist_songs_request_t * job = artist_songs_job;
    /* A read is adopted whatever its album order, so no Songs or Play intent is lost; a finished read
     * is adopted only when its order is stale (artist_songs_take_prepared() takes a current one).
     * artist_songs_poll() reads a stale one again in the page's order. */
    if (!job || !artist_songs_timer ||
        (atomic_load_explicit(&job->done, memory_order_acquire) &&
         job->album_sort == effective_group_album_sort(kind)) ||
        atomic_load_explicit(&job->cancel, memory_order_relaxed) || job->left ||
        job->origin != lv_screen_active() || job->catalog_generation != tagcache_generation() ||
        library_rescan_active || job->kind != kind || strcmp(job->name, name) != 0)
        return false;
    /* A read the user started with a Songs tap keeps its page: the tap still shows it. */
    bool keep_page = job->autoplay ? job->push_page : !job->prefetch;
    job->prefetch = false;
    job->autoplay = true;
    job->push_page = push_page || keep_page;
    job->play_mode = mode;
    lv_timer_resume(artist_songs_timer);
    lv_timer_ready(artist_songs_timer);
    return true;
}

/* Touch on the Songs row (true), or a press that cannot become a tap (false). */
static void artist_songs_press_notice_cb(lv_timer_t * timer) {
    (void) timer; /* one shot: LVGL deletes it after this call */
    artist_songs_press_timer = NULL;
    if (library_rescan_active || artist_songs_job || artist_songs_pending.set) return;
    (void) artist_songs_start(true);
}

static void artist_page_songs_press_cb(bool pressed) {
    if (artist_songs_press_timer) { lv_timer_delete(artist_songs_press_timer); artist_songs_press_timer = NULL; }
    if (!pressed) {
        if (artist_songs_job && artist_songs_job->prefetch) {
            artist_songs_job->left = true;
            atomic_store_explicit(&artist_songs_job->cancel, true, memory_order_relaxed);
        }
        return;
    }
    if (library_rescan_active || artist_songs_job || artist_songs_pending.set) return;
    /* After the pressed highlight has been drawn, not before it. */
    artist_songs_press_timer = lv_timer_create(artist_songs_press_notice_cb, 24, NULL);
    if (artist_songs_press_timer) lv_timer_set_repeat_count(artist_songs_press_timer, 1);
}

void artist_albums_scroll_begin_cb(lv_event_t * e) {
    (void) e;
    artist_page_songs_press_cb(false);
}

/* The Songs row tap: adopts the read the touch started, or starts one. */
static bool artist_albums_open_songs(void) {
    artist_songs_request_t * touched = artist_songs_job;
    if (touched && touched->prefetch && !touched->left && touched->origin == lv_screen_active() &&
        touched->catalog_generation == tagcache_generation() && touched->kind == artist_albums_current_kind &&
        strcmp(touched->name, artist_albums_current_name) == 0) {
        touched->prefetch = false;
        lv_timer_ready(artist_songs_timer);
        return true;
    }
    if (touched && !touched->prefetch && !touched->left && artist_songs_timer &&
        !atomic_load_explicit(&touched->cancel, memory_order_relaxed) && touched->origin == lv_screen_active() &&
        touched->catalog_generation == tagcache_generation() && touched->kind == artist_albums_current_kind &&
        strcmp(touched->name, artist_albums_current_name) == 0) {
        /* The same read is already on its way: a second tap adopts it. A read that Play or Shuffle
         * started (autoplay, no page) still gets the page the tap asked for. */
        if (touched->autoplay) touched->push_page = true;
        return true;
    }
    return artist_songs_start(false);
}

static bool artist_albums_show_all_songs(void) {
    return artist_albums_open_songs();
}

void artist_album_row_click_cb(int index) {
    if (library_rescan_active) return;
    /* The albums start at index 0; All Songs lives in the page header. */
    int group_index = index;
    if (group_index < 0 || group_index >= artist_albums_group_count) return;
    group_row_t group;
    if (!press_resolved_take(PRESS_SRC_ARTIST_ALBUMS, index, &group) &&
        metadata_db_get_albums_for_group_sorted(artist_albums_current_kind, artist_albums_current_name,
                                                effective_group_album_sort(artist_albums_current_kind), group_index, 1, &group) != 1) return;
    (void) show_album_group_filtered(&group, artist_albums_song_filter());
}

void artist_album_row_press_cb(int index, bool pressed) {
    press_resolved_clear();
    if (!pressed) { album_prefetch_cancel(); return; }
    if (library_rescan_active || index < 0 || index >= artist_albums_group_count) return;
    group_row_t group;
    if (metadata_db_get_albums_for_group_sorted(artist_albums_current_kind, artist_albums_current_name,
                                                effective_group_album_sort(artist_albums_current_kind), index, 1, &group) != 1) return;
    press_resolved_set(PRESS_SRC_ARTIST_ALBUMS, index, &group);
    album_prefetch_start(&group, artist_albums_song_filter());
}

void hide_collection_menu(void) {
    if (collection_menu_popup) lv_obj_add_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (collection_menu_backdrop) lv_obj_add_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_popup) lv_obj_add_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN);
    if (album_collection_menu_backdrop) lv_obj_add_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN);
}

static void collection_menu_backdrop_cb(lv_event_t * e) {
    if (lv_event_get_code(e) == LV_EVENT_CLICKED) hide_collection_menu();
}

static void show_collection_menu(bool album) {
    if (library_rescan_active) return;
    lv_obj_t * popup = album ? album_collection_menu_popup : collection_menu_popup;
    lv_obj_t * backdrop = album ? album_collection_menu_backdrop : collection_menu_backdrop;
    if (!popup || !backdrop) return;
    lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(popup, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(backdrop);
    lv_obj_move_foreground(popup);
}

/* Starts playback from a list of entries, independent of the shared Songs page. */
bool play_group_entries(const group_song_entry_t * entries, int count, const char * title,
                               bool is_album, play_mode_t mode) {
    if (count <= 0) return false;
    char ** paths = calloc((size_t) count, sizeof(*paths));
    if (!paths) return false;
    for (int i = 0; i < count; i++) {
        paths[i] = strdup(entries[i].path);
        if (!paths[i]) {
            for (int j = 0; j < count; j++) free(paths[j]);
            free(paths);
            return false;
        }
    }
    mode = collection_play_mode(mode);
    int start = (mode == PLAY_MODE_SHUFFLE || mode == PLAY_MODE_SHUFFLE_ONCE) ? rand() % count : 0;
    gui_player_set_play_mode(mode);
    set_player_source_group_songs_direct(entries, count, title ? title : "", start);
    current_settings.last_source_kind = is_album ? 2 : 0;
    snprintf(current_settings.last_source_name, sizeof(current_settings.last_source_name), "%s",
             is_album && title ? title : "");
    on_file_selected(paths, count, start);
    return true;
}

bool play_current_group(play_mode_t mode) {
    return play_group_entries(group_songs_entries, group_songs_count, group_songs_source_title,
                              group_songs_source_is_album, mode);
}

static bool load_collection_for_playback(bool * built_now) {
    if (!collection_menu_is_album) {
        /* The artist menu now also opens from the Artists and Album Artists
         * lists, so play the artist the menu was opened for, not whichever
         * artist's album list was visited last. */
        group_song_entry_t * prepared;
        int prepared_count, prepared_total;
        if (artist_songs_take_prepared(collection_menu_name, collection_menu_artist_kind, &prepared,
                                       &prepared_count, &prepared_total)) {
            artist_songs_show(collection_menu_name, collection_menu_artist_kind, prepared, prepared_count,
                              prepared_total, true);
            return true;
        }
        return artist_albums_show_all_songs_internal(true, collection_menu_name,
                                                     collection_menu_artist_kind);
    }
    group_row_t group = {0};
    snprintf(group.name, sizeof(group.name), "%s", collection_menu_name);
    snprintf(group.album_artist, sizeof(group.album_artist), "%s", collection_menu_album_artist);
    group.song_count = collection_menu_song_count;
    return show_album_group_ex(&group, collection_menu_artist[0] ? collection_menu_artist : NULL, built_now);
}

static void collection_play_requested(play_mode_t mode) {
    bool built_now = false;
    if (!collection_menu_is_album && artist_songs_adopt_running(collection_menu_name, collection_menu_artist_kind, mode, true))
        return; /* plays when the running read lands */
    if (!load_collection_for_playback(&built_now)) return;
    if (collection_menu_is_album) {
        album_load_request_t * request = album_load_pending ? album_load_pending : album_load_job;
        if (built_now) (void) play_current_group(mode); /* a finished read built the page already */
        else if (request && request->generation == album_load_generation) request->autoplay_mode = mode;
    } else {
        (void) play_current_group(mode);
    }
}

static void collection_play_shuffled_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    collection_play_requested(PLAY_MODE_SHUFFLE);
}

static void collection_play_sequential_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    collection_play_requested(PLAY_MODE_SEQUENTIAL);
}

static bool collection_song_at(int offset, song_row_t * song) {
    if (collection_menu_is_album) {
        /* collection_menu_song_count is the filtered total when the menu was
         * opened from an Artists drill-down, so the rows must be filtered the
         * same way or the offset indexes a different list. */
        if (collection_menu_artist[0])
            return metadata_db_get_songs_filtered_page(NULL, collection_menu_artist,
                                                       collection_menu_album_artist,
                                                       collection_menu_name, NULL, offset, 1, song) == 1;
        return metadata_db_get_album_songs(collection_menu_name, collection_menu_album_artist,
                                            offset, song, 1) == 1;
    }
    return (collection_menu_artist_kind == METADATA_DB_GROUP_ALBUM_ARTIST
                ? metadata_db_get_album_artist_songs(collection_menu_name, offset, song, 1)
                : metadata_db_get_artist_songs(collection_menu_name, offset, song, 1)) == 1;
}

static void collection_add_random_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_song_count <= 0) return;
    song_row_t song;
    if (collection_song_at(rand() % collection_menu_song_count, &song))
        gui_player_queue_add(song.path);
}

static void collection_add_album_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    int count = 0;
    group_song_entry_t * entries = load_album_entries_filtered(collection_menu_name,
                                                                collection_menu_album_artist,
                                                                collection_menu_song_count,
                                                                collection_menu_artist[0] ? collection_menu_artist : NULL,
                                                                &count, NULL);
    if (!entries) return;
    const char ** paths = malloc(sizeof(*paths) * (size_t) count);
    if (paths) {
        for (int i = 0; i < count; i++) paths[i] = entries[i].path;
        gui_player_queue_add_many(paths, count);
        free(paths);
    }
    free_group_song_entries(entries, count);
}

static void collection_refresh_metadata_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    hide_collection_menu();
    if (collection_menu_is_album)
        start_library_metadata_refresh_album(collection_menu_name, collection_menu_album_artist);
    else
        start_library_metadata_refresh_artist(collection_menu_artist_kind, collection_menu_name);
}

static void collection_reload_cover_cb(lv_event_t * e) {
    if (library_rescan_active || lv_event_get_code(e) != LV_EVENT_CLICKED || !collection_menu_is_album)
        return;
    hide_collection_menu();
    start_library_cover_reload_album(collection_menu_name, collection_menu_album_artist);
}

static void open_album_collection_menu(const group_row_t * group, const char * artist_filter) {
    collection_menu_is_album = true;
    snprintf(collection_menu_artist, sizeof(collection_menu_artist), "%s", artist_filter ? artist_filter : "");
    /* Match the row's own listing, so "add album" never queues tracks the
     * screen does not show. */
    if (collection_menu_artist[0]) {
        int64_t filtered = metadata_db_count_songs_filtered(NULL, collection_menu_artist,
                                                            group->album_artist, group->name, NULL);
        collection_menu_song_count = filtered > 0 && filtered <= INT_MAX ? (int) filtered : 0;
    } else {
        collection_menu_song_count = group->song_count;
    }
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    snprintf(collection_menu_album_artist, sizeof(collection_menu_album_artist), "%s", group->album_artist);
    show_collection_menu(true);
}

static void open_artist_collection_menu(const group_row_t * group, metadata_db_group_kind_t kind) {
    collection_menu_is_album = false;
    collection_menu_artist_kind = kind;
    collection_menu_song_count = group->song_count;
    snprintf(collection_menu_name, sizeof(collection_menu_name), "%s", group->name);
    collection_menu_album_artist[0] = '\0';
    collection_menu_artist[0] = '\0';
    show_collection_menu(false);
}

static void open_artist_collection_menu_at(metadata_db_group_kind_t kind,
                                           search_binding_id_t binding_id, int index) {
    index = search_remap_index(binding_id, index);
    group_row_t group;
    if (metadata_db_get_groups_page(kind, index, 1, &group) == 1)
        open_artist_collection_menu(&group, kind);
}

void artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

void artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ARTIST, SEARCH_BINDING_ARTISTS, index);
}

void album_artist_collection_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

void album_artist_collection_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_artist_collection_menu_at(METADATA_DB_GROUP_ALBUM_ARTIST, SEARCH_BINDING_ALBUM_ARTIST, index);
}

static void open_album_collection_menu_at(int index) {
    group_row_t group;
    if (resolve_album_display_row(index, &group))
        open_album_collection_menu(&group, NULL);
}

/* The long-press menu of an album by identity (Cover Flow); nothing if it is gone. */
void gui_library_open_album_menu(const char * name, const char * album_artist) {
    group_row_t group;
    if (library_rescan_active || !album_group_resolve(name, album_artist, &group)) return;
    open_album_collection_menu(&group, NULL);
}

void album_more_click_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

void album_row_long_press_cb(int index) {
    if (library_rescan_active) return;
    open_album_collection_menu_at(index);
}

static void artist_album_more_click_cb(int index) {
    if (library_rescan_active) return;
    int group_index = index;
    group_row_t group;
    if (group_index >= 0 && group_index < artist_albums_group_count &&
        metadata_db_get_albums_for_group_sorted(artist_albums_current_kind, artist_albums_current_name,
                                                 effective_group_album_sort(artist_albums_current_kind), group_index, 1, &group) == 1)
        open_album_collection_menu(&group, artist_albums_song_filter());
}

/* ---- Artist page header actions (gui_artist_page.c) ----------------------- */
/* Play and Shuffle: what the old "All Songs" row's list would play, started
 * straight away without leaving the page, like the album page's buttons. */
static void artist_page_play(play_mode_t mode) {
    if (library_rescan_active) return;
    /* The songs the page's own read already prepared, or is still reading, are taken over instead
     * of collected again on the UI thread. */
    group_song_entry_t * prepared;
    int prepared_count, prepared_total;
    if (artist_songs_take_prepared(artist_albums_current_name, artist_albums_current_kind, &prepared,
                                   &prepared_count, &prepared_total)) {
        artist_play_collected(artist_albums_current_name, artist_albums_current_kind, prepared, prepared_count,
                              prepared_total, mode, true);
        return;
    }
    if (artist_songs_adopt_running(artist_albums_current_name, artist_albums_current_kind, mode, false)) return;
    /* A Songs tap already waiting for this artist keeps its page when this Play joins it. */
    bool songs_waiting = artist_songs_pending.set && artist_songs_pending.push_page &&
                         artist_songs_pending.kind == artist_albums_current_kind &&
                         strcmp(artist_songs_pending.name, artist_albums_current_name) == 0;
    /* No read at all: start the artist read on the worker with the playback intent, so the UI thread
     * does not collect and sort up to 4000 songs. It plays when the songs land, by the same rules as
     * an adopted read (never repopulates another page, never pushes one). */
    if (artist_songs_start(false)) {
        artist_songs_request_t * job = artist_songs_job;
        if (artist_songs_pending.set) {
            /* Two reads already stalled: this Play waits for a slot instead of collecting up to 4000
             * songs on the UI thread (the last tap wins). */
            artist_songs_pending.autoplay = true;
            artist_songs_pending.play_mode = mode;
            artist_songs_pending.push_page = songs_waiting;
            return;
        }
        if (job && job->kind == artist_albums_current_kind &&
            strcmp(job->name, artist_albums_current_name) == 0) {
            job->prefetch = false;
            job->autoplay = true;
            job->push_page = false;
            job->play_mode = mode;
            lv_timer_resume(artist_songs_timer);
            lv_timer_ready(artist_songs_timer);
            return;
        }
    }
    int n = 0, real_total = 0;
    group_song_entry_t * entries = artist_songs_collect(artist_albums_current_name, artist_albums_current_kind,
                                                        effective_group_album_sort(artist_albums_current_kind), NULL,
                                                        &n, &real_total);
    if (!entries) return;
    artist_play_collected(artist_albums_current_name, artist_albums_current_kind, entries, n, real_total, mode, true);
}
static void artist_page_play_cb(void) { artist_page_play(PLAY_MODE_SEQUENTIAL); }
static void artist_page_shuffle_cb(void) { artist_page_play(PLAY_MODE_SHUFFLE); }
static void artist_page_songs_cb(void) {
    if (library_rescan_active) return;
    (void) artist_albums_show_all_songs();
}
/* Long press on the Songs row: the artist menu the old row's more button opened. */
static void artist_page_menu_cb(void) {
    if (library_rescan_active) return;
    int64_t offset = metadata_db_get_group_offset(artist_albums_current_kind, artist_albums_current_name, NULL);
    group_row_t group;
    if (offset < 0 || metadata_db_get_groups_page(artist_albums_current_kind, (int) offset, 1, &group) != 1) return;
    open_artist_collection_menu(&group, artist_albums_current_kind);
}

void build_collection_menus(void) {
    const menu_popup_row_t rows[] = {
        { TR("Play all shuffled"), collection_play_shuffled_cb, false },
        { TR("Play sequentially"), collection_play_sequential_cb, false },
        { TR("Add a random song to queue"), collection_add_random_cb, false },
        { TR("Refresh metadata"), collection_refresh_metadata_cb, false },
        { TR("Cancel"), collection_menu_backdrop_cb, false, true },
    };
    const menu_popup_row_t album_rows[] = {
        { TR("Play all shuffled"), collection_play_shuffled_cb, false },
        { TR("Play sequentially"), collection_play_sequential_cb, false },
        { TR("Add a random song to queue"), collection_add_random_cb, false },
        { TR("Add album to queue"), collection_add_album_cb, false },
        { TR("Reload cover"), collection_reload_cover_cb, false },
        { TR("Refresh metadata"), collection_refresh_metadata_cb, false },
        { TR("Cancel"), collection_menu_backdrop_cb, false, true },
    };
    collection_menu_popup = build_menu_popup(rows, (int) (sizeof(rows) / sizeof(rows[0])),
                                               collection_menu_backdrop_cb,
                                               &collection_menu_backdrop);
    album_collection_menu_popup = build_menu_popup(album_rows,
                                                     (int) (sizeof(album_rows) / sizeof(album_rows[0])),
                                                     collection_menu_backdrop_cb,
                                                     &album_collection_menu_backdrop);
}

/* A direct lv_image_dsc_t points into the bounded LRU's pixel allocation.
 * Refresh on every screen load so a list that was hidden while the other
 * album screen populated/evicted cache entries never redraws an old row
 * against an evicted descriptor. This also immediately picks up artwork
 * decoded while the list was off-screen. */
void refresh_artist_albums_now_playing_indicator(void) {
    if (!artist_albums_list) return;
    int32_t current_revision;
    bool have_revision = metadata_db_try_get_generation(&current_revision);
    int match = -1;
    pthread_mutex_lock(&now_playing_lookup_mutex);
    const now_playing_lookup_result_t * result = &now_playing_lookup_applied_result;
    if (now_playing_lookup_applied_valid &&
        strcmp(result->request.path, now_playing_path) == 0 &&
        (!have_revision || result->library_revision == current_revision) &&
        result->request.artist_albums_kind == artist_albums_current_kind &&
        result->request.artist_albums_sort == requested_group_album_sort(artist_albums_current_kind) &&
        strcmp(result->request.artist_albums_name, artist_albums_current_name) == 0 &&
        result->artist_albums_row > 0 &&
        result->artist_albums_row <= artist_albums_group_count)
        match = result->artist_albums_row - 1;
    pthread_mutex_unlock(&now_playing_lookup_mutex);
    compact_list_set_now_playing(artist_albums_list, match);
}

/* The subtitle a cached artist-album row shows now, from the page's in-memory album facts (what
 * artist_albums_fetch_page() would put there): lets the rows pick up facts that arrived after they
 * were fetched without another trip to the worker. */
bool artist_album_rederive_row(int index, compact_list_page_row_t * row, void * ctx) {
    (void) index; (void) ctx;
    char subtitle[sizeof(row->subtitle)];
    int year = 0, tracks = 0;
    subtitle[0] = 0;
    if (gui_artist_data_album_by_key(row->label, row->aux_key, &year, &tracks))
        gui_artist_page_album_subtitle(subtitle, sizeof(subtitle), year, tracks);
    if (!strcmp(subtitle, row->subtitle)) return false;
    memcpy(row->subtitle, subtitle, sizeof(row->subtitle));
    return true;
}

/* Paged artist-album rows keep the complete result set out of memory. Row
 * zero is the synthetic All Songs action; database offset zero starts at
 * display index one. */
static int artist_albums_fetch_page(void * ctx, int offset, int count, compact_list_page_row_t out_rows[]) {
    (void) ctx;
    char current_name[sizeof(artist_albums_current_name)];
    metadata_db_group_kind_t current_kind;
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(current_name, sizeof(current_name), "%s", artist_albums_current_name);
    current_kind = artist_albums_current_kind;
    metadata_db_album_sort_t current_sort = artist_albums_current_sort;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    const int prefix = 0; /* albums only: All Songs lives in the page header */
    if (count <= 0) return prefix;
    group_row_t * rows = malloc(sizeof(*rows) * (size_t) count);
    int n = rows ? metadata_db_get_albums_for_group_sorted(current_kind, current_name,
                                                            current_sort, offset,
                                                            count, rows) : 0;
    for (int i = 0; i < n; i++) {
        snprintf(out_rows[prefix + i].label, sizeof(out_rows[prefix + i].label), "%s", rows[i].name);
        out_rows[prefix + i].identity = rows[i].first_song_id;
        fill_album_cover_key(&out_rows[prefix + i], &rows[i]);
        snprintf(out_rows[prefix + i].trailing_asset, sizeof(out_rows[prefix + i].trailing_asset),
                 "%s", "navigation/more.png");
        /* "1999 · 8 tracks" once the page's single background pass has run. */
        out_rows[prefix + i].aux_key = gui_artist_data_album_key(rows[i].album_artist);
        int year = 0, tracks = 0;
        if (gui_artist_data_album(rows[i].name, rows[i].album_artist, &year, &tracks))
            gui_artist_page_album_subtitle(out_rows[prefix + i].subtitle, sizeof(out_rows[prefix + i].subtitle), year, tracks);
    }
    free(rows);
    return n + prefix;
}

void populate_artist_albums_view(const char * name, metadata_db_group_kind_t kind) {
    int song_hint = artist_open_song_hint;
    artist_open_song_hint = -1;
    metadata_db_album_sort_t sort = effective_group_album_sort(kind);
    pthread_mutex_lock(&artist_albums_state_mutex);
    snprintf(artist_albums_current_name, sizeof(artist_albums_current_name), "%s", name);
    artist_albums_current_kind = kind;
    artist_albums_current_sort = sort;
    pthread_mutex_unlock(&artist_albums_state_mutex);

    int64_t count64 = metadata_db_count_albums_for_group(kind, name);
    artist_albums_group_count = count64 > 0 && count64 <= INT_MAX ? (int) count64 : 0;

    lv_label_set_text(artist_albums_title_label, name);
    compact_list_set_paged_provider(artist_albums_list, artist_albums_fetch_page, NULL,
                                    artist_albums_group_count);
    compact_list_set_trailing_click(artist_albums_list, artist_album_more_click_cb);
    /* One worker pass over the artist's songs fills the subtitles and header. */
    gui_artist_data_open(kind, name, sort);
    int song_total = song_hint;
    if (song_total < 0) {
        int64_t group_offset = metadata_db_get_group_offset(kind, name, NULL);
        group_row_t artist_group;
        song_total = group_offset >= 0 && metadata_db_get_groups_page(kind, (int) group_offset, 1, &artist_group) == 1
                         ? artist_group.song_count : 0;
    }
    gui_artist_page_params_t header = {
        .name = name, .albums = artist_albums_group_count, .songs = song_total,
        .on_play = artist_page_play_cb, .on_shuffle = artist_page_shuffle_cb,
        .on_songs = artist_page_songs_cb, .on_menu = artist_page_menu_cb,
        .on_songs_press = artist_page_songs_press_cb,
    };
    gui_artist_page_build(artist_albums_list, &header);
    request_now_playing_indicator_lookup();
    refresh_artist_albums_now_playing_indicator();
}

void show_artist_albums(const char * name, metadata_db_group_kind_t kind) {
    populate_artist_albums_view(name, kind);
    nav_push(artist_albums_screen);
}

void artist_albums_snapshot(metadata_db_group_kind_t * kind, char * name, size_t name_size, int * group_count) {
    pthread_mutex_lock(&artist_albums_state_mutex);
    *kind = artist_albums_current_kind;
    snprintf(name, name_size, "%s", artist_albums_current_name);
    if (group_count) *group_count = artist_albums_group_count;
    pthread_mutex_unlock(&artist_albums_state_mutex);
}

bool library_collection_menu_visible(void) {
    return (collection_menu_popup && !lv_obj_has_flag(collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (collection_menu_backdrop && !lv_obj_has_flag(collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_popup && !lv_obj_has_flag(album_collection_menu_popup, LV_OBJ_FLAG_HIDDEN)) ||
           (album_collection_menu_backdrop &&
            !lv_obj_has_flag(album_collection_menu_backdrop, LV_OBJ_FLAG_HIDDEN));
}
