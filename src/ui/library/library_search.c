#include "library_search.h"
#include "../text/i18n/i18n.h"
#include "library_thumbnails.h"
#include "library_az_index.h"
#include "browse/gui_library.h"
#include "playlists/gui_playlists.h"
#include "../network/subsonic/gui_subsonic.h"
#include "../../network/services/subsonic_client.h"
#include "../../library/remote/subsonic/subsonic_cache.h"
#include "../shell/gui_shell.h"
#include "../../core/process/worker_thread.h"
#include "../../core/process/worker_notify.h"
#include "../shell/gui_navigation.h"
#include "../../core/text/utf8_util.h"
#include "../text/rendering/gui_text_input.h"
#include "../widgets/builders/screen_builders.h"
#include "../appearance/gui_theme.h"
#include "../../library/metadata/parsing/metadata.h"
#include "../../library/metadata/db/metadata_db.h"
#include "../../library/artwork/albumart/albumart.h"
#include "../assets/assets.h"
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern void enable_gesture_bubble_recursive(lv_obj_t *obj);

/* ---- Live search (Artists/Albums/Album Artist/All Songs/Files) --
 *
 * A search icon in the title row opens an inline search bar + the
 * existing T9 keypad at the bottom of the SAME screen (reparented via
 * t9_keypad_attach(), see its own doc comment) -- unlike this app's
 * existing modal show_text_entry() flow, the list stays visible and
 * narrows live between them as each character is typed, rather than
 * being hidden behind a full-screen keyboard. The search bar (bg_search.png)
 * is drawn as a full-title-row-width container created AFTER (so on top
 * of) the screen's own back button and title -- covering both rather than
 * needing direct references to hide them, and doubling as a tap-absorbing
 * surface so the covered back button can't be hit through it. The only
 * way out of search while it's open is the close ("x") button; the
 * screen's own back button becomes reachable again once that's tapped. */

#define SEARCH_BAR_Y STATUS_BAR_CLEARANCE
#if defined(BOARD_TEMPOTEC_V1)
/* Keep one complete result, including its top gap, above the keypad. */
#define SEARCH_BAR_HEIGHT LV_MIN(80, t9_keypad_get_grid_y() - SEARCH_BAR_Y - ui_list_row_height() - GUI_ROW_GAP)
#else
#define SEARCH_BAR_HEIGHT 80
#endif
#define SEARCH_BAR_EDGE_INSET BOARD_SCALE_PX(8)
#define SEARCH_BAR_CLOSE_SIZE BOARD_SCALE_PX(51)
#define SEARCH_BAR_TEXT_GAP BOARD_SCALE_PX(8)

static void search_bar_textarea_geometry(lv_obj_t * bar, int32_t * x, int32_t * y, int32_t * width) {
    int32_t bar_width = BOARD_SCREEN_WIDTH;
    if (bar) {
        lv_obj_update_layout(bar);
        bar_width = lv_obj_get_width(bar);
    }
    int32_t left = SEARCH_BAR_EDGE_INSET;
    int32_t close_left = bar_width - SEARCH_BAR_EDGE_INSET - SEARCH_BAR_CLOSE_SIZE;
    if (x) *x = left;
    if (y) *y = BOARD_SCALE_PX(14);
    if (width) {
        int32_t available = close_left - SEARCH_BAR_TEXT_GAP - left;
        *width = available > BOARD_SCALE_PX(40) ? available : BOARD_SCALE_PX(40);
    }
}

typedef struct {
    search_binding_id_t id;
    lv_obj_t *screen, *list, *search_btn, *search_bar;
    library_search_item_of_t item_of; /* NULL: label-only rows from name_of */
    library_search_name_of_t name_of; /* in-memory fallback, only used when db_backed is false */
    const int *count_ptr; /* likewise -- only read when db_backed is false */
    bool active, is_overlay_list, db_backed;
    int *filtered_indices; /* display index -> real index; NULL when not filtering */
    int filtered_count; /* only meaningful while filtered_indices != NULL */
    metadata_db_az_kind_t db_kind; /* only meaningful when db_backed */
    compact_list_fetch_page_cb_t restore_fetch_page; /* restored on ordinary close; only meaningful when db_backed */
    char (*filtered_labels)[COMPACT_LIST_LABEL_MAX]; /* Owned label storage backing eager list row pointers. The DB results have no
                                  * other long-lived home, so list rows must be cleared/replaced before this is freed. */
    char (*filtered_subtitles)[256]; /* Owned subtitles backing eager music search rows. */
    library_global_result_t *global_results;
    char global_server_key[SUBSONIC_CACHE_KEY_SIZE];
    int32_t global_generation;
    lv_obj_t *files_title, *files_options, *restore_thumbnail_list;
    bool (*files_options_wanted)(void);
    bool inset_search_beside_options;
    library_search_group_item_cb_t group_item;
    library_search_az_strip_cb_t az_strip_for_screen;
    library_search_refresh_now_playing_cb_t refresh_now_playing;
    library_search_global_click_cb_t global_click;
} search_binding_t;

static search_binding_t search_bindings[SEARCH_BINDING_COUNT];

static search_binding_t * find_search_binding_for_screen(lv_obj_t * screen) {
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        if (search_bindings[i].screen == screen) return &search_bindings[i];
    }
    return NULL;
}

int search_remap_index(search_binding_id_t binding_id, int display_index) {
    search_binding_t * b = &search_bindings[binding_id];
    if (b->filtered_indices) return b->filtered_indices[display_index];
    return display_index;
}

int library_search_find_filtered_row(search_binding_id_t binding_id, int real_index) {
    search_binding_t * b = &search_bindings[binding_id];
    if (!b->filtered_indices || real_index < 0) return -1;
    for (int i = 0; i < b->filtered_count; i++) {
        if (b->filtered_indices[i] == real_index) return i;
    }
    return -1;
}

bool library_search_showing_results(search_binding_id_t binding_id) {
    return search_bindings[binding_id].filtered_indices != NULL;
}


/* Plain case-insensitive substring match -- not strcasestr(), which needs
 * _GNU_SOURCE defined before <string.h> that this file doesn't set (and
 * musl guards the prototype on it -- confirmed via musl's own string.h).
 * Empty needle matches everything (the "search box is empty" case). */
static bool search_matches(const char * haystack, const char * needle) {
    if (!haystack) return false;
    if (!needle || !needle[0]) return true;
    size_t needle_len = strlen(needle);
    for (const char * h = haystack; *h; h++) {
        size_t i = 0;
        while (i < needle_len && h[i] != '\0' && tolower((unsigned char) h[i]) == tolower((unsigned char) needle[i])) i++;
        if (i == needle_len) return true;
    }
    return false;
}

/* Real, live results a search box could plausibly need to show at once --
 * matches this app's own bounded-search-cache convention (metadata_db_
 * search_songs()'s own cap, remote_control.c's LIBRARY_JSON_MAX_LIMIT).
 * Without this, typing a common single letter against a 30k+-song library
 * linearly scans the whole array and mallocs/copies a match-sized buffer on
 * every keystroke. compact_list_set_items() itself stays cheap at any
 * item_count (only ~20 real row widgets ever exist regardless, unlike
 * populate_indexed_list()'s one-widget-per-row cost), so this isn't a
 * crash risk -- but scrolling through thousands of results nobody will
 * ever reach the end of is a wasted allocation and a bad live-search feel
 * on every single character typed. */
