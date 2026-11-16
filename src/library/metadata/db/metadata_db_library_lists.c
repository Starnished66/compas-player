#include "metadata_db_internal.h"
#include "song_rating.h"

#include "../../browser/paths/path_cache.h"
#include "../../remote/remote_state.h"
#include "../../remote/subsonic/subsonic_saved_servers.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Frees the first n strings of a path list and the list itself. */
static void free_paths(char ** paths, int n) {
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}

typedef struct {
    int32_t id;
    int32_t count;
    int32_t last_played;
    int32_t ordinal;
} played_hit_t;

static int cmp_hit_played(const void * a, const void * b) {
    const played_hit_t * ha = a;
    const played_hit_t * hb = b;
    if (ha->count != hb->count) return ha->count < hb->count ? 1 : -1;
    if (ha->last_played != hb->last_played) return ha->last_played < hb->last_played ? 1 : -1;
    /* Equal play counts and timestamps are ordered by ascending stable song ID. */
    if (ha->ordinal != hb->ordinal) return ha->ordinal < hb->ordinal ? -1 : 1;
    return 0;
}

/* Recently Played order: newest play first, then ascending stable song ID. */
static int cmp_hit_recent(const void * a, const void * b) {
    const played_hit_t * ha = a;
    const played_hit_t * hb = b;
    if (ha->last_played != hb->last_played) return ha->last_played < hb->last_played ? 1 : -1;
    if (ha->ordinal != hb->ordinal) return ha->ordinal < hb->ordinal ? -1 : 1;
    return 0;
}

typedef int (*played_hit_cmp_t)(const void *, const void *);

/* Max-heap on cmp: the root is the hit that sorts last, so a full heap of
 * `limit` hits keeps the best `limit` seen so far. */
static void played_hit_heap_sift_up(played_hit_t * heap, int index, played_hit_cmp_t cmp) {
    while (index > 0) {
        int parent = (index - 1) / 2;
        if (cmp(&heap[parent], &heap[index]) >= 0) break;
        played_hit_t swap = heap[parent];
        heap[parent] = heap[index];
        heap[index] = swap;
        index = parent;
    }
}

static void played_hit_heap_sift_down(played_hit_t * heap, int count, int index, played_hit_cmp_t cmp) {
    for (;;) {
        if (count < 2 || index > (count - 2) / 2) return;
        int child = index * 2 + 1;
        if (child + 1 < count && cmp(&heap[child], &heap[child + 1]) < 0) child++;
        if (cmp(&heap[index], &heap[child]) >= 0) return;
        played_hit_t swap = heap[index];
        heap[index] = heap[child];
        heap[child] = swap;
        index = child;
    }
}

/* Caller holds METADATA_DB_GUARD. Streams every slot once and keeps the best
 * `limit` played songs by cmp in a bounded heap. recent selects songs with a
 * last-played stamp, otherwise songs with a play count. On success *out_hits
 * is sorted by cmp (NULL when none), and *out_total counts every matching
 * song, not just the kept ones. Returns false on allocation failure. */
static bool collect_played_hits(int limit, bool recent, played_hit_t ** out_hits, int * out_count,
                                int * out_total) {
    played_hit_cmp_t cmp = recent ? cmp_hit_recent : cmp_hit_played;
    *out_hits = NULL;
    *out_count = 0;
    if (out_total) *out_total = 0;
    int32_t slots = tagcache_slot_count();
    played_hit_t * hits = NULL;
    int n = 0, cap = 0, total = 0;
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(i + 1, 0, &song)) continue;
        if (recent ? song.last_played <= 0 : song.playcount <= 0) continue;
        if (total < INT_MAX) total++;
        played_hit_t candidate = {
            .id = song.id,
            .count = song.playcount,
            .last_played = song.last_played,
            .ordinal = i,
        };
        if (n < limit) {
            if (n >= cap) {
                int next = cap ? (cap > limit / 2 ? limit : cap * 2) : 16;
                if (next > limit) next = limit;
                if ((size_t) next > SIZE_MAX / sizeof(*hits)) {
                    free(hits);
                    return false;
                }
                played_hit_t * h = realloc(hits, sizeof(*h) * (size_t) next);
                if (!h) {
                    free(hits);
                    return false;
                }
                hits = h;
                cap = next;
            }
            hits[n] = candidate;
            played_hit_heap_sift_up(hits, n, cmp);
            n++;
        } else if (cmp(&candidate, &hits[0]) < 0) {
            hits[0] = candidate;
            played_hit_heap_sift_down(hits, n, 0, cmp);
        }
    }
    if (n > 0) qsort(hits, (size_t) n, sizeof(*hits), cmp);
    else {
        free(hits);
        hits = NULL;
    }
    *out_hits = hits;
    *out_count = n;
    if (out_total) *out_total = total;
    return true;
}