#define SEARCH_RESULTS_MAX 200

/* ---- Live search: async DB query for db_backed bindings -------------
 * Debounces keystrokes via search_debounce_timer and offloads the database
 * search query to a worker pthread, keeping the UI responsive. In-memory
 * bindings search synchronously in search_apply_filter(). */
#define SEARCH_DEBOUNCE_MS 200

static lv_timer_t * search_debounce_timer;

typedef struct {
    metadata_db_az_kind_t db_kind;
    search_binding_id_t binding_id;
    bool server_active;
    subsonic_server_t server;
    int32_t db_generation;
    char unknown_artist[48];
    char unknown_album[48];
    char query[256];
    int group_count;
    struct {
        int index;
        const char *path;
    } group[];
} search_job_request_t;

static pthread_t search_job_thread;
static bool search_job_active = false;
static atomic_bool search_job_done_flag = false;
static uint64_t search_catalog_epoch;
static uint64_t search_job_catalog_epoch;
static int32_t search_job_db_generation;
static search_binding_t * search_job_for_binding;
static int search_job_result_count;
static metadata_db_search_hit_t search_job_result_hits[SEARCH_RESULTS_MAX];
static int64_t search_job_result_identity[SEARCH_RESULTS_MAX];
static uint64_t search_job_result_artwork_key[SEARCH_RESULTS_MAX];
static char search_job_result_titles[SEARCH_RESULTS_MAX][COMPACT_LIST_LABEL_MAX];
static char search_job_result_subtitles[SEARCH_RESULTS_MAX][256];
/* Heap, only while a global search is in flight; the binding takes it over. */
static library_global_result_t * global_job_results;
static int global_job_result_count;
static http_cancel_token_t search_job_cancel;
static bool search_job_cancel_initialized;
static char search_job_server_key[SUBSONIC_CACHE_KEY_SIZE];

static void search_job_finish(bool cancel) {
    if (!search_job_active) return;
    if (cancel && search_job_cancel_initialized) http_cancel_token_cancel(&search_job_cancel);
    pthread_join(search_job_thread, NULL);
    search_job_active = false;
    if (search_job_cancel_initialized) {
        http_cancel_token_destroy(&search_job_cancel);
        search_job_cancel_initialized = false;
    }
}

static bool search_job_pending_valid = false;
static char search_job_pending_query[256];
static search_binding_t * search_job_pending_binding;

void library_search_invalidate_catalog(void) {
    search_catalog_epoch++;
    search_job_pending_valid = false;
    if (search_job_active && search_job_for_binding &&
        search_job_for_binding->id == SEARCH_BINDING_GLOBAL && search_job_cancel_initialized)
        http_cancel_token_cancel(&search_job_cancel);
}

typedef struct {
    metadata_db_search_hit_t hit;
    int64_t identity;
    uint64_t artwork_key;
    char title[COMPACT_LIST_LABEL_MAX];
    char subtitle[256];
} search_result_sort_item_t;

static int cmp_search_result_recency(const void * a, const void * b) {
    const search_result_sort_item_t * sa = (const search_result_sort_item_t *) a;
    const search_result_sort_item_t * sb = (const search_result_sort_item_t *) b;
    if (sa->hit.offset < sb->hit.offset) return -1;
    if (sa->hit.offset > sb->hit.offset) return 1;
    return 0;
}

/* Capture translated fallbacks on the UI thread before worker formatting. */
static void search_format_subtitle(const char *artist, const char *album,
                                   const search_job_request_t *req,
                                   char *subtitle, size_t subtitle_size) {
    gui_library_format_artist_album_subtitle(
        artist && artist[0] ? artist : req->unknown_artist,
        album && album[0] ? album : req->unknown_album,
        subtitle, subtitle_size);
}

static void search_format_song_identity(const song_row_t *song, const search_job_request_t *req,
                                        char *title, size_t title_size,
                                        char *subtitle, size_t subtitle_size) {
    metadata_db_song_display_title(song, title, title_size);
    search_format_subtitle(song->tags.artist, song->tags.album, req, subtitle, subtitle_size);
}

static void global_add_heading(const char * label) {
    if (!global_job_results || global_job_result_count >= SEARCH_RESULTS_MAX) return;
    library_global_result_t * row = &global_job_results[global_job_result_count++];
    memset(row, 0, sizeof(*row));
    row->kind = LIBRARY_GLOBAL_HEADING;
    utf8_truncate_safe(row->label, label, sizeof(row->label));
}

static void global_add_result(library_global_result_kind_t kind, int offset, int64_t identity,
                              const char * label, const char * subtitle,
                              const char * target, const char * secondary,
                              uint64_t artwork_key, const char * artwork_name) {
    if (!global_job_results || global_job_result_count >= SEARCH_RESULTS_MAX) return;
    library_global_result_t * row = &global_job_results[global_job_result_count++];
    memset(row, 0, sizeof(*row));
    row->kind = kind;
    row->offset = offset;
    row->identity = identity;
    row->artwork_key = artwork_key;
    snprintf(row->artwork_name, sizeof(row->artwork_name), "%s", artwork_name ? artwork_name : "");
    utf8_truncate_safe(row->label, label ? label : "", sizeof(row->label));
    utf8_truncate_safe(row->subtitle, subtitle ? subtitle : "", sizeof(row->subtitle));
    snprintf(row->target, sizeof(row->target), "%s", target ? target : "");
    snprintf(row->secondary, sizeof(row->secondary), "%s", secondary ? secondary : "");
}

static void search_global_db_kind(metadata_db_az_kind_t db_kind, library_global_result_kind_t result_kind,
                                  const char * heading, const search_job_request_t *req) {
    metadata_db_search_hit_t hits[32];
    int count = metadata_db_search_names(db_kind, req->query, 32, hits);
    if (count <= 0) return;
    global_add_heading(heading);
    for (int i = 0; i < count && global_job_result_count < SEARCH_RESULTS_MAX; i++) {
        if (db_kind == METADATA_DB_AZ_ARTIST) {
            group_row_t group;
            if (metadata_db_get_groups_page(METADATA_DB_GROUP_ARTIST, hits[i].offset, 1, &group) == 1)
                global_add_result(result_kind, hits[i].offset, group.first_song_id, group.name, NULL, group.name, NULL,
                                  albumart_artist_thumbnail_key(group.name), group.name);
        } else if (db_kind == METADATA_DB_AZ_ALBUM) {
            group_row_t group;
            song_row_t first_song;
            if (metadata_db_get_albums_page_filtered(NULL, hits[i].offset, 1, &group) == 1 &&
                metadata_db_get_song_by_id(group.first_song_id, &first_song))
                global_add_result(result_kind, hits[i].offset, group.first_song_id, group.name,
                                  group.album_artist, group.name, group.album_artist,
                                  song_album_thumbnail_key(&first_song), NULL);
        } else {
            song_row_t song;
            if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL,
                    hits[i].offset, 1, &song) == 1) {
                char title[COMPACT_LIST_LABEL_MAX], subtitle[256];
                search_format_song_identity(&song, req, title, sizeof(title), subtitle, sizeof(subtitle));
                global_add_result(result_kind, hits[i].offset, song.id, title, subtitle, song.path, NULL,
                                  song_album_thumbnail_key(&song), NULL);
            }
        }
    }
}

static bool search_global_playlists(const char * query) {
    char ** paths = NULL;
    int count = 0;
    metadata_db_load_all_playlists(&paths, &count);
    int matched = 0;
    for (int i = 0; i < count && matched < 32; i++) {
        const char * name = strrchr(paths[i], '/');
        name = name ? name + 1 : paths[i];
        char label[COMPACT_LIST_LABEL_MAX];
        size_t len = strlen(name);
        size_t copy = len < sizeof(label) - 1 ? len : sizeof(label) - 1;
        memcpy(label, name, copy);
        label[copy] = '\0';
        if (search_matches(label, query)) {
            if (!matched) global_add_heading("Playlists");
            global_add_result(LIBRARY_GLOBAL_PLAYLIST, i, 0, label, NULL, paths[i], NULL, 0, NULL);
            matched++;
        }
    }
    for (int i = 0; i < count; i++) free(paths[i]);
    free(paths);
    return matched > 0;
}

/* The worker fills a SEARCH_RESULTS_MAX array; keep only the rows that matched. */
static void trim_global_results(void) {
    if (!global_job_results) return;
    size_t rows = global_job_result_count > 0 ? (size_t) global_job_result_count : 1;
    library_global_result_t * trimmed = realloc(global_job_results, rows * sizeof(*global_job_results));
    if (trimmed) global_job_results = trimmed;
}

static void search_global_worker(const search_job_request_t * req) {
    global_job_result_count = 0;
    free(global_job_results);
    global_job_results = malloc(sizeof(*global_job_results) * SEARCH_RESULTS_MAX);
    if (!global_job_results) return;
    search_global_db_kind(METADATA_DB_AZ_ARTIST, LIBRARY_GLOBAL_ARTIST, "Artists", req);
    search_global_db_kind(METADATA_DB_AZ_ALBUM, LIBRARY_GLOBAL_ALBUM, "Albums", req);
    search_global_db_kind(METADATA_DB_AZ_ALL_SONGS, LIBRARY_GLOBAL_SONG, "Songs", req);
    search_global_playlists(req->query);
    int32_t generation_after = 0;
    if (!metadata_db_try_get_generation(&generation_after) || generation_after != req->db_generation) {
        global_job_result_count = 0;
        return;
    }
    if (!req->server_active || global_job_result_count >= SEARCH_RESULTS_MAX - 1 ||
        (search_job_cancel_initialized && http_cancel_token_is_cancelled(&search_job_cancel))) return;
    subsonic_artist_t * artists = NULL;
    subsonic_album_t * albums = NULL;
    subsonic_song_t * songs = NULL;
    int artist_count = 0, album_count = 0, song_count = 0;
    bool server_ok = subsonic_search3(&req->server, req->query, 20, &artists, &artist_count,
                                      &albums, &album_count, &songs, &song_count,
                                      search_job_cancel_initialized ? &search_job_cancel : NULL);
    if (server_ok) {
        if (artist_count + album_count + song_count > 0) global_add_heading("Subsonic");
        for (int i = 0; i < artist_count && global_job_result_count < SEARCH_RESULTS_MAX; i++)
            global_add_result(LIBRARY_GLOBAL_SERVER_ARTIST, i, 0, artists[i].name, NULL, artists[i].id, "", 0, NULL);
        for (int i = 0; i < album_count && global_job_result_count < SEARCH_RESULTS_MAX; i++) {
            char subtitle[256];
            snprintf(subtitle, sizeof(subtitle), "%s", albums[i].artist);
            global_add_result(LIBRARY_GLOBAL_SERVER_ALBUM, i, 0, albums[i].name, subtitle, albums[i].id, albums[i].artist_id, 0, NULL);
        }
        for (int i = 0; i < song_count && global_job_result_count < SEARCH_RESULTS_MAX; i++) {
            char subtitle[256];
            search_format_subtitle(songs[i].artist, songs[i].album,
                                          req, subtitle, sizeof(subtitle));
            global_add_result(LIBRARY_GLOBAL_SERVER_SONG, i, 0, songs[i].title, subtitle, songs[i].id, songs[i].album, 0, NULL);
            global_job_results[global_job_result_count - 1].server_song = songs[i];
        }
    }
    free(artists); free(albums); free(songs);
    if (!metadata_db_try_get_generation(&generation_after) || generation_after != req->db_generation ||
        (search_job_cancel_initialized && http_cancel_token_is_cancelled(&search_job_cancel)))
        global_job_result_count = 0;
}

static void * search_job_thread_func(void * arg) {
    search_job_request_t * req = (search_job_request_t *) arg;
    worker_lower_priority(WORKER_NICE_BACKGROUND);
    if (req->binding_id == SEARCH_BINDING_GROUP_SONGS) {
        search_job_result_count = 0;
        song_row_t *songs = malloc((size_t)req->group_count * sizeof(*songs));
        const char *paths[SEARCH_RESULTS_MAX];
        if (songs) {
            for (int i = 0; i < req->group_count; i++) paths[i] = req->group[i].path;
            metadata_db_get_songs_by_paths(paths, req->group_count, songs);
            for (int i = 0; i < req->group_count; i++) {
                if (songs[i].id < 0) continue;
                int n = search_job_result_count++;
                search_job_result_hits[n].offset = req->group[i].index;
                search_format_subtitle(songs[i].tags.artist, songs[i].tags.album, req,
                    search_job_result_subtitles[n], sizeof(search_job_result_subtitles[n]));
            }
            free(songs);
        }
        free(req);
        atomic_store_explicit(&search_job_done_flag, true, memory_order_release);
        worker_notify_ui();
        return NULL;
    }
    if (req->binding_id == SEARCH_BINDING_GLOBAL) {
        search_global_worker(req);
        trim_global_results();
        atomic_store_explicit(&search_job_done_flag, true, memory_order_release);
        free(req);
        worker_notify_ui();
        return NULL;
    }
    search_job_result_count = metadata_db_search_names(req->db_kind, req->query, SEARCH_RESULTS_MAX, search_job_result_hits);
    for (int i = 0; i < search_job_result_count; i++) {
        search_job_result_identity[i] = 0;
        search_job_result_artwork_key[i] = 0;
        search_job_result_titles[i][0] = '\0';
        search_job_result_subtitles[i][0] = '\0';
        if (req->db_kind == METADATA_DB_AZ_ARTIST || req->db_kind == METADATA_DB_AZ_ALBUM_ARTIST) {
            group_row_t group;
            metadata_db_group_kind_t kind = req->db_kind == METADATA_DB_AZ_ARTIST
                                                ? METADATA_DB_GROUP_ARTIST
                                                : METADATA_DB_GROUP_ALBUM_ARTIST;
            if (metadata_db_get_groups_page(kind, search_job_result_hits[i].offset, 1, &group) == 1) {
                search_job_result_identity[i] = group.first_song_id;
                search_job_result_artwork_key[i] = albumart_artist_thumbnail_key(group.name);
            }
        } else if (req->db_kind == METADATA_DB_AZ_ALBUM) {
            group_row_t group;
            if (metadata_db_get_albums_page_filtered(NULL, search_job_result_hits[i].offset, 1, &group) == 1) {
                song_row_t song;
                if (metadata_db_get_song_by_id(group.first_song_id, &song)) {
                    search_job_result_identity[i] = song.id;
                    search_job_result_artwork_key[i] = song_album_thumbnail_key(&song);
                }
            }
        } else {
            song_row_t song;
            if (metadata_db_get_songs_filtered_page(NULL, NULL, NULL, NULL, NULL,
                    search_job_result_hits[i].offset, 1, &song) == 1) {
                search_job_result_identity[i] = song.id;
                search_job_result_artwork_key[i] = song_album_thumbnail_key(&song);
                search_format_song_identity(&song, req, search_job_result_titles[i],
                    sizeof(search_job_result_titles[i]), search_job_result_subtitles[i],
                    sizeof(search_job_result_subtitles[i]));
                if (req->binding_id == SEARCH_BINDING_RECENTLY_ADDED) {
                    int64_t rec_offset = metadata_db_get_song_recency_offset(song.path);
                    search_job_result_hits[i].offset = (rec_offset >= 0 && rec_offset <= INT_MAX) ? (int) rec_offset : INT_MAX;
                }
            }
        }
    }
    if (req->binding_id == SEARCH_BINDING_RECENTLY_ADDED && search_job_result_count > 1) {
        search_result_sort_item_t * sort_items = malloc(sizeof(search_result_sort_item_t) * (size_t) search_job_result_count);
        if (sort_items) {
            for (int i = 0; i < search_job_result_count; i++) {
                sort_items[i].hit = search_job_result_hits[i];
                sort_items[i].identity = search_job_result_identity[i];
                sort_items[i].artwork_key = search_job_result_artwork_key[i];
                memcpy(sort_items[i].title, search_job_result_titles[i], sizeof(sort_items[i].title));
                memcpy(sort_items[i].subtitle, search_job_result_subtitles[i], sizeof(sort_items[i].subtitle));
            }
            qsort(sort_items, (size_t) search_job_result_count, sizeof(search_result_sort_item_t), cmp_search_result_recency);
            for (int i = 0; i < search_job_result_count; i++) {
                search_job_result_hits[i] = sort_items[i].hit;
                search_job_result_identity[i] = sort_items[i].identity;
                search_job_result_artwork_key[i] = sort_items[i].artwork_key;
                memcpy(search_job_result_titles[i], sort_items[i].title, sizeof(search_job_result_titles[i]));
                memcpy(search_job_result_subtitles[i], sort_items[i].subtitle, sizeof(search_job_result_subtitles[i]));
            }
            free(sort_items);
        }
    }
    free(req);
    atomic_store_explicit(&search_job_done_flag, true, memory_order_release); /* written last -- poll_search_job() only checks this flag */
    worker_notify_ui();
    return NULL;
}