/* Caller holds METADATA_DB_GUARD. Copies each hit's path, all or nothing. */
static void played_hits_to_paths(const played_hit_t * hits, int n, char *** out_paths, int * out_count) {
    if (n <= 0) return;
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    if (!paths) return;
    int w = 0;
    for (int i = 0; i < n; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(hits[i].id, TAGCACHE_FIELD_PATH, &song)) break;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        w++;
    }
    if (w != n) {
        free_paths(paths, w);
        return;
    }
    *out_paths = paths;
    *out_count = w;
}

int metadata_db_song_rating_get(const char * path) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !path) return 0;
    tagcache_song_t song;
    if (tagcache_song_by_path(path, &song)) return song_rating_stars(song.rating);
    int32_t rating = 0;
    if (!remote_state_get(path, &rating, NULL, NULL)) return 0;
    return song_rating_stars(rating);
}

void metadata_db_song_rating_set(const char * path, int stars) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !path) return;
    if (tagcache_song_by_path(path, NULL)) {
        tagcache_set_rating(path, song_rating_stored(stars));
        remote_state_note_rating_change();
    }
    else remote_state_set_rating(path, song_rating_stored(stars));
}

/* How long a confirmed rating write waits for the card. */
#define RATING_DURABLE_TIMEOUT_MS 10000

bool metadata_db_song_rating_set_durable(const char * path, int stars) {
    int waiter;
    {
        METADATA_DB_GUARD;
        if (!metadata_db_db_ready || !path) return false;
        /* The sidecar stays under the lock, as in metadata_db_song_rating_set(),
         * so a concurrent scan cannot migrate the row between check and write. */
        if (!tagcache_song_by_path(path, NULL)) return remote_state_set_rating(path, song_rating_stored(stars));
        waiter = tagcache_set_rating_durable_begin(path, song_rating_stored(stars));
        if (waiter) remote_state_note_rating_change();
    }
    /* The flush and fdatasync wait happens outside the database lock, which
     * the UI thread takes every tick. */
    return waiter && tagcache_numeric_wait_durable(waiter, RATING_DURABLE_TIMEOUT_MS);
}

typedef struct {
    char * path;
    int stars;
} rated_path_t;

static int cmp_rated_path(const void * a, const void * b) {
    const rated_path_t * pa = a;
    const rated_path_t * pb = b;
    if (pa->stars != pb->stars) return pb->stars - pa->stars;
    return tagcache_cmp_ascii(pa->path, pb->path);
}

/* Caller holds METADATA_DB_GUARD. Legacy stored 1 decodes to 5 stars. */
static bool collect_rated_paths(rated_path_t ** hits, int * count) {
    int cap = 0;
    int32_t slots = tagcache_slot_count();
    for (int32_t i = 0; i < slots; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_by_id(i + 1, 0, &song)) continue;
        if (song_rating_stars(song.rating) < 3) continue;
        if (*count >= cap) {
            if (cap > INT_MAX / 2) return false;
            int next = cap ? cap * 2 : 16;
            if ((size_t) next > SIZE_MAX / sizeof(**hits)) return false;
            rated_path_t * grown = realloc(*hits, sizeof(*grown) * (size_t) next);
            if (!grown) return false;
            *hits = grown;
            cap = next;
        }
        if (!tagcache_song_fields_by_id(i + 1, TAGCACHE_FIELD_PATH, &song)) return false;
        char * path = strdup(song.path);
        if (!path) return false;
        (*hits)[(*count)++] = (rated_path_t) { path, song_rating_stars(song.rating) };
    }
    return true;
}