/* At most one job in flight -- a debounce fire arriving while the previous
 * one is still running replaces the pending request rather than queuing,
 * same "only the latest ever matters" shape as launch_cover_decode_req()'s
 * cover_decode_pending. */
static void launch_search_job(search_binding_t * b, const char * query) {
    if (search_job_active) {
        if (search_job_for_binding && search_job_for_binding->id == SEARCH_BINDING_GLOBAL &&
            search_job_cancel_initialized) http_cancel_token_cancel(&search_job_cancel);
        search_job_pending_binding = b;
        if (query != search_job_pending_query)
            snprintf(search_job_pending_query, sizeof(search_job_pending_query), "%s", query);
        search_job_pending_valid = true;
        return;
    }

    int missing[SEARCH_RESULTS_MAX], missing_count = 0;
    const char *paths[SEARCH_RESULTS_MAX];
    size_t path_bytes = 0;
    if (b->id == SEARCH_BINDING_GROUP_SONGS) {
        for (int i = 0; i < b->filtered_count; i++) {
            if (b->filtered_subtitles[i][0]) continue;
            library_search_group_item_t item = b->group_item(b->filtered_indices[i]);
            if (!item.path || !item.path[0]) continue;
            missing[missing_count] = i;
            paths[missing_count++] = item.path;
            path_bytes += strlen(item.path) + 1;
        }
        if (!missing_count) return;
    }
    search_job_request_t * req = malloc(sizeof(*req) +
        (size_t)missing_count * sizeof(req->group[0]) + path_bytes);
    if (!req) return;
    req->db_kind = b->db_kind;
    req->binding_id = (search_binding_id_t) (b - search_bindings);
    req->server_active = b->id == SEARCH_BINDING_GLOBAL &&
        gui_subsonic_session_active() && gui_shell_wifi_connected();
    if (req->server_active) {
        req->server = subsonic_server_from_settings();
        subsonic_cache_server_key(&req->server, search_job_server_key);
    } else search_job_server_key[0] = '\0';
    req->db_generation = 0;
    if (b->id == SEARCH_BINDING_GLOBAL) metadata_db_try_get_generation(&req->db_generation);
    utf8_truncate_safe(req->unknown_artist, TR("Unknown artist"), sizeof(req->unknown_artist));
    utf8_truncate_safe(req->unknown_album, TR("Unknown album"), sizeof(req->unknown_album));
    snprintf(req->query, sizeof(req->query), "%s", query);

    /* Only missing subtitles need a copied path; rows already belong to the UI. */
    req->group_count = missing_count;
    char *path_copy = (char *)(req->group + missing_count);
    for (int i = 0; i < missing_count; i++) {
        req->group[i].index = missing[i];
        req->group[i].path = path_copy;
        size_t size = strlen(paths[i]) + 1;
        memcpy(path_copy, paths[i], size);
        path_copy += size;
    }

    search_job_for_binding = b;
    search_job_catalog_epoch = search_catalog_epoch;
    search_job_db_generation = req->db_generation;
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_active = true;
    if (req->server_active) {
        http_cancel_token_init(&search_job_cancel);
        search_job_cancel_initialized = true;
    } else search_job_cancel_initialized = false;
    if (pthread_create(&search_job_thread, NULL, search_job_thread_func, req) != 0) {
        free(req);
        search_job_active = false;
        if (search_job_cancel_initialized) http_cancel_token_destroy(&search_job_cancel);
        search_job_cancel_initialized = false;
    }
}

/* Builds compact_list_item_t/filtered_indices/filtered_labels from a
 * finished search job's hits and applies them to b->list. */
static void search_apply_results_to_list(search_binding_t * b, const metadata_db_search_hit_t * hits,
                                         const int64_t * identities, const uint64_t * artwork_keys,
                                         const char titles[][COMPACT_LIST_LABEL_MAX],
                                         const char subtitles[][256],
                                         int matched) {
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (matched > 0 ? matched : 1));
    int * indices = malloc(sizeof(int) * (size_t) (matched > 0 ? matched : 1));
    char(*labels)[COMPACT_LIST_LABEL_MAX] = b->id == SEARCH_BINDING_GLOBAL ? NULL :
        malloc(sizeof(*labels) * (size_t) (matched > 0 ? matched : 1));
    char(*owned_subtitles)[256] = b->id == SEARCH_BINDING_GLOBAL ? NULL :
        malloc(sizeof(*owned_subtitles) * (size_t) (matched > 0 ? matched : 1));
    library_global_result_t * global_results = NULL;
    if (b->id == SEARCH_BINDING_GLOBAL) {
        global_results = global_job_results;
        global_job_results = NULL;
    }
    if (!items || !indices ||
        (b->id != SEARCH_BINDING_GLOBAL && (!labels || !owned_subtitles)) ||
        (b->id == SEARCH_BINDING_GLOBAL && !global_results)) {
        free(items);
        free(indices);
        free(labels);
        free(owned_subtitles);
        free(global_results);
        return;
    }
    for (int i = 0; i < matched; i++) {
        if (global_results) {
            library_global_result_t * result = &global_results[i];
            items[i] = (compact_list_item_t){
                .label = result->kind == LIBRARY_GLOBAL_HEADING ? TR(result->label) : result->label,
                .subtitle = result->subtitle[0] ? result->subtitle : NULL,
                .is_section_header = result->kind == LIBRARY_GLOBAL_HEADING,
                .identity = result->identity, .artwork_key = result->artwork_key,
                .artwork_name = result->artwork_name[0] ? result->label : NULL
            };
            indices[i] = i;
            continue;
        }
        const char * title = titles[i][0] ? titles[i] : hits[i].label;
        utf8_truncate_safe(labels[i], title, sizeof(labels[i]));
        memcpy(owned_subtitles[i], subtitles[i], sizeof(owned_subtitles[i]));
        items[i] = (compact_list_item_t) {
            .label = labels[i],
            .subtitle = owned_subtitles[i][0] ? owned_subtitles[i] : NULL,
            .identity = identities[i],
            .artwork_key = artwork_keys[i],
            .artwork_name = b->db_backed && (b->db_kind == METADATA_DB_AZ_ARTIST ||
                             b->db_kind == METADATA_DB_AZ_ALBUM_ARTIST) ? labels[i] : NULL
        };
        indices[i] = hits[i].offset;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    free(b->filtered_indices);
    free(b->filtered_labels);
    free(b->filtered_subtitles);
    free(b->global_results);
    b->filtered_indices = indices;
    b->filtered_labels = labels;
    b->filtered_subtitles = owned_subtitles;
    b->global_results = global_results;
    b->global_generation = global_results ? search_job_db_generation : 0;
    if (global_results && search_job_server_key[0])
        snprintf(b->global_server_key, sizeof(b->global_server_key), "%s", search_job_server_key);
    else b->global_server_key[0] = '\0';
    b->filtered_count = matched;
    b->refresh_now_playing();
}

/* Called every tick from update_timer_cb, same as poll_cover_decode()/poll_
 * lyrics_load(). Discards the result (never applies it) if the binding it
 * was for is no longer the active search -- the user may have closed
 * search, or switched to a different search-active screen, while the query
 * was in flight. */
void poll_search_job(void) {
    if (!search_job_active || !atomic_load_explicit(&search_job_done_flag, memory_order_acquire)) return;
    search_job_finish(false);

    bool session_current = true;
    if (search_job_for_binding->id == SEARCH_BINDING_GLOBAL && search_job_server_key[0]) {
        char active_key[SUBSONIC_CACHE_KEY_SIZE];
        subsonic_server_t active_server = subsonic_server_from_settings();
        subsonic_cache_server_key(&active_server, active_key);
        session_current = gui_subsonic_session_active() && gui_shell_wifi_connected() &&
                          strcmp(active_key, search_job_server_key) == 0;
    }
    int32_t current_generation = 0;
    bool generation_current = search_job_for_binding->id != SEARCH_BINDING_GLOBAL ||
        (metadata_db_try_get_generation(&current_generation) && current_generation == search_job_db_generation);
    bool can_apply = generation_current && search_job_catalog_epoch == search_catalog_epoch && !search_job_pending_valid &&
        search_job_for_binding->active && find_search_binding_for_screen(lv_screen_active()) == search_job_for_binding;
    if (can_apply) {
        if (search_job_for_binding->id == SEARCH_BINDING_GLOBAL && !session_current) {
            for (int i = global_job_result_count - 1; i >= 0; i--) {
                if (global_job_results[i].kind == LIBRARY_GLOBAL_HEADING &&
                    strcmp(global_job_results[i].label, "Subsonic") == 0) {
                    global_job_result_count = i;
                    break;
                }
            }
        }
        if (search_job_for_binding->id == SEARCH_BINDING_GLOBAL)
            search_apply_results_to_list(search_job_for_binding, NULL, NULL, NULL, NULL, NULL,
                                         global_job_result_count);
        else if (search_job_for_binding->id == SEARCH_BINDING_GROUP_SONGS) {
            search_binding_t *b = search_job_for_binding;
            for (int i = 0; i < search_job_result_count; i++) {
                int row = search_job_result_hits[i].offset;
                memcpy(b->filtered_subtitles[row], search_job_result_subtitles[i],
                       sizeof(b->filtered_subtitles[row]));
            }
            compact_list_refresh_visible(b->list);
        }
        else search_apply_results_to_list(search_job_for_binding, search_job_result_hits,
                                     search_job_result_identity, search_job_result_artwork_key,
                                     search_job_result_titles, search_job_result_subtitles,
                                     search_job_result_count);
    }
    free(global_job_results); /* results nobody took */
    global_job_results = NULL;

    if (search_job_pending_valid) {
        search_job_pending_valid = false;
        if (search_job_pending_binding->active &&
            find_search_binding_for_screen(lv_screen_active()) == search_job_pending_binding)
            launch_search_job(search_job_pending_binding, search_job_pending_query);
    }
}

/* Rebuilds binding->list contents to entries matching `query` via
 * compact_list_set_items(). An empty query clears the list.
 * `filtered_indices` maps display rows back to unfiltered indices for
 * search_remap_index(), capped at SEARCH_RESULTS_MAX matches.
 *
 * db_backed bindings query metadata_db_search_names() via a background
 * worker (launch_search_job()). In-memory bindings are filtered synchronously. */