void metadata_db_load_favorite_songs(char *** out_paths, int * out_count, int ** out_stars) {
    if (out_stars) *out_stars = NULL;
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!metadata_db_db_ready) return;
    rated_path_t * hits = NULL;
    int n = 0;
    bool ok = collect_rated_paths(&hits, &n);
    char ** paths = ok && n > 0 ? malloc(sizeof(*paths) * (size_t) n) : NULL;
    int * stars = out_stars && paths ? malloc(sizeof(*stars) * (size_t)n) : NULL;
    if (out_stars && !stars) { free(paths); paths = NULL; }
    if (paths) {
        qsort(hits, (size_t) n, sizeof(*hits), cmp_rated_path);
        for (int i = 0; i < n; i++) {
            paths[i] = hits[i].path;
            if (stars) stars[i] = hits[i].stars;
        }
        if (out_stars) *out_stars = stars;
        *out_paths = paths;
        *out_count = n;
    } else {
        for (int i = 0; i < n; i++) free(hits[i].path);
    }
    free(hits);
}

void metadata_db_song_play_count_increment(const char * path) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !path) return;
    int32_t now = (int32_t) time(NULL);
    if (tagcache_song_by_path(path, NULL)) tagcache_add_play(path, now);
    else remote_state_add_play(path, now);
}

/* Caller holds METADATA_DB_GUARD. Paths of the best `limit` songs by play
 * count, or by last-played time when recent. */
static void load_played_paths(int limit, bool recent, char *** out_paths, int * out_count) {
    *out_paths = NULL;
    *out_count = 0;
    if (!metadata_db_db_ready || limit <= 0) return;
    played_hit_t * hits = NULL;
    int n = 0;
    if (!collect_played_hits(limit, recent, &hits, &n, NULL)) return;
    played_hits_to_paths(hits, n, out_paths, out_count);
    free(hits);
}

void metadata_db_load_top_played_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    load_played_paths(limit, false, out_paths, out_count);
}

void metadata_db_load_recently_played_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    if (limit > METADATA_DB_RECENTLY_PLAYED_MAX) limit = METADATA_DB_RECENTLY_PLAYED_MAX;
    load_played_paths(limit, true, out_paths, out_count);
}

int metadata_db_get_recently_played_page(int offset, int max_rows, song_row_t * out_rows,
                                         int64_t * out_last_played, int * out_total) {
    METADATA_DB_GUARD;
    if (out_total) *out_total = 0;
    if (!metadata_db_db_ready || offset < 0 || max_rows <= 0 || !out_rows) return -1;
    if (offset >= METADATA_DB_RECENTLY_PLAYED_MAX) offset = METADATA_DB_RECENTLY_PLAYED_MAX;
    int keep = METADATA_DB_RECENTLY_PLAYED_MAX - offset < max_rows ? METADATA_DB_RECENTLY_PLAYED_MAX
                                                                   : offset + max_rows;
    played_hit_t * hits = NULL;
    int n = 0, total = 0;
    if (!collect_played_hits(keep, true, &hits, &n, &total)) return -1;
    if (out_total) *out_total = total < METADATA_DB_RECENTLY_PLAYED_MAX ? total : METADATA_DB_RECENTLY_PLAYED_MAX;
    int w = 0;
    for (int i = offset; i < n && w < max_rows; i++) {
        tagcache_song_t song;
        if (!tagcache_song_by_id(hits[i].id, &song)) {
            free(hits);
            return -1;
        }
        metadata_db_copy_song(&song, &out_rows[w]);
        if (out_last_played) out_last_played[w] = hits[i].last_played;
        w++;
    }
    free(hits);
    return w;
}

void metadata_db_load_recently_added_songs(int limit, char *** out_paths, int * out_count) {
    METADATA_DB_GUARD;
    *out_paths = NULL;
    *out_count = 0;
    if (!metadata_db_db_ready || limit <= 0) return;
    int32_t live = tagcache_live_count();
    if (live <= 0) return;
    int n = live < limit ? (int) live : limit;
    char ** paths = malloc(sizeof(*paths) * (size_t) n);
    if (!paths) return;
    int w = 0;
    for (int32_t i = 0; i < live && w < n; i++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_at_recency_rank(i, TAGCACHE_FIELD_PATH, &song)) continue;
        paths[w] = strdup(song.path);
        if (!paths[w]) break;
        w++;
    }
    if (w != n) {
        free_paths(paths, w);
        return;
    }
    *out_paths = paths;
    *out_count = w;
}