static void search_apply_filter(search_binding_t * b, const char * query) {
    if (b->id == SEARCH_BINDING_GROUP_SONGS) library_search_invalidate_catalog();
    bool have_query = query && query[0];
    compact_list_set_empty_heading(b->list, have_query ? TR("No matches") : TR("Type to search"));

    if (b->db_backed || b->id == SEARCH_BINDING_GLOBAL) {
        if (!have_query) {
            library_search_invalidate_catalog();
            compact_list_set_items(b->list, NULL, 0);
            free(b->filtered_indices);
            b->filtered_indices = NULL;
            b->filtered_count = 0;
            free(b->filtered_labels);
            b->filtered_labels = NULL;
            free(b->filtered_subtitles);
            b->filtered_subtitles = NULL;
            free(b->global_results);
            b->global_results = NULL;
            b->refresh_now_playing();
            return;
        }
        /* Deliberately does NOT clear filtered_indices/filtered_labels
         * here -- the previous result set stays on screen until the new
         * one actually lands (search_apply_results_to_list(), above),
         * rather than flashing to empty for however long the background
         * query takes. */
        launch_search_job(b, query);
        return;
    }

    int count = have_query ? *b->count_ptr : 0;
    int cap = count < SEARCH_RESULTS_MAX ? count : SEARCH_RESULTS_MAX;
    compact_list_item_t * items = malloc(sizeof(compact_list_item_t) * (size_t) (cap > 0 ? cap : 1));
    int * indices = malloc(sizeof(int) * (size_t) (cap > 0 ? cap : 1));
    char(*subtitles)[256] = b->id == SEARCH_BINDING_GROUP_SONGS
        ? calloc((size_t) (cap > 0 ? cap : 1), sizeof(*subtitles)) : NULL;
    if (!items || !indices || (b->id == SEARCH_BINDING_GROUP_SONGS && !subtitles)) {
        free(items);
        free(indices);
        free(subtitles);
        return;
    }
    int matched = 0;

    for (int i = 0; i < count && matched < cap; i++) {
        const char * name = b->name_of(i);
        if (!search_matches(name, query)) {
            if (b->id == SEARCH_BINDING_GROUP_SONGS) {
                /* An untagged song shows its file name: match that, not the
                 * folders every path shares. */
                library_search_group_item_t item = b->group_item ? b->group_item(i) : (library_search_group_item_t){0};
                const char * path = item.path;
                const char * file = path ? strrchr(path, '/') : NULL;
                if (!search_matches(file ? file + 1 : path, query)) continue;
            } else {
                continue;
            }
        }
        if (b->id == SEARCH_BINDING_GROUP_SONGS) {
            library_search_group_item_t item = b->group_item ? b->group_item(i) : (library_search_group_item_t){0};
            items[matched] = (compact_list_item_t){
                .label = name,
                .identity = item.song_id,
                .artwork_key = item.artwork_key,
            };
            items[matched].subtitle = subtitles[matched];
            if (item.subtitle[0]) {
                snprintf(subtitles[matched], sizeof(subtitles[matched]), "%s", item.subtitle);
            }
        } else {
            items[matched] = b->item_of ? b->item_of(i) : (compact_list_item_t){ .label = name };
        }
        indices[matched] = i;
        matched++;
    }

    compact_list_set_items(b->list, items, matched);
    free(items);

    free(b->filtered_indices);
    b->filtered_indices = NULL;
    free(b->filtered_labels);
    free(b->filtered_subtitles);
    b->filtered_labels = NULL;
    b->filtered_count = 0;

    if (have_query) {
        b->filtered_indices = indices;
        b->filtered_count = matched;
        b->filtered_subtitles = subtitles;
    } else {
        /* Unfiltered -- display index already equals the real index, no
         * remap table needed (also covers count == 0 cleanly). */
        free(indices);
        free(subtitles);
        b->filtered_subtitles = NULL;
    }
    b->refresh_now_playing();
    if (b->id == SEARCH_BINDING_GROUP_SONGS && matched) launch_search_job(b, query);
}

/* Fires once SEARCH_DEBOUNCE_MS after the last keystroke -- re-checks
 * everything fresh (not captured at keystroke time) since by definition no
 * newer keystroke has arrived without cancelling and recreating this timer
 * first (see search_textarea_value_changed_cb() below). */
static void search_debounce_timer_cb(lv_timer_t * timer) {
    (void) timer;
    search_debounce_timer = NULL;
    if (!t9_keypad_is_inline_active()) return;
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b) return;
    search_apply_filter(b, t9_keypad_get_text());
}

void search_textarea_value_changed_cb(lv_event_t * e) {
    (void) e;
    if (!t9_keypad_is_inline_active()) return;
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }
    search_binding_t *b = find_search_binding_for_screen(lv_screen_active());
    if (b && b->id == SEARCH_BINDING_GROUP_SONGS) {
        search_apply_filter(b, t9_keypad_get_text());
        return;
    }
    search_debounce_timer = lv_timer_create(search_debounce_timer_cb, SEARCH_DEBOUNCE_MS, NULL);
    lv_timer_set_repeat_count(search_debounce_timer, 1);
}

/* Restores a binding's list to its normal full-height, bottom-anchored
 * layout -- the exact geometry build_compact_list_screen() itself uses. */
static void search_restore_list_geometry(lv_obj_t * list) {
    lv_obj_set_size(list, lv_pct(100),
                     lv_display_get_vertical_resolution(lv_display_get_default()) - STATUS_BAR_CLEARANCE -
                         TITLE_ROW_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(list, LV_ALIGN_BOTTOM_MID, 0, -HOME_INDICATOR_CONTENT_INSET);
}

/* Called by the shared T9 Enter handler before it reparents the keypad away
 * from the active library screen. Keep the live query/filter and expand the
 * list into the space the keypad occupied. */
void search_keypad_dismissed(void) {
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b || !b->active) return;
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
        search_apply_filter(b, t9_keypad_get_text());
    }
    lv_obj_set_size(b->list, lv_pct(100),
                   lv_display_get_vertical_resolution(lv_display_get_default()) -
                       SEARCH_BAR_Y - SEARCH_BAR_HEIGHT - HOME_INDICATOR_CONTENT_INSET);
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);
}

/* Enter leaves the query textarea in its search bar while returning the
 * shared keypad to the modal text-entry screen. A tap in that same field
 * should restore the inline layout and keypad without resetting the query. */
void search_textarea_clicked_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED || t9_keypad_is_inline_active()) return;
    search_binding_t * b = find_search_binding_for_screen(lv_screen_active());
    if (!b || !b->active || lv_obj_get_parent(lv_event_get_target(e)) != b->search_bar) return;

    lv_obj_set_size(b->list, lv_pct(100), t9_keypad_get_grid_y() - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);
    int32_t textarea_x, textarea_y, textarea_width;
    search_bar_textarea_geometry(b->search_bar, &textarea_x, &textarea_y, &textarea_width);
    t9_keypad_attach(b->screen, b->search_bar, textarea_x, textarea_y, textarea_width, false);
}

/* Enter's meaning while a search binding owns the keypad: hide just the
 * keypad (search bar, typed query, and the current filter all stay) and
 * give the list back the vertical space the keypad occupied -- matches a
 * normal search box where dismissing the on-screen keyboard doesn't clear
 * what you searched for. text_entry_textarea is deliberately NOT
 * reparented back here (unlike the full t9_keypad_release()) so the typed
 * query stays visible in the search bar. */

static void search_open(search_binding_t * b) {
    /* db_backed bindings (Artists/Albums/Album Artist/All Songs) query
     * metadata_db_search_names() directly -- see search_apply_filter()'s own
     * comment -- so opening search on any of them never needs this. Only
     * Files/Subsonic still scan the in-memory name_of/count_ptr arrays. */
    lv_obj_add_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
#if defined(BOARD_TEMPOTEC_V1)
    lv_obj_set_height(b->search_bar, SEARCH_BAR_HEIGHT);
#endif
    if (b->id == SEARCH_BINDING_FILES && b->files_options)
        lv_obj_add_flag(b->files_options, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) lv_obj_remove_flag(b->list, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *az = b->az_strip_for_screen(b->screen);
    if (az) lv_obj_add_flag(az, LV_OBJ_FLAG_HIDDEN);

    /* Room for the search bar above AND the keypad below -- the keypad
     * attaches at its own fixed TEXT_ENTRY_GRID_X/Y position regardless of
     * which screen it's on, so that's the exact number to stop above. */
    lv_obj_set_size(b->list, lv_pct(100), t9_keypad_get_grid_y() - (SEARCH_BAR_Y + SEARCH_BAR_HEIGHT));
    lv_obj_align(b->list, LV_ALIGN_TOP_MID, 0, SEARCH_BAR_Y + SEARCH_BAR_HEIGHT);

    /* The text field ends immediately before the right-aligned close button. */
    int32_t textarea_x, textarea_y, textarea_width;
    search_bar_textarea_geometry(b->search_bar, &textarea_x, &textarea_y, &textarea_width);
    t9_keypad_attach(b->screen, b->search_bar, textarea_x, textarea_y, textarea_width, true);

    b->active = true;
    if (b->is_overlay_list && (b->id == SEARCH_BINDING_FILES || b->id == SEARCH_BINDING_GROUP_SONGS)) library_thumbnails_begin_view(b->list);
    search_apply_filter(b, ""); /* start unfiltered -- also (re)establishes filtered_indices == NULL */
}

static void search_close_internal(search_binding_t * b, bool restore_provider) {
    if (search_job_active && search_job_for_binding == b && search_job_cancel_initialized)
        http_cancel_token_cancel(&search_job_cancel);
    t9_keypad_release();
    if (b->is_overlay_list) {
        if (b->id == SEARCH_BINDING_FILES) {
            library_thumbnails_end_view(b->list);
        } else if (b->id == SEARCH_BINDING_GROUP_SONGS) {
            library_thumbnails_end_view(b->list);
            library_thumbnails_begin_view(b->restore_thumbnail_list);
        }
    }

    /* Not strictly required for correctness -- poll_search_job()/search_
     * debounce_timer_cb() both already re-check b->active/find_search_
     * binding_for_screen() at fire time and no-op harmlessly if this
     * binding is no longer the active search -- but cancelling outright
     * avoids a pointless wait for a query result nobody will ever see. */
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }

    lv_obj_add_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    if (b->id == SEARCH_BINDING_FILES && b->files_options &&
        (!b->files_options_wanted || b->files_options_wanted()))
        lv_obj_remove_flag(b->files_options, LV_OBJ_FLAG_HIDDEN);

    if (b->is_overlay_list) {
        lv_obj_add_flag(b->list, LV_OBJ_FLAG_HIDDEN); /* reveals file_browser.c's own folder-browsing UI underneath again */
    } else {
        search_restore_list_geometry(b->list);
    }

    compact_list_set_empty_heading(b->list,
        b->id == SEARCH_BINDING_GLOBAL ? TR("Type to search") : NULL);

    /* Eager search rows can point into filtered_labels. Drop those rows
     * before releasing the labels, including for a hidden overlay list. */
    compact_list_set_items(b->list, NULL, 0);
    free(b->filtered_indices);
    b->filtered_indices = NULL;
    b->filtered_count = 0;
    free(b->filtered_labels);
    b->filtered_labels = NULL;
    free(b->filtered_subtitles);
    b->filtered_subtitles = NULL;
    free(b->global_results);
    b->global_results = NULL;
    b->global_server_key[0] = '\0';
    b->global_generation = 0;

    if (b->db_backed && b->id != SEARCH_BINDING_GLOBAL) {
        if (restore_provider) {
            /* Switches the list back to paged mode instead of rebuilding a
             * static items[] array. */
            int artist_count = 0, album_artist_count = 0, album_count = 0;
            metadata_db_get_group_counts(&artist_count, &album_artist_count, &album_count);
            int total = 0;
            switch (b->db_kind) {
                case METADATA_DB_AZ_ARTIST: total = artist_count; break;
                case METADATA_DB_AZ_ALBUM_ARTIST: total = album_artist_count; break;
                case METADATA_DB_AZ_ALBUM: total = gui_library_albums_paged_total(album_count); break;
                case METADATA_DB_AZ_ALL_SONGS:
                    total = b->id == SEARCH_BINDING_ALL_SONGS ? all_songs_paged_total()
                                                             : (int)metadata_db_get_song_count();
                    break;
            }
            compact_list_set_paged_provider(b->list, b->restore_fetch_page, NULL, total);
        }
    } else if (b->id == SEARCH_BINDING_GLOBAL) {
        /* The shared result cleanup above already released global rows. */
    } else if (!b->is_overlay_list) {
        compact_list_item_t * items = NULL;
        int count = *b->count_ptr;
        if (count > 0) {
            items = malloc(sizeof(compact_list_item_t) * (size_t) count);
            for (int i = 0; i < count; i++)
                items[i] = b->item_of ? b->item_of(i) : (compact_list_item_t){ .label = b->name_of(i) };
        }
        compact_list_set_items(b->list, items, count);
        free(items);
    }

    b->active = false;
    /* After `active` clears, so the strip's own eligibility sees search as over. */
    lv_obj_t *az = b->az_strip_for_screen(b->screen);
    if (az && library_az_index_list_allowed(b->list)) lv_obj_remove_flag(az, LV_OBJ_FLAG_HIDDEN);
    b->refresh_now_playing();
}

static void search_close(search_binding_t * b) {
    search_close_internal(b, true);
}

/* screen_gesture_event_cb()'s back-swipe hook -- see its own forward
 * declaration for why. */
bool search_close_if_active_for_screen(lv_obj_t * screen) {
    search_binding_t * b = find_search_binding_for_screen(screen);
    if (!b || !b->active) return false;
    search_close(b);
    return true;
}

void library_search_invalidate_bindings(void) {
    if (search_debounce_timer) {
        lv_timer_delete(search_debounce_timer);
        search_debounce_timer = NULL;
    }
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++) {
        search_binding_t * b = &search_bindings[i];
        if ((!b->db_backed && b->id != SEARCH_BINDING_GROUP_SONGS && b->id != SEARCH_BINDING_GLOBAL) || !b->list) continue;
        if (b->active) search_close_internal(b, false);
        else if (b->filtered_labels || b->filtered_indices || b->filtered_subtitles) {
            /* Files results can be hidden while their eager items still
             * point into filtered_labels, so clear the list before freeing.
             * A binding without results is left alone: clearing a paged
             * library list here reset its scroll before rearm_library_list()
             * could keep it. */
            compact_list_set_items(b->list, NULL, 0);
            free(b->filtered_indices);
            b->filtered_indices = NULL;
            b->filtered_count = 0;
            free(b->filtered_labels);
            b->filtered_labels = NULL;
            free(b->filtered_subtitles);
            b->filtered_subtitles = NULL;
            free(b->global_results);
            b->global_results = NULL;
        }
    }
}

static void search_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_open(&search_bindings[id]);
}

static void search_close_btn_click_cb(lv_event_t * e) {
    if (lv_event_get_code(e) != LV_EVENT_CLICKED) return;
    search_binding_id_t id = (search_binding_id_t) (intptr_t) lv_event_get_user_data(e);
    search_close(&search_bindings[id]);
}

/* Called once per screen right after that screen (and its list) is built,
 * same two call sites (boot-time gui_init() + post-rescan rebuild) the
 * A-Z index already registers at. Builds the initial search icon (top-
 * right of the title row) and the search bar (hidden until search_btn is
 * tapped). */