bool metadata_db_book_favorite_is_set(const char * path) {
    return path && path[0] && path_cache_has(PATH_CACHE_BOOK_FAVORITES, path);
}

void metadata_db_book_favorite_set(const char * path, bool is_favorite) {
    if (!path || !path[0]) return;
    if (is_favorite) path_cache_insert(PATH_CACHE_BOOK_FAVORITES, path);
    else path_cache_delete(PATH_CACHE_BOOK_FAVORITES, path);
}

void metadata_db_book_replace_all(char * const * paths, int count) {
    path_cache_replace(PATH_CACHE_BOOKS, paths, count);
}

void metadata_db_load_favorite_books(char *** out_paths, int * out_count) {
    path_cache_load_matching(PATH_CACHE_BOOKS, PATH_CACHE_BOOK_FAVORITES, out_paths, out_count);
}

void metadata_db_load_all_books(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_BOOKS, out_paths, out_count);
}

void metadata_db_load_all_playlists(char *** out_paths, int * out_count) {
    path_cache_load(PATH_CACHE_PLAYLISTS, out_paths, out_count);
}

void metadata_db_playlist_insert_one(const char * path) {
    if (!path || !path[0]) return;
    path_cache_insert(PATH_CACHE_PLAYLISTS, path);
}

void metadata_db_playlist_delete_one(const char * path) {
    if (!path || !path[0]) return;
    path_cache_delete(PATH_CACHE_PLAYLISTS, path);
}

void metadata_db_subsonic_server_save(const char * url, const char * username, const char * password, bool verify_tls) {
    subsonic_saved_servers_upsert(url, username, password, verify_tls);
}

void metadata_db_load_subsonic_servers(subsonic_server_row_t ** out_rows, int * out_count) {
    *out_rows = NULL;
    *out_count = 0;
    subsonic_saved_server_t * rows = NULL;
    int count = 0;
    subsonic_saved_servers_load(&rows, &count);
    if (count <= 0) {
        free(rows);
        return;
    }
    /* subsonic_saved_server_t and subsonic_server_row_t are separately
     * declared (subsonic_saved_servers.h has no dependency on
     * metadata_db.h, matching path_cache.h/remote_state.h's own
     * layering) but field-for-field identical -- copy rather than cast,
     * so a future divergence between the two is a compile error in this
     * one place, not a silent mismatch. */
    subsonic_server_row_t * out = malloc(sizeof(*out) * (size_t) count);
    if (!out) {
        free(rows);
        return;
    }
    for (int i = 0; i < count; i++) {
        snprintf(out[i].url, sizeof(out[i].url), "%s", rows[i].url);
        snprintf(out[i].username, sizeof(out[i].username), "%s", rows[i].username);
        snprintf(out[i].password, sizeof(out[i].password), "%s", rows[i].password);
        out[i].verify_tls = rows[i].verify_tls;
    }
    free(rows);
    *out_rows = out;
    *out_count = count;
}

/* ---- One-pass walks for the duration cache and the artist page ----------- */
bool metadata_db_get_song_identity(const char * path, int32_t * out_mtime, int32_t * out_size) {
    if (!path || !out_mtime || !out_size) return false;
    METADATA_DB_GUARD;
    tagcache_song_t song;
    if (!metadata_db_db_ready || !tagcache_song_by_path(path, &song)) return false;
    *out_mtime = song.mtime;
    *out_size = song.size;
    return true;
}

/* Each batch of paths is copied under the database lock and visited after it is
 * released (as walk_group_songs does): the visitor's allocations and hashing,
 * and a low-priority caller being preempted, never hold up the UI's own queries. */