void library_search_register(search_binding_id_t id, const library_search_config_t *config) {
    search_binding_t * b = &search_bindings[id];
    free(b->filtered_indices); /* A rebuild replaces the fixed id after the old screen is gone. */
    free(b->filtered_labels);
    free(b->filtered_subtitles);
    free(b->global_results);
    lv_obj_t *screen = config->screen;
    lv_obj_t *list = config->list;
    lv_obj_t *search_btn = build_top_right_icon_button(screen, asset_path("navigation/search.png"), NULL);
    lv_obj_add_event_cb(search_btn, search_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);
    lv_obj_add_flag(search_btn, LV_OBJ_FLAG_GESTURE_BUBBLE); /* added after finalize_screen_navigation()'s one-time pass, needs this set explicitly -- see screen_gesture_event_cb()'s own comment */
    if (id == SEARCH_BINDING_FILES && config->inset_search_beside_options) {
        /* Keep Files search beside its folder action, with the options icon
         * at the right edge. The title ends before Search. */
        lv_obj_align(search_btn, LV_ALIGN_TOP_RIGHT, -HEADER_BUTTON_WIDTH,
                     STATUS_BAR_CLEARANCE + (TITLE_ROW_HEIGHT - HEADER_BUTTON_WIDTH) / 2);
        /* Files reserves its title before the sort button, left of Search. */
    }

    /* Cover the full title row at the active display width, including any
     * title or action beneath it. */
    lv_obj_t * bar = lv_obj_create(screen);
    lv_obj_set_size(bar, lv_pct(100), SEARCH_BAR_HEIGHT);
    lv_obj_set_pos(bar, 0, SEARCH_BAR_Y);
    lv_obj_add_style(bar, &style_theme_screen_bg, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_image_src(bar, asset_path("sub_back/bg_search.png"), 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    /* lv_obj_create() pulls in the default theme's "card" style, which sets
     * pad_all to a nonzero default -- every lv_obj_set_pos() below for this
     * bar's children is relative to its CONTENT area (inside that padding),
     * so left uncleared, close_btn (and the reparented textarea) land
     * further right/down than intended. Same root cause already documented
     * on volume_popup's own construction. */
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(bar, LV_OBJ_FLAG_CLICKABLE); /* absorbs taps so the covered back button/title beneath can't be hit through it */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t * close_btn = lv_image_create(bar);
    lv_image_set_src(close_btn, asset_path("sub_back/close.png"));
    lv_obj_set_size(close_btn, SEARCH_BAR_CLOSE_SIZE, SEARCH_BAR_CLOSE_SIZE);
    lv_image_set_inner_align(close_btn, LV_IMAGE_ALIGN_CONTAIN);
    lv_obj_align(close_btn, LV_ALIGN_RIGHT_MID, -SEARCH_BAR_EDGE_INSET, 0);
    lv_obj_add_flag(close_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(close_btn, BOARD_SCALE_PX(6));
    lv_obj_add_event_cb(close_btn, search_close_btn_click_cb, LV_EVENT_CLICKED, (void *) (intptr_t) id);

    /* Same reasoning as search_btn above -- bar and its children were all
     * added after finalize_screen_navigation()'s one-time recursive pass,
     * so a swipe starting on any of them (the bar covers the whole title
     * row) needs this set explicitly to bubble up to
     * screen_gesture_event_cb() at all. */
    lv_obj_add_flag(bar, LV_OBJ_FLAG_GESTURE_BUBBLE);
    enable_gesture_bubble_recursive(bar);

    *b = (search_binding_t){
        .id = id, .screen = screen, .list = list, .search_btn = search_btn, .search_bar = bar,
        .name_of = config->name_of, .count_ptr = config->count_ptr,
        .is_overlay_list = config->is_overlay_list, .db_backed = config->db_backed,
        .db_kind = config->db_kind, .restore_fetch_page = config->restore_fetch_page,
        .files_title = config->files_title, .files_options = config->files_options,
        .files_options_wanted = config->files_options_wanted,
        .inset_search_beside_options = config->inset_search_beside_options,
        .restore_thumbnail_list = config->restore_thumbnail_list, .group_item = config->group_item,
        .az_strip_for_screen = config->az_strip_for_screen,
        .refresh_now_playing = config->refresh_now_playing,
        .global_click = config->global_click
    };
}

void library_search_open(search_binding_id_t id) {
    search_binding_t * b = &search_bindings[id];
    if (!b->screen || !b->search_btn) return;
    if (lv_screen_active() != b->screen) nav_push(b->screen);
    search_open(b);
}

void library_search_set_restore_thumbnail_list(search_binding_id_t id, lv_obj_t *list) {
    if (id < 0 || id >= SEARCH_BINDING_COUNT) return;
    search_bindings[id].restore_thumbnail_list = list;
}

void library_search_global_select(int display_index, bool long_press) {
    search_binding_t * b = &search_bindings[SEARCH_BINDING_GLOBAL];
    if (!b->active || !b->global_click || display_index < 0 || display_index >= b->filtered_count) return;
    int source = b->filtered_indices ? b->filtered_indices[display_index] : display_index;
    if (source < 0 || source >= b->filtered_count || !b->global_results) return;
    library_global_result_kind_t kind = b->global_results[source].kind;
    if (kind >= LIBRARY_GLOBAL_ARTIST && kind <= LIBRARY_GLOBAL_PLAYLIST) {
        int32_t generation = 0;
        if (!metadata_db_try_get_generation(&generation) || generation != b->global_generation) return;
    }
    if (kind >= LIBRARY_GLOBAL_SERVER_ARTIST && kind <= LIBRARY_GLOBAL_SERVER_SONG) {
        subsonic_server_t active_server = subsonic_server_from_settings();
        char active_key[SUBSONIC_CACHE_KEY_SIZE];
        subsonic_cache_server_key(&active_server, active_key);
        if (!gui_subsonic_session_active() || !gui_shell_wifi_connected() ||
            !b->global_server_key[0] || strcmp(active_key, b->global_server_key) != 0) return;
    }
    if (kind != LIBRARY_GLOBAL_HEADING)
        b->global_click(&b->global_results[source], long_press);
}


void library_search_set_item_of(search_binding_id_t id, library_search_item_of_t item_of) {
    search_bindings[id].item_of = item_of;
}

bool library_search_try_map_filtered_row(search_binding_id_t id, int display_index, int *source_index) {
    search_binding_t *b = &search_bindings[id];
    if (!b->filtered_indices || display_index < 0 || display_index >= b->filtered_count) return false;
    if (source_index) *source_index = b->filtered_indices[display_index];
    return true;
}

bool library_search_is_active(search_binding_id_t id) { return search_bindings[id].active; }

bool library_search_is_active_on_list(lv_obj_t *list) {
    for (int i = 0; i < SEARCH_BINDING_COUNT; i++)
        if (search_bindings[i].list == list && search_bindings[i].active) return true;
    return false;
}

bool library_search_has_active_job(void) { return search_job_active; }

void library_search_quiesce_for_ui_reload(void) {
    if (search_debounce_timer) lv_timer_pause(search_debounce_timer);
    search_job_pending_valid = false;
    search_job_finish(true);
    atomic_store_explicit(&search_job_done_flag, false, memory_order_relaxed);
    search_job_for_binding = NULL;
    search_job_pending_binding = NULL;
}

void library_search_join_for_shutdown(void) {
    search_job_pending_valid = false;
    search_job_finish(true);
}

lv_obj_t * library_search_button(search_binding_id_t id) {
    return search_bindings[id].search_btn;
}

void library_search_update_header(search_binding_id_t id, lv_obj_t *title,
                                  lv_obj_t *preferred_action, bool editing) {
    search_binding_t *b = &search_bindings[id];
    if (!b->search_btn) return;
    lv_obj_t *action = preferred_action ? preferred_action : b->search_btn;
    if (title) reserve_title_width_before(title, action);
    if (editing) {
        lv_obj_add_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
        if (b->search_bar) lv_obj_add_flag(b->search_bar, LV_OBJ_FLAG_HIDDEN);
    } else if (!b->active) {
        lv_obj_remove_flag(b->search_btn, LV_OBJ_FLAG_HIDDEN);
    }
}