#define METADATA_DB_WALK_IDENTITY_BATCH 32
bool metadata_db_walk_song_identities(bool (*visit)(const char * path, int32_t mtime, int32_t size, void * user),
                                      void * user) {
    if (!visit) return false;
    int32_t slots, generation;
    uint32_t incarnation;
    {
        METADATA_DB_GUARD;
        if (!metadata_db_db_ready) return false;
        slots = tagcache_slot_count();
        generation = tagcache_generation();
        incarnation = metadata_db_db_incarnation;
    }
    char (*paths)[PATH_MAX] = malloc(sizeof(*paths) * METADATA_DB_WALK_IDENTITY_BATCH);
    if (!paths) return false;
    int32_t mtimes[METADATA_DB_WALK_IDENTITY_BATCH], sizes[METADATA_DB_WALK_IDENTITY_BATCH];
    bool ok = true;
    for (int32_t first = 1; first <= slots && ok; first += METADATA_DB_WALK_IDENTITY_BATCH) {
        int copied = 0;
        {
            METADATA_DB_GUARD;
            if (!metadata_db_db_ready || tagcache_generation() != generation ||
                metadata_db_db_incarnation != incarnation || !tagcache_storage_current()) { ok = false; break; }
            for (int32_t id = first; id < first + METADATA_DB_WALK_IDENTITY_BATCH && id <= slots; id++) {
                tagcache_song_t song;
                if (!tagcache_song_fields_by_id(id, TAGCACHE_FIELD_PATH, &song) || !song.path[0]) continue;
                strcpy(paths[copied], song.path);
                mtimes[copied] = song.mtime;
                sizes[copied] = song.size;
                copied++;
            }
        }
        for (int i = 0; i < copied && ok; i++) ok = visit(paths[i], mtimes[i], sizes[i], user);
    }
    free(paths);
    return ok;
}

/* Ranks read per guard hold, so UI page queries never wait for the whole scan. */
#define RATING_ORDER_BATCH 64

static bool rating_bucket_page(uint8_t *stars, int first, int end, int sections[6],
                                int32_t generation) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || tagcache_generation() != generation) return false;
    for (int rank = first; rank < end; rank++) {
        tagcache_song_t song;
        if (!tagcache_song_fields_at_title_rank(rank, 0, &song)) return false;
        stars[rank] = song_rating_stars(song.rating);
        sections[stars[rank]]++;
    }
    return true;
}

bool metadata_db_try_rating_order_available(void) {
    if (!metadata_db_query_try_lock()) return false;
    bool available = metadata_db_db_ready && tagcache_title_ranks_indexed();
    metadata_db_query_unlock();
    return available;
}

static bool rating_order_size(int *count, int32_t *generation) {
    METADATA_DB_GUARD;
    if (!metadata_db_db_ready || !tagcache_title_ranks_indexed()) return false;
    *count = tagcache_live_count();
    *generation = tagcache_generation();
    return true;
}

static void rating_ranks(const uint8_t *stars, int count, const int sections[6], int *ranks) {
    int positions[6], offset = 0;
    for (int s = 5; s >= 0; s--) {
        positions[s] = offset;
        offset += sections[s];
    }
    for (int rank = 0; rank < count; rank++) ranks[positions[stars[rank]]++] = rank;
}

bool metadata_db_load_rating_order(int **out_ranks, int out_sections[6],
                                   int *out_count, int32_t *out_generation) {
    *out_ranks = NULL;
    *out_count = 0;
    memset(out_sections, 0, sizeof(int) * 6);
    int count;
    if (!rating_order_size(&count, out_generation)) return false;
    if (count <= 0) return true;
    int *ranks = malloc(sizeof(*ranks) * (size_t)count);
    uint8_t *stars = malloc((size_t)count);
    bool valid = ranks && stars;
    for (int first = 0; valid && first < count; first += RATING_ORDER_BATCH) {
        int end = count - first > RATING_ORDER_BATCH ? first + RATING_ORDER_BATCH : count;
        valid = rating_bucket_page(stars, first, end, out_sections, *out_generation);
    }
    if (!valid) {
        free(ranks);
        free(stars);
        memset(out_sections, 0, sizeof(int) * 6);
        return false;
    }
    rating_ranks(stars, count, out_sections, ranks);
    free(stars);
    *out_ranks = ranks;
    *out_count = count;
    return true;
}
